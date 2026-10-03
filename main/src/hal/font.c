/**
 * font.c —— EDC Book / M5ReadPaper 系 bin 字体解析（NFR-5）
 *
 * 已对照 M5ReadPaper 源码与真实字体分区数据逐条核实，存在两代磁盘格式：
 *
 * 新格式（version==2，M5ReadPaper tools/generate_1bit_font_bin.py 生成）：
 *   [0]   char_count u32 / font_height u8 / version u8 (=2)
 *   [6]   family_name 64B + style_name 64B → 头共 134B
 *   [134] 索引区 char_count × 20B（不是 16B！尾部 4B 是生成端 cached_bitmap
 *         占位，依据 bin_font_print.cpp:1917-1922 解析步进与
 *         generate_1bit_font_bin.py:878 的 '<HHBBbbIII' 打包）
 *   位图：1-bit 打包，行 MSB-first，生成端写盘前对行字节取反，
 *         故存储位 1=白底、0=墨（font_decoder.cpp decode_bitmap_1bit:30-31）
 *
 * 旧格式（EDC Book 原始格式，tools/font_partition.bin 与 PaperS3 真机 flash
 * 0x510000 处实测均为此格式；头部只有 5 字节、没有 version 字段，读到的
 * "version" 其实是首索引项 unicode 低字节=0）：
 *   [0] char_count u32 / font_height u8
 *   [5] 索引区 char_count × 20B（同上新格式）
 *   位图：Huffman 灰度位流，MSB-first，逐像素：
 *         '0' → 白(15)；'10' → 黑(0)；'11'+4bit g → 灰 15-g
 *         （生成端 bin_font_generator.py.ref:142-160；
 *          解码端 font_decoder.cpp decode_bitmap:69-146）
 * 判别规则与 M5ReadPaper 一致：version>=2 按新格式 134B 头，否则按旧格式
 * 5B 头（bin_font_print.cpp:1737-1750）。version==3 为 2bit Huffman，
 * 本渲染器不支持，拒绝加载（上层按 NFR-5 降级不显示）。
 *
 * 两版通用事实（已核实）：
 * - bitmap_offset 为自文件头起的绝对偏移（bin_font_print.cpp
 *   load_glyph_bitmap_smart 直接 seek(offset)，bin_font_print.cpp:1138-1156）
 * - 绘制原点：行盒左上角 (x,y)，字形落在 (x+x_offset, y+y_offset)，
 *   y_offset = ascender - bitmap_top + 裁剪上边距，即"行顶到位图顶"距离
 *   （bin_font_print.cpp:3414-3415 canvas_x/canvas_y；生成端
 *    bin_font_generator.py.ref:172-173）
 *
 * 解析逻辑已用 tools/font_partition.bin 实测验证：43511 字 30px，
 * 索引升序、'一'(U+4E00)/'墨'(U+58A8)/'A' Huffman 解码出正确字形。
 *
 * 内存纪律：索引载入 PSRAM（43511 字 ×20B ≈ 850KB）；位图按需读，
 * 不整载 10MB 分区；栈上无 >4KB 缓冲。
 */
#include "font.h"
#include "../log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

#define TAG "moka.font"

#define FONT_HDR_SIZE_NEW   134     /* v2/v3：6B 基础头 + 64B 族名 + 64B 样式名 */
#define FONT_HDR_SIZE_OLD   5       /* 旧 EDC 格式：4B 字数 + 1B 字高，无版本字段 */
#define FONT_INDEX_ENT      20      /* 磁盘索引项 20B（含 4B cached_bitmap 占位） */
#define FONT_MAX_CHARS      200000  /* 头校验用上限（格式无 magic，靠合理性检查） */

#define FONT_VER_1BIT       2       /* 新格式：1-bit 打包位图 */
#define FONT_VER_HUFFMAN3   3       /* V3：2bit Huffman 灰度（不支持） */

typedef enum {
    FONT_FMT_HUFFMAN_OLD = 0,   /* 旧 EDC：5B 头 + Huffman 灰度位图 */
    FONT_FMT_1BIT,              /* v2：134B 头 + 1-bit 位图 */
} font_format_t;

/* 20B 磁盘索引项（自然对齐恰好无填充，布局即文件布局；末 4B 恒 0） */
typedef struct {
    uint16_t unicode;        /* 仅 BMP */
    uint16_t width;          /* advance */
    uint8_t  bitmap_w;
    uint8_t  bitmap_h;
    int8_t   x_offset;
    int8_t   y_offset;
    uint32_t bitmap_offset;  /* 自文件头起的绝对偏移（已核实） */
    uint32_t bitmap_size;
    uint32_t reserved;       /* 生成端 cached_bitmap 占位 */
} glyph_index_t;

_Static_assert(sizeof(glyph_index_t) == FONT_INDEX_ENT, "glyph_index_t 必须为 20 字节");

struct moka_font {
    const esp_partition_t *part;   /* 分区来源（与 fp 互斥） */
    FILE                *fp;       /* SD 卡来源 */
    size_t    data_size;           /* 分区/文件总字节数（位图越界保护） */
    uint32_t  char_count;
    uint8_t   height;
    uint8_t   version;             /* 旧格式恒 0（无版本字段） */
    int8_t    y_shift;             /* 基线校正：行盒顶 padding（绘制时统一上提） */
    font_format_t format;
    glyph_index_t *index;          /* PSRAM，char_count 项 */
    uint8_t  *bitmap_buf;          /* 单字形位图暂存（按需扩容） */
    size_t    bitmap_buf_cap;
    uint8_t  *gray_buf;            /* 单字形灰度缓冲（平滑放大解码用，按需扩容） */
    size_t    gray_buf_cap;
};

/* ---------- 底层读 ---------- */

static bool font_read(const moka_font_t *f, uint32_t offset, void *dst, size_t len)
{
    if (f->part) {
        return esp_partition_read(f->part, offset, dst, len) == ESP_OK;
    }
    if (f->fp) {
        if (fseek(f->fp, (long)offset, SEEK_SET) != 0) return false;
        return fread(dst, 1, len, f->fp) == len;
    }
    return false;
}

/* ---------- 加载 ---------- */

/* 读公共 6B 头并判定格式/头部大小（本格式无 magic，用合理性检查代替） */
static bool font_load_header(moka_font_t *f)
{
    uint8_t hdr[6];
    if (!font_read(f, 0, hdr, sizeof(hdr))) {
        MOKA_LOGE(TAG, "读字体头失败");
        return false;
    }
    uint32_t count = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                     ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    uint8_t height  = hdr[4];
    uint8_t version = hdr[5];

    if (count == 0 || count == 0xFFFFFFFF || count > FONT_MAX_CHARS) {
        MOKA_LOGE(TAG, "字形数非法 0x%08lx（分区可能未烧录字库）",
                  (unsigned long)count);
        return false;
    }
    if (height == 0 || height == 0xFF || height > 128) {
        MOKA_LOGE(TAG, "字高非法 %d", height);
        return false;
    }
    if (version == FONT_VER_HUFFMAN3) {
        MOKA_LOGE(TAG, "版本 v3 为 2bit Huffman 编码，本渲染器不支持");
        return false;
    }
    if (version == FONT_VER_1BIT) {
        f->format  = FONT_FMT_1BIT;
        f->version = 2;
    } else {
        /* 旧格式没有版本字段：hdr[5] 实为索引首项 unicode 低字节（=0），
         * version<2 一律按旧 EDC Huffman 格式解析（与 M5ReadPaper 一致） */
        f->format  = FONT_FMT_HUFFMAN_OLD;
        f->version = 0;
    }
    f->char_count = count;
    f->height     = height;
    MOKA_LOGI(TAG, "字体头 OK：%lu 字，%dpx，格式=%s",
              (unsigned long)count, height,
              f->format == FONT_FMT_1BIT ? "v2 1-bit" : "旧EDC Huffman");
    return true;
}

/* 索引整体载入 PSRAM（大对象）；PSRAM 不足时回退内部 RAM 并告警 */
static bool font_load_index(moka_font_t *f)
{
    size_t hdr = (f->format == FONT_FMT_1BIT) ? FONT_HDR_SIZE_NEW : FONT_HDR_SIZE_OLD;
    size_t bytes = (size_t)f->char_count * FONT_INDEX_ENT;
    if (hdr + bytes > f->data_size) {
        MOKA_LOGE(TAG, "索引区越界（头 %u + 索引 %u > 数据 %u B），字库损坏",
                  (unsigned)hdr, (unsigned)bytes, (unsigned)f->data_size);
        return false;
    }
    f->index = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!f->index) {
        MOKA_LOGE(TAG, "PSRAM 不足，索引回退内部 RAM（%u B）", (unsigned)bytes);
        f->index = malloc(bytes);
    }
    if (!f->index) {
        MOKA_LOGE(TAG, "索引分配失败（%u B）", (unsigned)bytes);
        return false;
    }
    if (!font_read(f, hdr, f->index, bytes)) {
        MOKA_LOGE(TAG, "索引读取失败");
        return false;
    }
    /* 二分查找依赖升序；抽查首尾 */
    if (f->char_count > 1 &&
        f->index[0].unicode > f->index[f->char_count - 1].unicode) {
        MOKA_LOGE(TAG, "索引未按 unicode 升序，二分查找不可用");
        return false;
    }
    MOKA_LOGI(TAG, "索引载入 %s：%u B",
              esp_ptr_external_ram(f->index) ? "PSRAM" : "内部RAM",
              (unsigned)bytes);
    return true;
}

/* 基线校正：有的字库行盒顶部留了大片空白（如汉仪空山楷 CJK y_offset≈10~14px，
 * 旧 EDC 字库仅 3~4px），直接按行顶绘制会整行偏下。以参考字形的 y_offset
 * 作为行盒顶 padding，绘制时统一上提，使"行顶=视觉顶"。
 * 参考字取"永"（书法八法代表字，字库几乎必有），退路 墨/M。 */
static const glyph_index_t *find_glyph(const moka_font_t *f, uint16_t cp);

static void font_calib_y_shift(moka_font_t *f)
{
    static const uint16_t REFS[] = { 0x6C38, 0x58A8, 0x004D };  /* 永 / 墨 / M */
    f->y_shift = 0;
    for (size_t i = 0; i < sizeof(REFS) / sizeof(REFS[0]); i++) {
        const glyph_index_t *g = find_glyph(f, REFS[i]);
        if (g && g->bitmap_size > 0) {
            int s = g->y_offset;
            if (s < 0) s = 0;
            if (s > f->height / 2) s = f->height / 2;   /* 防御异常字库 */
            f->y_shift = (int8_t)s;
            MOKA_LOGI(TAG, "基线校正 y_shift=%d（参考字 U+%04X y_offset=%d）",
                      (int)f->y_shift, REFS[i], (int)g->y_offset);
            return;
        }
    }
}

static moka_font_t *font_finish_load(moka_font_t *f)
{
    if (!font_load_header(f) || !font_load_index(f)) {
        font_close(f);
        return NULL;
    }
    font_calib_y_shift(f);
    return f;
}

moka_font_t *font_open_partition(const char *label)
{
    if (!label) label = "font";
    /* 分区表子类型 0x40（自定义字体分区） */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, label);
    if (!part) {
        /* 兼容：子类型不匹配时退化为仅按 label 找 */
        part = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    }
    if (!part) {
        MOKA_LOGE(TAG, "找不到字体分区 \"%s\"", label);
        return NULL;
    }
    moka_font_t *f = calloc(1, sizeof(moka_font_t));
    if (!f) return NULL;
    f->part = part;
    f->data_size = part->size;
    return font_finish_load(f);
}

moka_font_t *font_open_sd(const char *path)
{
    if (!path) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        MOKA_LOGE(TAG, "打不开字体文件 %s", path);
        return NULL;
    }
    moka_font_t *f = calloc(1, sizeof(moka_font_t));
    if (!f) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz <= 0) {
        MOKA_LOGE(TAG, "字体文件 %s 长度为 0", path);
        fclose(fp);
        free(f);
        return NULL;
    }
    f->fp = fp;
    f->data_size = (size_t)sz;
    return font_finish_load(f);
}

void font_close(moka_font_t *f)
{
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f->index);
    free(f->bitmap_buf);
    free(f->gray_buf);
    free(f);
}

int font_height(const moka_font_t *f)
{
    return f ? f->height : 0;
}

/* ---------- 查找与渲染 ---------- */

/* 二分查找（索引按 unicode 升序，已在加载时抽查） */
static const glyph_index_t *find_glyph(const moka_font_t *f, uint16_t cp)
{
    size_t lo = 0, hi = f->char_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint16_t u = f->index[mid].unicode;
        if (u == cp) return &f->index[mid];
        if (u < cp) lo = mid + 1;
        else        hi = mid;
    }
    return NULL;
}

/* UTF-8 解码一个码点并前移指针；非法序列按 1 字节跳过返回 U+FFFD */
static uint32_t utf8_next(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    uint32_t cp;
    if (p[0] < 0x80) {
        cp = p[0]; *pp += 1;
    } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F); *pp += 2;
    } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
             (p[2] & 0x3F); *pp += 3;
    } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
             ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); *pp += 4;
    } else {
        cp = 0xFFFD; *pp += 1;
    }
    return cp;
}

/* 写 4bpp 帧缓冲单像素（偶数 x 占高半字节），自带边界裁剪 */
static inline void fb_put_px(uint8_t *fb, int fbw, int fbh, int x, int y, uint8_t gray4)
{
    if ((unsigned)x >= (unsigned)fbw || (unsigned)y >= (unsigned)fbh) return;
    uint8_t *byte = &fb[(y * fbw + x) / 2];
    if (x & 1) *byte = (*byte & 0xF0) | (gray4 & 0x0F);
    else       *byte = (*byte & 0x0F) | ((gray4 & 0x0F) << 4);
}

/* 以 (x,y) 为左上写 scale×scale 像素块（最近邻放大用；scale==1 即单像素） */
static inline void fb_put_px_scaled(uint8_t *fb, int fbw, int fbh,
                                    int x, int y, uint8_t gray4, int scale)
{
    for (int dy = 0; dy < scale; dy++) {
        for (int dx = 0; dx < scale; dx++) {
            fb_put_px(fb, fbw, fbh, x + dx, y + dy, gray4);
        }
    }
}

/* v2 1-bit 位图展开（存储位 1=白底、0=墨，MSB-first；只写墨像素，底透明） */
static void blit_glyph_1bit(uint8_t *fb, int fbw, int fbh, int gx, int gy,
                            const glyph_index_t *g, const uint8_t *bitmap, int scale)
{
    int row_bytes = (g->bitmap_w + 7) / 8;
    if ((size_t)row_bytes * g->bitmap_h > g->bitmap_size) {
        return;   /* 位图长度与尺寸不符，丢弃该字形 */
    }
    for (int row = 0; row < g->bitmap_h; row++) {
        const uint8_t *rb = bitmap + (size_t)row * row_bytes;
        for (int col = 0; col < g->bitmap_w; col++) {
            int bit = (rb[col / 8] >> (7 - (col % 8))) & 1;   /* MSB-first */
            if (bit) continue;    /* 存储位 1=白底，跳过；0=墨 */
            fb_put_px_scaled(fb, fbw, fbh, gx + col * scale, gy + row * scale, 0x0, scale);
        }
    }
}

/* 旧 EDC Huffman 灰度位图展开（'0'=白15，'10'=黑0，'11'+4bit g=灰15-g；
 * 白像素跳过保持背景透明，其余按灰阶写入。位流耗尽即停，容错截断字库） */
static void blit_glyph_huffman(uint8_t *fb, int fbw, int fbh, int gx, int gy,
                               const glyph_index_t *g, const uint8_t *bitmap, int scale)
{
    uint32_t total = (uint32_t)g->bitmap_w * g->bitmap_h;
    uint32_t byte_pos = 0;
    uint8_t  bit_pos = 0;
    for (uint32_t i = 0; i < total && byte_pos < g->bitmap_size; ) {
        /* 位指针归一化必须放在每像素开头（与 font_decoder.cpp:84-91 一致），
         * 否则上一像素恰好读完字节边界时移位次数为负是 UB */
        if (bit_pos >= 8) {
            bit_pos = 0;
            byte_pos++;
            if (byte_pos >= g->bitmap_size) break;
        }
        uint8_t cur = bitmap[byte_pos];
        uint8_t bit = (cur >> (7 - bit_pos)) & 1;
        uint8_t gray;
        if (bit == 0) {
            gray = 0xF;
            bit_pos++;
        } else {
            bit_pos++;
            if (bit_pos >= 8) { bit_pos = 0; byte_pos++; if (byte_pos >= g->bitmap_size) break; cur = bitmap[byte_pos]; }
            bit = (cur >> (7 - bit_pos)) & 1;
            if (bit == 0) {
                gray = 0x0;
                bit_pos++;
            } else {
                uint8_t gv = 0;
                bit_pos++;
                for (int b = 0; b < 4; b++) {
                    if (bit_pos >= 8) { bit_pos = 0; byte_pos++; if (byte_pos >= g->bitmap_size) break; cur = bitmap[byte_pos]; }
                    if (byte_pos < g->bitmap_size) {
                        gv = (uint8_t)((gv << 1) | ((cur >> (7 - bit_pos)) & 1));
                        bit_pos++;
                    }
                }
                gray = (uint8_t)(0xF - gv);
            }
        }
        int row = (int)(i / g->bitmap_w);
        int col = (int)(i % g->bitmap_w);
        if (gray != 0xF) {
            fb_put_px_scaled(fb, fbw, fbh, gx + col * scale, gy + row * scale, gray, scale);
        }
        i++;
    }
}

/* 放大倍数上限：scale_x2 单位（16 = 8.0x，关机页用 12 = 6.0x），
 * 封顶防调用方传异常值刷爆 CPU */
#define FONT_MAX_SCALE  16

/* 取可绘制字形：位图越界（字库被 10MB 分区截尾，全角标点区首当其冲）时，
 * 全角字符（U+FF01~FF5E）回退到对应半角（cp-0xFEE0）——宁可显示半角
 * 标点也不让整句缺字（真机实测 ，；（）： 全落在截断区）。
 * 返回 NULL = 无可用字形（真缺字或半角也被截断）。 */
static const glyph_index_t *find_drawable_glyph(const moka_font_t *f, uint32_t cp)
{
    const glyph_index_t *g = find_glyph(f, (uint16_t)cp);
    if (g && (uint64_t)g->bitmap_offset + g->bitmap_size <= f->data_size) return g;
    if (cp >= 0xFF01 && cp <= 0xFF5E) {
        const glyph_index_t *h = find_glyph(f, (uint16_t)(cp - 0xFEE0));
        if (h && (uint64_t)h->bitmap_offset + h->bitmap_size <= f->data_size) return h;
    }
    return NULL;
}

static int draw_text_impl(moka_font_t *f, uint8_t *fb4bpp, int fb_width, int fb_height,
                          int x, int y, const char *utf8_text, int scale)
{
    if (!f || !fb4bpp || !utf8_text) return 0;
    int pen = x;
    int missing = 0;
    const char *p = utf8_text;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp > 0xFFFF) { missing++; continue; }   /* 索引仅存 BMP */
        const glyph_index_t *g = find_drawable_glyph(f, cp);
        if (!g) { missing++; continue; }            /* 缺字形跳过，不计失败（NFR-5） */

        if (g->bitmap_size > 0) {
            if (g->bitmap_size > f->bitmap_buf_cap) {
                uint8_t *nb = realloc(f->bitmap_buf, g->bitmap_size);
                if (!nb) {
                    MOKA_LOGE(TAG, "位图缓冲分配失败（%lu B）",
                              (unsigned long)g->bitmap_size);
                    pen += g->width * scale;
                    continue;
                }
                f->bitmap_buf     = nb;
                f->bitmap_buf_cap = g->bitmap_size;
            }
            if (font_read(f, g->bitmap_offset, f->bitmap_buf, g->bitmap_size)) {
                /* 绘制原点已核实：(x,y) 为行盒左上角，字形画在
                 * (pen+x_offset, y+y_offset)，y_offset 为行顶到位图顶距离
                 * （依据 bin_font_print.cpp:3414-3415）；放大时偏移同步乘 scale。
                 * y_shift 为字库级基线校正（行盒顶 padding，见 font_calib_y_shift） */
                if (f->format == FONT_FMT_1BIT) {
                    blit_glyph_1bit(fb4bpp, fb_width, fb_height,
                                    pen + g->x_offset * scale,
                                    y + (g->y_offset - f->y_shift) * scale,
                                    g, f->bitmap_buf, scale);
                } else {
                    blit_glyph_huffman(fb4bpp, fb_width, fb_height,
                                       pen + g->x_offset * scale,
                                       y + (g->y_offset - f->y_shift) * scale,
                                       g, f->bitmap_buf, scale);
                }
            } else {
                MOKA_LOGE(TAG, "字形 U+%04X 位图读取失败", (unsigned)cp);
            }
        }
        pen += g->width * scale;
    }
    if (missing > 0) {
        MOKA_LOGI(TAG, "跳过 %d 个缺字形（不计失败）", missing);
    }
    return pen - x;
}

int font_draw_text(moka_font_t *f, uint8_t *fb4bpp, int fb_width, int fb_height,
                   int x, int y, const char *utf8_text)
{
    return draw_text_impl(f, fb4bpp, fb_width, fb_height, x, y, utf8_text, 1);
}

/* ---------- 锐化双线性放大（插值消阶梯 + S 形曲线防发虚） ---------- */

/* 解码单字形到灰度缓冲 gray[bw*bh]（0x0F=白底，其余=墨阶）。
 * 与直绘路径共用同一套位流解码语义，只是落地到缓冲而非帧缓冲。 */
static void decode_glyph_gray(const moka_font_t *f, const glyph_index_t *g,
                              const uint8_t *bitmap, uint8_t *gray)
{
    int bw = g->bitmap_w, bh = g->bitmap_h;
    memset(gray, 0xFF, (size_t)bw * bh);   /* 兜底全白（位流截断时尾部保持白） */

    if (f->format == FONT_FMT_1BIT) {
        int row_bytes = (bw + 7) / 8;
        if ((size_t)row_bytes * bh > g->bitmap_size) return;
        for (int row = 0; row < bh; row++) {
            const uint8_t *rb = bitmap + (size_t)row * row_bytes;
            for (int col = 0; col < bw; col++) {
                int bit = (rb[col / 8] >> (7 - (col % 8))) & 1;
                gray[row * bw + col] = bit ? 0xF : 0x0;   /* 存储位 1=白底、0=墨 */
            }
        }
        return;
    }

    /* 旧 EDC Huffman 灰度位流（语义同 blit_glyph_huffman） */
    uint32_t total = (uint32_t)bw * bh;
    uint32_t byte_pos = 0;
    uint8_t  bit_pos = 0;
    for (uint32_t i = 0; i < total && byte_pos < g->bitmap_size; ) {
        if (bit_pos >= 8) {
            bit_pos = 0;
            byte_pos++;
            if (byte_pos >= g->bitmap_size) break;
        }
        uint8_t cur = bitmap[byte_pos];
        uint8_t bit = (cur >> (7 - bit_pos)) & 1;
        uint8_t grayv;
        if (bit == 0) {
            grayv = 0xF;
            bit_pos++;
        } else {
            bit_pos++;
            if (bit_pos >= 8) { bit_pos = 0; byte_pos++; if (byte_pos >= g->bitmap_size) break; cur = bitmap[byte_pos]; }
            bit = (cur >> (7 - bit_pos)) & 1;
            if (bit == 0) {
                grayv = 0x0;
                bit_pos++;
            } else {
                uint8_t gv = 0;
                bit_pos++;
                for (int b = 0; b < 4; b++) {
                    if (bit_pos >= 8) { bit_pos = 0; byte_pos++; if (byte_pos >= g->bitmap_size) break; cur = bitmap[byte_pos]; }
                    if (byte_pos < g->bitmap_size) {
                        gv = (uint8_t)((gv << 1) | ((cur >> (7 - bit_pos)) & 1));
                        bit_pos++;
                    }
                }
                grayv = (uint8_t)(0xF - gv);
            }
        }
        gray[i] = grayv;
        i++;
    }
}

/* 锐化强度（×100）：双线性插值后的 S 形灰度曲线斜率。
 * 1.0 = 原始插值（真机观感发虚，已否决）；∞ = 最近邻（非整数倍阶梯，
 * 真机观感笔画粗细不均，已否决）。220 = 边缘过渡带压到 1~2px，
 * 平滑且锐利（真机调参目标值，偏差可只改这一个宏）。 */
#define FONT_SMOOTH_GAIN_X100 220

/* 锐化双线性放大回写：四角插值消除最近邻的阶梯与笔画不均，
 * 插值结果再过 S 形锐化曲线把灰度过渡带压窄，解决纯插值的发虚。
 * 纯白（>=0xF）像素跳过保持背景透明。 */
static void blit_glyph_smooth(uint8_t *fb, int fbw, int fbh, int gx, int gy,
                              const glyph_index_t *g, const uint8_t *gray, int scale_x2)
{
    int bw = g->bitmap_w, bh = g->bitmap_h;
    if (bw <= 0 || bh <= 0) return;
    int ow = (bw * scale_x2 + 1) / 2;   /* ceil：边缘像素不丢 */
    int oh = (bh * scale_x2 + 1) / 2;

    for (int oy = 0; oy < oh; oy++) {
        /* 定点 8bit 小数：src_y = oy / scale = oy*2/scale_x2 */
        int sy_q = (oy * 2 * 256) / scale_x2;
        int sy = sy_q >> 8;
        int fy = sy_q & 0xFF;
        if (sy >= bh - 1) { sy = bh - 2; fy = 256; }
        if (sy < 0) { sy = 0; fy = 0; }
        int sy2 = sy + ((fy > 0 && sy + 1 < bh) ? 1 : 0);
        for (int ox = 0; ox < ow; ox++) {
            int sx_q = (ox * 2 * 256) / scale_x2;
            int sx = sx_q >> 8;
            int fx = sx_q & 0xFF;
            if (sx >= bw - 1) { sx = bw - 2; fx = 256; }
            if (sx < 0) { sx = 0; fx = 0; }
            int sx2 = sx + ((fx > 0 && sx + 1 < bw) ? 1 : 0);

            int v00 = gray[sy * bw + sx];
            int v10 = gray[sy * bw + sx2];
            int v01 = gray[sy2 * bw + sx];
            int v11 = gray[sy2 * bw + sx2];
            /* 8bit 小数定点灰度（0..3840 = 0..15<<8） */
            int v_q = ((v00 * (256 - fx) + v10 * fx) * (256 - fy)
                     + (v01 * (256 - fx) + v11 * fx) * fy) >> 8;
            /* S 形锐化：以中灰（1920 = 7.5<<8）为支点拉伸对比 */
            int vs = (v_q - 1920) * FONT_SMOOTH_GAIN_X100 / 100 + 1920;
            if (vs < 0) vs = 0;
            if (vs > 3840) vs = 3840;
            uint8_t v = (uint8_t)((vs + 128) >> 8);
            if (v >= 0xF) continue;  /* 纯白跳过，背景透明 */
            fb_put_px(fb, fbw, fbh, gx + ox, gy + oy, v);
        }
    }
}

int font_draw_text_smooth(moka_font_t *f, uint8_t *fb4bpp, int fb_width, int fb_height,
                          int x, int y, const char *utf8_text, int scale_x2)
{
    if (!f || !fb4bpp || !utf8_text) return 0;
    /* 放大倍数 1.0x~4.0x（步进 0.5x，scale_x2=2..8），超限钳位 */
    if (scale_x2 < 2) scale_x2 = 2;
    if (scale_x2 > FONT_MAX_SCALE) scale_x2 = FONT_MAX_SCALE;
    if (scale_x2 == 2) {
        return draw_text_impl(f, fb4bpp, fb_width, fb_height, x, y, utf8_text, 1);
    }

    int pen = x;
    int missing = 0;
    const char *p = utf8_text;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp > 0xFFFF) { missing++; continue; }
        const glyph_index_t *g = find_drawable_glyph(f, cp);
        if (!g) { missing++; continue; }

        if (g->bitmap_size > 0) {
            if (g->bitmap_size > f->bitmap_buf_cap) {
                uint8_t *nb = realloc(f->bitmap_buf, g->bitmap_size);
                if (!nb) { MOKA_LOGE(TAG, "位图缓冲分配失败"); pen += g->width * scale_x2 / 2; continue; }
                f->bitmap_buf = nb; f->bitmap_buf_cap = g->bitmap_size;
            }
            size_t need = (size_t)g->bitmap_w * g->bitmap_h;
            if (need > f->gray_buf_cap) {
                uint8_t *nb = realloc(f->gray_buf, need);
                if (!nb) { MOKA_LOGE(TAG, "灰度缓冲分配失败"); pen += g->width * scale_x2 / 2; continue; }
                f->gray_buf = nb; f->gray_buf_cap = need;
            }
            if (font_read(f, g->bitmap_offset, f->bitmap_buf, g->bitmap_size)) {
                decode_glyph_gray(f, g, f->bitmap_buf, f->gray_buf);
                blit_glyph_smooth(fb4bpp, fb_width, fb_height,
                                  pen + g->x_offset * scale_x2 / 2,
                                  y + (g->y_offset - f->y_shift) * scale_x2 / 2,
                                  g, f->gray_buf, scale_x2);
            } else {
                MOKA_LOGE(TAG, "字形 U+%04X 位图读取失败", (unsigned)cp);
            }
        }
        pen += g->width * scale_x2 / 2;
    }
    if (missing > 0) {
        MOKA_LOGI(TAG, "跳过 %d 个缺字形（不计失败）", missing);
    }
    return pen - x;
}

int font_text_width(moka_font_t *f, const char *utf8_text)
{
    if (!f || !utf8_text) return 0;
    int w = 0;
    const char *p = utf8_text;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        if (cp > 0xFFFF) continue;
        const glyph_index_t *g = find_glyph(f, (uint16_t)cp);
        if (g) w += g->width;
    }
    return w;
}
