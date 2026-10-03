/**
 * card.cpp —— 卡片页渲染实现
 *
 * 布局（逻辑屏 540x960，命中区域宏见 ui/render.h）：
 * - 正面：单词（居中，y≈320）/ 音标（y≈400，可降级隐藏）/ 词性（y≈470）/ 底部提示
 * - 背面：释义（y=80 起自动换行）→ 例句（SOH..STX 区间反色）→ y>=800 三自评按钮
 * - 模式 B：标题 + 释义（y=100 起换行）→ 反馈行（y=480）→ 选项区（y=560 起）
 */
#include "card.h"

#include <stdio.h>
#include <string.h>

#include "render.h"
#include "log.h"

#define TAG "ui_card"

#define CARD_WORD_CY     335    /* 单词块纵向中心（任意字号下保持此中心） */
#define CARD_PHON_Y      400
#define CARD_POS_Y       470
#define CARD_HINT_Y      910
#define CARD_GLOSS_X     40
#define CARD_GLOSS_Y     80
#define CARD_GLOSS_W     (DISP_WIDTH - 2 * CARD_GLOSS_X)
#define CARD_EX_GAP      40     /* 例句距释义结束行的间距 */
#define CARD_WORD_MAX_W  (DISP_WIDTH - 80)   /* 长单词自动降倍保留的左右边距 */

/* 左上角退出按钮：箭头用纯图元绘制（三角箭头+箭杆），
 * 不依赖字体是否含 "←" 字形（内置字库截断区不确定覆盖符号区） */
static void draw_exit_button(void)
{
    ui_render_rect_border(HIT_CARD_EXIT_X0, HIT_CARD_EXIT_Y0,
                          HIT_CARD_EXIT_W, HIT_CARD_EXIT_H, UI_GRAY_BLACK);
    int cy = HIT_CARD_EXIT_Y0 + HIT_CARD_EXIT_H / 2;
    /* 三角箭头头：顶点在左，向右逐列展开 */
    for (int i = 0; i <= 10; i++) {
        ui_render_vline(HIT_CARD_EXIT_X0 + 16 + i, cy - i, 2 * i + 1, UI_GRAY_BLACK);
    }
    /* 箭杆：3px 粗横线 */
    ui_render_fill_rect(HIT_CARD_EXIT_X0 + 27, cy - 1,
                        HIT_CARD_EXIT_X0 + HIT_CARD_EXIT_W - 16 - (HIT_CARD_EXIT_X0 + 27),
                        3, UI_GRAY_BLACK);
}

/* 顶部进度行（"今日待学 x/x · 今日待复习 x/x"）：38px 大字库下整行可能
 * 超宽，自动在 "·" 处断成两行居中（render 层换行助手） */
static void draw_header(const char *header)
{
    if (header && header[0]) {
        ui_render_text_cn_center_wrap(DISP_WIDTH / 2, CARD_HEADER_Y,
                                      DISP_WIDTH - 80, header, "·");
    }
}

static void draw_hint(const char *s)
{
    ui_render_text_cn_center(DISP_WIDTH / 2, CARD_HINT_Y, s);
}

void ui_card_front(const ui_card_front_t *d)
{
    ui_render_clear();
    if (!d || !d->word) {
        MOKA_LOGE(TAG, "正面渲染参数为空");
        ui_render_commit();
        return;
    }
    draw_exit_button();
    draw_header(d->header);

    /* 单词大字：双线性平滑放大（字号来自设置，1.0x~4.0x）；
     * 长单词超宽时自动降倍（最低 1.0x），保证完整显示不截断 */
    int s = d->word_scale2;
    if (s < 2) s = 2;
    if (s > 8) s = 8;
    int fh = font_height(ui_render_font_cn());   /* 字体可换（SD 优先），不硬编码 30px */
    if (fh <= 0) fh = 30;
    int w1 = ui_render_text_cn_width(d->word);
    while (s > 2 && w1 * s / 2 > CARD_WORD_MAX_W) s--;
    int block_h = fh * s / 2;
    int word_y  = CARD_WORD_CY - block_h / 2;
    int word_w  = w1 * s / 2;
    ui_render_text_cn_smooth((DISP_WIDTH - word_w) / 2, word_y, d->word, s);

    /* 音标行：phon 为 NULL（无音标或 IPA 字体缺失）时整行隐藏（NFR-5）；
     * 大字号时单词块下移，音标/词性顺移到单词块下方 */
    int phon_y = word_y + block_h + 24;
    if (phon_y < CARD_PHON_Y) phon_y = CARD_PHON_Y;
    moka_font_t *ipa = ui_render_font_ipa();
    if (d->phon && d->phon[0] && ipa) {
        /* IPA 字体无测宽 API，近似居中：以中文字体估算宽度 */
        int w = 0;
        for (const uint8_t *p = (const uint8_t *)d->phon; *p;) {
            int n = 1;
            if ((*p & 0xE0) == 0xC0) n = 2; else if ((*p & 0xF0) == 0xE0) n = 3;
            w += (*p < 0x80) ? font_height(ipa) / 2 : font_height(ipa);
            p += n;
        }
        ui_render_text(ipa, DISP_WIDTH / 2 - w / 2, phon_y, d->phon);
    }
    if (d->pos_text) {
        int pos_y = phon_y + 64;
        if (pos_y < CARD_POS_Y) pos_y = CARD_POS_Y;
        ui_render_text_cn_center(DISP_WIDTH / 2, pos_y, d->pos_text);
    }
    draw_hint("点击屏幕翻面");
    ui_render_commit();
}

/* 例句绘制：按字节扫描，SOH(0x01)..STX(0x02) 区间反色高亮（docs/wbx_format.md §3）。
 * 逐码点绘制并取 font_draw_text 的真实推进宽度，高亮块随字形精确反色。 */
static int draw_example_hl(moka_font_t *f, int x, int y, int max_w, const char *ex)
{
    int lh = font_height(f) + 8;
    int cx = x;
    bool hl = false;
    for (const uint8_t *p = (const uint8_t *)ex; *p;) {
        if (*p == 0x01) { hl = true;  p++; continue; }
        if (*p == 0x02) { hl = false; p++; continue; }
        if (*p == '\n') { cx = x; y += lh; p++; continue; }
        int n = 1;
        if ((*p & 0xE0) == 0xC0) n = 2;
        else if ((*p & 0xF0) == 0xE0) n = 3;
        else if ((*p & 0xF8) == 0xF0) n = 4;
        int est = (*p < 0x80) ? font_height(f) / 2 : font_height(f);
        if (cx + est > x + max_w && cx > x) { cx = x; y += lh; }
        if (y + lh > HIT_CARD_BTN_Y0) break;   /* 不画进按钮区 */
        char cp[5];
        memcpy(cp, p, (size_t)n);
        cp[n] = '\0';
        int w = ui_render_text(f, cx, y, cp);
        int adv = (w > 0) ? w : est;
        if (hl) ui_render_invert_rect(cx, y - 2, adv, lh);
        cx += adv;
        p += n;
    }
    return y + lh;
}

void ui_card_back(const ui_card_back_t *d)
{
    ui_render_clear();
    if (!d || !d->gloss) {
        MOKA_LOGE(TAG, "背面渲染参数为空");
        ui_render_commit();
        return;
    }
    moka_font_t *cn = ui_render_font_cn();   /* NULL 时绘制助手内部安全降级 */
    /* 释义（中文字体，自动换行） */
    int end_y = ui_render_wrapped(cn, CARD_GLOSS_X, CARD_GLOSS_Y, CARD_GLOSS_W, d->gloss);

    if (d->example && d->example[0] && cn) {
        /* 例句主体为英文，用内置字体（含 ASCII 字形）渲染 */
        draw_example_hl(cn, CARD_GLOSS_X, end_y + CARD_EX_GAP - 8, CARD_GLOSS_W, d->example);
    }

    /* 底部三自评按钮（区域划分与 HIT_CARD_* 一致） */
    static const char *def_labels[3] = { "忘记", "模糊", "认识" };
    for (int i = 0; i < HIT_CARD_BTN_COUNT; i++) {
        const char *label = (d->btn_labels[i]) ? d->btn_labels[i] : def_labels[i];
        ui_render_button(i * HIT_CARD_BTN_W, HIT_CARD_BTN_Y0,
                         HIT_CARD_BTN_W, HIT_CARD_BTN_H, label, false);
    }
    ui_render_commit();
}

void ui_card_quiz(const ui_card_quiz_t *d)
{
    ui_render_clear();
    if (!d || d->count < 2 || d->count > HIT_QUIZ_OPT_MAX) {
        MOKA_LOGE(TAG, "选择题渲染参数非法 count=%d", d ? d->count : -1);
        ui_render_commit();
        return;
    }
    draw_exit_button();
    draw_header(d->header);
    /* 进度行可能折成两行（92..176），释义从 180 起避开；
     * 字体 NULL 时 ui_render_wrapped 内部安全降级（与 ui_card_back 一致） */
    ui_render_wrapped(ui_render_font_cn(), CARD_GLOSS_X, 180, CARD_GLOSS_W, d->gloss);

    if (d->feedback) {
        ui_render_text_cn_center(DISP_WIDTH / 2, HIT_QUIZ_FEEDBACK_Y, d->feedback);
    }

    for (int i = 0; i < d->count; i++) {
        int y = HIT_QUIZ_OPT_Y0 + i * HIT_QUIZ_OPT_STEP;
        ui_render_button(HIT_QUIZ_OPT_X0, y,
                         HIT_QUIZ_OPT_X1 - HIT_QUIZ_OPT_X0, HIT_QUIZ_OPT_H,
                         d->options[i], i == d->highlight);
    }
    ui_render_commit();
}

/* 退出确认弹窗：覆盖式（不清屏），白块 + 双层边框 + 两按钮 */
void ui_card_exit_confirm(uint32_t learned_today)
{
    ui_render_fill_rect(HIT_POPUP_X0, HIT_POPUP_Y0, HIT_POPUP_W, HIT_POPUP_H,
                        UI_GRAY_WHITE);
    ui_render_rect_border(HIT_POPUP_X0, HIT_POPUP_Y0, HIT_POPUP_W, HIT_POPUP_H,
                          UI_GRAY_BLACK);
    ui_render_rect_border(HIT_POPUP_X0 + 3, HIT_POPUP_Y0 + 3,
                          HIT_POPUP_W - 6, HIT_POPUP_H - 6, UI_GRAY_BLACK);

    char buf[64];
    snprintf(buf, sizeof(buf), "今天已学习 %lu 个单词", (unsigned long)learned_today);
    /* 38px 大字库下长句可能超出弹窗宽度：超宽自动折行居中 */
    ui_render_text_cn_center_wrap(DISP_WIDTH / 2, HIT_POPUP_Y0 + 48,
                                  HIT_POPUP_W - 48, buf, NULL);
    ui_render_text_cn_center(DISP_WIDTH / 2, HIT_POPUP_Y0 + 108, "要退出吗");

    ui_render_button(HIT_POPUP_CONTINUE_X0, HIT_POPUP_BTN_Y0,
                     HIT_POPUP_BTN_W, HIT_POPUP_BTN_H, "继续", false);
    ui_render_button(HIT_POPUP_EXIT_X0, HIT_POPUP_BTN_Y0,
                     HIT_POPUP_BTN_W, HIT_POPUP_BTN_H, "退出", true);
    ui_render_commit();
}

void ui_card_message(const char *line1, const char *line2)
{
    ui_render_clear();
    int y = 420;
    if (line1) y = ui_render_text_cn_center_wrap(DISP_WIDTH / 2, y,
                                                 DISP_WIDTH - 80, line1, NULL);
    if (line2) ui_render_text_cn_center_wrap(DISP_WIDTH / 2, y + 14,
                                             DISP_WIDTH - 80, line2, NULL);
    ui_render_commit();
}
