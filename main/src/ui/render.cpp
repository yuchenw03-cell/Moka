/**
 * render.cpp —— 帧缓冲绘图助手实现
 *
 * 注意：
 * - 只操作 disp_framebuffer() 返回的 4bpp 缓冲，刷屏一律经 ui_render_commit
 *   （底层 hal/display.c 集中管理局刷计数与每 10 次局刷强制全刷，NFR-2）。
 * - 4bpp 像素布局：每字节两个像素，偶数 x 占高半字节（与 epdiy 一致）。
 *   若 hal 实现的字节序相反，只需改 set_px 一处。
 * - 文本居中/按钮标签统一用 ui_render_text_cn_width 精确测宽（估算宽度在
 *   大字库下偏位肉眼可见，已弃用）；换行估算仅用于 ui_render_wrapped 排版。
 */
#include "render.h"

#include <string.h>

#include "log.h"

#define TAG "render"

static uint8_t    *s_fb;
static moka_font_t *s_font_cn;
static moka_font_t *s_font_ipa;

void ui_render_init(moka_font_t *font_cn, moka_font_t *font_ipa)
{
    s_fb       = disp_framebuffer();
    s_font_cn  = font_cn;
    s_font_ipa = font_ipa;
    if (!s_fb)       MOKA_LOGE(TAG, "帧缓冲指针为空");
    if (!s_font_cn)  MOKA_LOGE(TAG, "中文字体加载失败，文本将不显示（NFR-5 降级）");
    if (!s_font_ipa) MOKA_LOGE(TAG, "IPA 音标字体缺失，音标行将隐藏（NFR-5 降级）");
}

moka_font_t *ui_render_font_cn(void)
{
    return s_font_cn;
}

moka_font_t *ui_render_font_ipa(void)
{
    return s_font_ipa;
}

/* 写单个像素（4bpp，偶数 x 高半字节） */
static inline void set_px(int x, int y, uint8_t gray)
{
    if (!s_fb || x < 0 || y < 0 || x >= DISP_WIDTH || y >= DISP_HEIGHT) return;
    uint8_t *p = s_fb + ((y * DISP_WIDTH) + x) / 2;
    if ((x & 1) == 0) *p = (*p & 0x0F) | (uint8_t)(gray << 4);
    else              *p = (*p & 0xF0) | (gray & 0x0F);
}

static inline uint8_t get_px(int x, int y)
{
    if (!s_fb || x < 0 || y < 0 || x >= DISP_WIDTH || y >= DISP_HEIGHT) return 0xF;
    uint8_t *p = s_fb + ((y * DISP_WIDTH) + x) / 2;
    return ((x & 1) == 0) ? (uint8_t)(*p >> 4) : (uint8_t)(*p & 0x0F);
}

void ui_render_clear(void)
{
    if (s_fb) memset(s_fb, 0xFF, (size_t)DISP_WIDTH * DISP_HEIGHT / 2); /* 两像素皆白 */
}

void ui_render_fill_rect(int x, int y, int w, int h, uint8_t gray)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISP_WIDTH)  w = DISP_WIDTH - x;
    if (y + h > DISP_HEIGHT) h = DISP_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    uint8_t pair = (uint8_t)((gray << 4) | gray);
    for (int yy = y; yy < y + h; yy++) {
        int xx = x;
        /* 对齐到字节边界后用整字节填充加速 */
        while (xx < x + w && (xx & 1)) { set_px(xx, yy, gray); xx++; }
        int full = (x + w - xx) / 2;
        if (full > 0 && s_fb) {
            memset(s_fb + ((yy * DISP_WIDTH) + xx) / 2, pair, (size_t)full);
            xx += full * 2;
        }
        while (xx < x + w) { set_px(xx, yy, gray); xx++; }
    }
}

void ui_render_invert_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISP_WIDTH)  w = DISP_WIDTH - x;
    if (y + h > DISP_HEIGHT) h = DISP_HEIGHT - y;
    /* 0x0F - v == v ^ 0x0F（全 1 减法无借位），整字节 XOR 0xFF 即双像素反色 */
    for (int yy = y; yy < y + h; yy++) {
        for (int xx = x; xx < x + w; xx++) {
            uint8_t v = get_px(xx, yy);
            set_px(xx, yy, (uint8_t)(v ^ 0x0F));
        }
    }
}

void ui_render_hline(int x, int y, int w, uint8_t gray)
{
    ui_render_fill_rect(x, y, w, 1, gray);
}

void ui_render_vline(int x, int y, int h, uint8_t gray)
{
    ui_render_fill_rect(x, y, 1, h, gray);
}

void ui_render_rect_border(int x, int y, int w, int h, uint8_t gray)
{
    ui_render_hline(x, y, w, gray);
    ui_render_hline(x, y + h - 1, w, gray);
    ui_render_vline(x, y, h, gray);
    ui_render_vline(x + w - 1, y, h, gray);
}

int ui_render_text(moka_font_t *f, int x, int y, const char *utf8)
{
    if (!f || !utf8 || !utf8[0] || !s_fb) return 0;   /* 降级：不显示该行（NFR-5） */
    return font_draw_text(f, s_fb, DISP_WIDTH, DISP_HEIGHT, x, y, utf8);
}

int ui_render_text_cn(int x, int y, const char *utf8)
{
    return ui_render_text(s_font_cn, x, y, utf8);
}

int ui_render_text_cn_smooth(int x, int y, const char *utf8, int scale_x2)
{
    if (!s_font_cn || !utf8 || !utf8[0] || !s_fb) return 0;   /* NFR-5 降级 */
    return font_draw_text_smooth(s_font_cn, s_fb, DISP_WIDTH, DISP_HEIGHT,
                                 x, y, utf8, scale_x2);
}

int ui_render_text_cn_width(const char *utf8)
{
    if (!s_font_cn || !utf8) return 0;
    return font_text_width(s_font_cn, utf8);
}

/* UTF-8 码点字节数（1~4；非法首字节按 1 处理，保证扫描不卡死） */
static int utf8_cp_len(uint8_t c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 估算单码点宽度（见文件头 TODO：字体层缺测宽 API） */
static int est_cp_width(moka_font_t *f, uint8_t lead)
{
    int h = f ? font_height(f) : 30;
    return (lead < 0x80) ? (h * 55 / 100) : h;
}

/* 估算整串宽度（跳过 SOH/STX 控制字符） */
static int est_text_width(moka_font_t *f, const char *s)
{
    int w = 0;
    for (const uint8_t *p = (const uint8_t *)s; *p;) {
        if (*p == 0x01 || *p == 0x02) { p++; continue; }
        w += est_cp_width(f, *p);
        p += utf8_cp_len(*p);
    }
    return w;
}

int ui_render_text_cn_center(int cx, int y, const char *utf8)
{
    if (!utf8) return 0;
    /* 精确测宽居中（font_text_width 已可用）：估算宽度在大字库下误差
     * 肉眼可见（真机照片实测头部行偏左约 1 字符），统一弃用估算 */
    int w = ui_render_text_cn_width(utf8);
    if (w <= 0) w = est_text_width(s_font_cn, utf8);   /* 字体缺失兜底 */
    return ui_render_text_cn(cx - w / 2, y, utf8);
}

int ui_render_text_cn_center_wrap(int cx, int y, int max_w, const char *utf8,
                                  const char *sep)
{
    if (!utf8 || !utf8[0]) return y;
    int fh = s_font_cn ? font_height(s_font_cn) : 30;
    int lh = fh + 8;

    int w = ui_render_text_cn_width(utf8);
    if (w <= 0 || w <= max_w) {          /* 单行放得下（或字体缺失无法测宽） */
        ui_render_text_cn_center(cx, y, utf8);
        return y + lh;
    }

    /* 断点搜索：满足"左半不超宽"的最右 sep；
     * UI 文案远短于 160B，栈上缓冲足够（规范 4KB 红线内） */
    char buf[160];
    size_t len = strlen(utf8);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    size_t sep_len = (sep && sep[0]) ? strlen(sep) : 0;
    const char *split = NULL;
    if (sep_len > 0) {
        for (const char *p = utf8; *p && (size_t)(p - utf8) < len; p++) {
            if (strncmp(p, sep, sep_len) != 0) continue;
            size_t l = (size_t)(p - utf8);
            memcpy(buf, utf8, l);
            buf[l] = '\0';
            int lw = ui_render_text_cn_width(buf);
            if (lw > 0 && lw <= max_w) split = p;
        }
    }

    if (split) {
        size_t l = (size_t)(split - utf8);
        memcpy(buf, utf8, l);
        buf[l] = '\0';
        /* trim 尾部空格：分隔符前的空格计入宽度会让视觉中心左偏 */
        while (l > 0 && buf[l - 1] == ' ') buf[--l] = '\0';
        ui_render_text_cn_center(cx, y, buf);
        const char *rest = split + sep_len;
        while (*rest == ' ') rest++;
        ui_render_text_cn_center(cx, y + lh, rest);
        return y + 2 * lh;
    }

    /* 无可用 sep：取不超宽的最长前缀为第一行，其余第二行 */
    size_t best = 0;
    for (const uint8_t *p = (const uint8_t *)utf8;
         *p && (size_t)((const char *)p - utf8) < len; ) {
        int n = utf8_cp_len(*p);
        size_t l = (size_t)((const char *)p - utf8) + (size_t)n;
        if (l >= sizeof(buf)) break;
        memcpy(buf, utf8, l);
        buf[l] = '\0';
        if (ui_render_text_cn_width(buf) > max_w) break;
        best = l;
        p += n;
    }
    if (best == 0) best = 1;    /* 极端防御：至少前进 1 字节 */
    memcpy(buf, utf8, best);
    buf[best] = '\0';
    ui_render_text_cn_center(cx, y, buf);
    ui_render_text_cn_center(cx, y + lh, utf8 + best);
    return y + 2 * lh;
}

int ui_render_wrapped(moka_font_t *f, int x, int y, int max_w, const char *utf8)
{
    if (!f || !utf8 || !utf8[0]) return y;
    int lh  = font_height(f) + 8;
    int cx  = x;
    for (const uint8_t *p = (const uint8_t *)utf8; *p;) {
        if (*p == '\n') { cx = x; y += lh; p++; continue; }
        int n = utf8_cp_len(*p);
        int est = est_cp_width(f, *p);
        if (cx + est > x + max_w && cx > x) { cx = x; y += lh; }
        if (y + lh > DISP_HEIGHT) break;   /* 防画出屏外 */
        char cp[5];
        memcpy(cp, p, (size_t)n);
        cp[n] = '\0';
        int w = ui_render_text(f, cx, y, cp);
        cx += (w > 0) ? w : est;
        p += n;
    }
    return y + lh;
}

void ui_render_button(int x, int y, int w, int h, const char *label, bool invert)
{
    ui_render_rect_border(x, y, w, h, UI_GRAY_BLACK);
    if (label && s_font_cn) {
        int fh = font_height(s_font_cn);
        int tw = ui_render_text_cn_width(label);      /* 精确测宽居中 */
        if (tw <= 0) tw = est_text_width(s_font_cn, label);
        ui_render_text_cn(x + (w - tw) / 2, y + (h - fh) / 2, label);
    }
    if (invert) ui_render_invert_rect(x, y, w, h);
}

void ui_render_commit(void)
{
    /* 每次画面更新唯一一次局刷推送（防连跳约束）；全刷由 hal 计数与休眠路径负责 */
    disp_push_partial(0, 0, DISP_WIDTH, DISP_HEIGHT);
}
