/**
 * shutdown.cpp —— 关机画面实现
 *
 * 版面设计（540×960 竖屏）：
 * - 主标题"墨卡"：内置 30px 中文字体 6 倍最近邻放大（≈180px/字），水平居中，
 *   纵向置于屏幕黄金分割偏上（y≈330）。
 * - 副标题"MOKA"：2 倍放大（60px），逐字母均匀分布，整体宽度与"墨卡"
 *   精确相同——首字母 M 左边缘对齐"墨"左边缘，末字母 A 右边缘对齐"卡"
 *   右边缘（字距 = (中文宽 - 四字母实宽和) / 3，实测宽度而非估算）。
 * - 本函数不调用 ui_render_commit：关机页必须全刷（断电留图要干净），
 *   刷屏动作由 power_shutdown() 的 disp_push_full() 统一执行。
 */
#include "shutdown.h"

#include "render.h"

#define LOGO_CN_SCALE_X2  12    /* 中文主标 6.0x：30px → 180px/字 */
#define LOGO_EN_SCALE_X2  4     /* 英文副标 2.0x：30px → 60px */
#define LOGO_CN_Y         330   /* 中文行顶 y（屏幕纵向黄金分割偏上） */
#define LOGO_GAP_Y        70    /* 中文底部到英文行顶的间距 */
#define LOGO_EN_LETTER_MAX 8    /* "MOKA" 字母数上限（防御性，实际 4） */

void ui_shutdown_render(void)
{
    ui_render_clear();

    moka_font_t *cn = ui_render_font_cn();
    if (!cn) return;    /* 字体缺失：白屏关机（NFR-5 降级，不显示乱码） */

    /* 中文主标：实测宽度 × 放大倍数 = 放大后总宽，水平居中 */
    int cn_w = ui_render_text_cn_width("墨卡") * LOGO_CN_SCALE_X2 / 2;
    if (cn_w <= 0 || cn_w > DISP_WIDTH) return;   /* 字体异常时宁可空白 */
    int x0 = (DISP_WIDTH - cn_w) / 2;
    ui_render_text_cn_smooth(x0, LOGO_CN_Y, "墨卡", LOGO_CN_SCALE_X2);

    /* 英文副标：逐字母测宽，均匀分布到与中文块等宽 */
    static const char *letters[] = { "M", "O", "K", "A" };
    const int n = 4;
    int lw[LOGO_EN_LETTER_MAX] = {0};
    int sum = 0;
    for (int i = 0; i < n; i++) {
        lw[i] = ui_render_text_cn_width(letters[i]) * LOGO_EN_SCALE_X2 / 2;
        sum += lw[i];
    }
    if (sum <= 0 || sum >= cn_w) return;    /* 字母缺失或过宽则放弃副标 */

    int gap = (cn_w - sum) / (n - 1);       /* 字距：首尾顶格，中间均分 */
    int pen = x0;
    int ey  = LOGO_CN_Y + font_height(cn) * LOGO_CN_SCALE_X2 / 2 + LOGO_GAP_Y;
    for (int i = 0; i < n; i++) {
        ui_render_text_cn_smooth(pen, ey, letters[i], LOGO_EN_SCALE_X2);
        pen += lw[i] + gap;
    }
}
