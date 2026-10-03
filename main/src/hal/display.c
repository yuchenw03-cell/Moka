/**
 * display.c —— epdiy 封装实现（ED047TC1 直驱，M5 Paper S3）
 * 坐标系：对外逻辑竖屏 540×960；epdiy 原生横向 960×540，旋转映射在本文件
 * push 时完成（logical↔native，见 logical_to_native）。若真机画面上下颠倒/镜像，
 * 改 MOKA_DISP_ROT_CW 为 0 即可。
 * 帧缓冲（NFR-1）：逻辑 4bpp 540×960=259200B，强制分配在 PSRAM 并校验；
 * push 时旋转拷贝进 epdiy 高层帧缓冲再调 epd_hl_update_*。
 * 刷新纪律（NFR-2）：局刷计数满 10 自动升级全刷；全刷清零计数；
 * 每次刷屏前开 PWR（G45）、完成后关。
 */
#include "display.h"
#include "epdiy_board_papers3.h"
#include "../log.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"

/* epdiy v2 聚合头（内含 epd_board.h / epd_display.h / epd_highlevel.h 等，
 * 见 epdiy.h:200-203；v1 时代的 epd_driver.h 已不存在） */
#include "epdiy.h"

#define TAG "moka.disp"

#define FB_BYTES        (DISP_WIDTH * DISP_HEIGHT / 2)   /* 259200 */
#define NATIVE_WIDTH    960
#define NATIVE_HEIGHT   540

/* 旋转方向：1 = 逻辑 90° CW（native_x = 959-ly, native_y = lx）；
 *           0 = 90° CCW（真机实测 CW 方向画面倒立 180°，故用 0） */
#define MOKA_DISP_ROT_CW 0

/* NFR-2：局刷累计阈值，达到后强制一次全刷清残影 */
#define PARTIAL_FULL_THRESHOLD 10

/* 刷屏环境温度。epdiy v2.1.3 的 epd_ambient_temperature()（epdiy.h:245）依赖
 * 板级 get_temperature 回调，PaperS3 无温度传感器（板级置 NULL），故固定 25°C */
#define EPD_TEMP_C 25

static uint8_t            *s_fb;              /* 逻辑帧缓冲（PSRAM） */
static EpdiyHighlevelState s_hl;
static bool                s_hl_ok;
static int                 s_partial_count;
static bool                s_epd_powered;

/* 4bpp 约定：每字节两像素，偶数 x 占高半字节（与 font.c / main.cpp 一致） */
static inline uint8_t fb_get_px(const uint8_t *fb, int w, int x, int y)
{
    uint8_t byte = fb[(y * w + x) / 2];
    return (x & 1) ? (byte & 0x0F) : (byte >> 4);
}

static inline void logical_to_native(int lx, int ly, int *nx, int *ny)
{
#if MOKA_DISP_ROT_CW
    *nx = NATIVE_WIDTH - 1 - ly;   /* 959 - ly */
    *ny = lx;
#else
    *nx = ly;
    *ny = NATIVE_HEIGHT - 1 - lx;  /* 539 - lx */
#endif
}

/* 把逻辑帧缓冲旋转拷贝到 epdiy 高层帧缓冲的原生矩形区域。
 * 逐像素用 epd_draw_pixel 写入：半字节打包格式由 epdiy 自己保证，
 * 避免与其内部帧缓冲格式（4bpp/8bpp）约定不一致。 */
static void rotate_copy_native_rect(int nx0, int ny0, int nw, int nh)
{
    uint8_t *hl_fb = epd_hl_get_framebuffer(&s_hl);
    if (!hl_fb) return;
    for (int ny = ny0; ny < ny0 + nh; ny++) {
        for (int nx = nx0; nx < nx0 + nw; nx++) {
            int lx, ly;
#if MOKA_DISP_ROT_CW
            lx = ny;
            ly = NATIVE_WIDTH - 1 - nx;
#else
            lx = NATIVE_HEIGHT - 1 - ny;
            ly = nx;
#endif
            uint8_t g = fb_get_px(s_fb, DISP_WIDTH, lx, ly);
            /* 颜色域已核实：epd_draw_pixel 只取高半字节（color & 0xF0 / >>4，
             * 依据 epdiy v2.1.3 epdiy.c:76-94），文档颜色域 0x00 黑 ~ 0xF0 白
             * （epd_highlevel.h:48-59）。moka 逻辑灰度是 0x0..0xF，
             * 须 ×17 展开成 8bit，否则一律被截成纯黑。
             * 注意：epd_draw_pixel 内部还会按 epd_get_rotation() 再转一次坐标，
             * 本文件的旋转由 rot 映射自行完成，因此全工程禁止调 epd_set_rotation()。 */
            epd_draw_pixel(nx, ny, (uint8_t)(g * 17), hl_fb);
        }
    }
}

static void push_power_on(void)
{
    if (!s_epd_powered) {
        epd_poweron();   /* 经板级回调开 PWR=G45 */
        s_epd_powered = true;
    }
}

static void push_power_off(void)
{
    if (s_epd_powered) {
        epd_poweroff();
        s_epd_powered = false;
    }
}

bool disp_init(void)
{
    if (s_fb) return true;   /* 幂等 */

    /* NFR-1：帧缓冲强制 PSRAM，失败必须报错，禁止静默 */
    s_fb = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) {
        MOKA_LOGE(TAG, "帧缓冲 PSRAM 分配失败（%d B）", FB_BYTES);
        return false;
    }
    if (!esp_ptr_external_ram(s_fb)) {
        MOKA_LOGE(TAG, "帧缓冲不在 PSRAM！（NFR-1 违规）");
        heap_caps_free(s_fb);
        s_fb = NULL;
        return false;
    }
    memset(s_fb, 0xFF, FB_BYTES);   /* 默认全白 */

    /* epd_init(board, display, options) 签名已核实（epdiy v2.1.3 epdiy.h:206-208）；
     * 板级类型为 EpdBoardDefinition（epd_board.h），见 epdiy_board_papers3.c。 */
    epd_init(&epd_board_papers3, &ED047TC1, EPD_OPTIONS_DEFAULT);

    /* EPD_BUILTIN_WAVEFORM = NULL 宏（epd_highlevel.h:70）；
     * 传 NULL 时 epd_hl_init 回落到屏参默认波形 epdiy_ED047TC1
     * （highlevel.c:28-30 + displays.c:53）。 */
    s_hl = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    s_hl_ok = (epd_hl_get_framebuffer(&s_hl) != NULL);
    if (!s_hl_ok) {
        MOKA_LOGE(TAG, "epd_hl_init 失败（高层帧缓冲分配失败）");
        heap_caps_free(s_fb);
        s_fb = NULL;
        return false;
    }

    /* epd_width()/epd_height() 函数名已核实（epdiy v2.1.3 epdiy.h:226/231） */
    MOKA_LOGI(TAG, "epdiy init OK: 原生 %dx%d，逻辑 %dx%d，fb=%p（%s）",
              epd_width(), epd_height(), DISP_WIDTH, DISP_HEIGHT,
              s_fb, esp_ptr_external_ram(s_fb) ? "PSRAM" : "RAM?");
    if (epd_width() != NATIVE_WIDTH || epd_height() != NATIVE_HEIGHT) {
        MOKA_LOGE(TAG, "屏尺寸不符！期望 %dx%d（ED047TC1）", NATIVE_WIDTH, NATIVE_HEIGHT);
    }
    return true;
}

uint8_t *disp_framebuffer(void)
{
    return s_fb;
}

void disp_clear(uint8_t gray)
{
    if (!s_fb) return;
    uint8_t g = gray & 0x0F;
    memset(s_fb, (g << 4) | g, FB_BYTES);
}

void disp_push_partial(int x, int y, int w, int h)
{
    if (!s_fb || !s_hl_ok) {
        MOKA_LOGE(TAG, "disp_push_partial：未初始化");
        return;
    }

    /* 越界裁剪（逻辑坐标） */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISP_WIDTH)  w = DISP_WIDTH - x;
    if (y + h > DISP_HEIGHT) h = DISP_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    s_partial_count++;
    if (s_partial_count >= PARTIAL_FULL_THRESHOLD) {
        MOKA_LOGI(TAG, "局刷累计 %d 次，升级为全刷（NFR-2）", s_partial_count);
        disp_push_full();
        return;
    }

    /* 逻辑脏矩形 → 原生矩形（旋转后宽高互换） */
    int ax, ay, bx, by;
    logical_to_native(x, y + h - 1, &ax, &ay);       /* 原生矩形左上角 */
    logical_to_native(x + w - 1, y, &bx, &by);       /* 原生矩形右下角 */
    EpdRect area = {
        .x      = ax < bx ? ax : bx,
        .y      = ay < by ? ay : by,
        .width  = (bx > ax ? bx - ax : ax - bx) + 1,
        .height = (by > ay ? by - ay : ay - by) + 1,
    };
    /* EPD 控制器列对齐：向外扩到 8 像素边界 */
    area.x &= ~7;
    if (area.x + area.width > NATIVE_WIDTH) area.width = NATIVE_WIDTH - area.x;
    area.width = (area.width + 7) & ~7;
    if (area.x + area.width > NATIVE_WIDTH) area.width = NATIVE_WIDTH - area.x;
    if (area.y + area.height > NATIVE_HEIGHT) area.height = NATIVE_HEIGHT - area.y;

    rotate_copy_native_rect(area.x, area.y, area.width, area.height);

    push_power_on();
    /* epd_hl_update_area(state, mode, temperature, area) 参数顺序已核实
     * （epdiy v2.1.3 epd_highlevel.h:135-137）；MODE_GL16 / EPD_DRAW_SUCCESS
     * 枚举名已核实（epdiy.h:65 / epdiy.h:132）。
     * 注意：epd_hl_update_area 内部会按 epd_get_rotation() 对 area 做逆旋转
     * （highlevel.c:75-107）——本工程保持默认 EPD_ROT_LANDSCAPE 不旋转，
     * 旋转由本文件 rot 映射完成，禁止调 epd_set_rotation()。 */
    enum EpdDrawError err = epd_hl_update_area(&s_hl, MODE_GL16, EPD_TEMP_C, area);
    push_power_off();

    if (err != EPD_DRAW_SUCCESS) {
        MOKA_LOGE(TAG, "局刷失败 err=%d（area %d,%d %dx%d）",
                  (int)err, area.x, area.y, area.width, area.height);
    } else {
        MOKA_LOGI(TAG, "局刷 #%d：逻辑(%d,%d %dx%d)", s_partial_count, x, y, w, h);
    }
}

void disp_push_full(void)
{
    if (!s_fb || !s_hl_ok) {
        MOKA_LOGE(TAG, "disp_push_full：未初始化");
        return;
    }
    rotate_copy_native_rect(0, 0, NATIVE_WIDTH, NATIVE_HEIGHT);

    push_power_on();
    /* epd_hl_update_screen(state, mode, temperature) 签名已核实
     * （epdiy v2.1.3 epd_highlevel.h:119-121）。
     * 全刷用 MODE_GC16（闪烁刷，epdiy.h:57）而非局刷的 MODE_GL16——
     * GL16 是无闪烁模式，清残影能力弱，不满足 NFR-2 "全刷清残影" 语义。 */
    enum EpdDrawError err = epd_hl_update_screen(&s_hl, MODE_GC16, EPD_TEMP_C);
    push_power_off();

    s_partial_count = 0;
    if (err != EPD_DRAW_SUCCESS) {
        MOKA_LOGE(TAG, "全刷失败 err=%d", (int)err);
    } else {
        MOKA_LOGI(TAG, "全刷完成，局刷计数清零");
    }
}

void disp_push_full_clean(void)
{
    /* 为什么不是裸 disp_push_full：epd_hl_update_screen 走帧间 diff，
     * "前帧白、新帧也白"的像素不驱屏——关机页只有中间一块文字时，
     * 大面积白底不参与刷新，与旧页面残留形成肉眼可见的色差。
     * 先 epd_clear() 把物理屏拉到全白（低层直驱，不看 diff），
     * 再 GC16 推送只需驱动文字像素，白底物理上已经干净，
     * 等效"全屏全刷"且无色差。代价是多一次约 1s 的清屏闪烁，
     * 仅用于关机页这类对干净度敏感的场景。 */
    disp_force_clear();
    disp_push_full();
}

void disp_force_clear(void)
{
    if (!s_fb || !s_hl_ok) {
        MOKA_LOGE(TAG, "disp_force_clear：未初始化");
        return;
    }
    /* epd_clear() 走低层 epd_push_pixels 直驱（epdiy.c:37→epd_clear_area），
     * 不经过高层帧缓冲 diff——epd_hl_update_screen 在"前帧=新帧=全白"时
     * diff 为空会空转跳过，物理画面（如前固件残页）原样保留。
     * 这里强制物理刷白，保证任何前态下屏幕都回到干净白底。 */
    push_power_on();
    epd_clear();
    push_power_off();

    s_partial_count = 0;
    MOKA_LOGI(TAG, "强制清屏完成（物理刷白，绕过 diff）");
}

void disp_power_save(bool on)
{
    /* 语义约定：on=true 进入省电（关 EPD 高压 PWR=G45）；on=false 恢复供电。
     * 刷屏流程（disp_push_*）内部自行上电/断电，不受本开关残留状态影响。 */
    if (on) {
        if (s_epd_powered) {
            epd_poweroff();
            s_epd_powered = false;
        }
        MOKA_LOGI(TAG, "EPD 电源关闭（省电）");
    } else {
        if (!s_epd_powered) {
            epd_poweron();
            s_epd_powered = true;
        }
        MOKA_LOGI(TAG, "EPD 电源开启");
    }
}
