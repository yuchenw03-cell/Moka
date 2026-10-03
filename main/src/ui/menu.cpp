/**
 * menu.cpp —— 菜单页 / 设置页渲染实现
 * 布局坐标全部来自 ui/render.h 的 HIT_* 宏（命中判定与绘制同源）。
 */
#include "menu.h"

#include <stdio.h>
#include <time.h>

#include "render.h"

#define MENU_TITLE_Y   28
#define MENU_ROW_NAME_DY   14
#define MENU_ROW_SUM_DY    54

void ui_menu_render(const ui_menu_item_t *items, int count, int page, int pages)
{
    ui_render_clear();

    /* 标题栏：左"墨卡"（2.0x 最近邻大字，整数倍笔画均匀），
     * 右 [关机][统计][设置]；x=24 避让关机按钮区（180 起） */
    int title_fh = font_height(ui_render_font_cn());
    if (title_fh <= 0) title_fh = 30;
    ui_render_text_cn_smooth(24, (HIT_HEADER_H - title_fh * 2) / 2, "墨卡", 4);
    ui_render_button(HIT_MENU_PWR_X0 + 10, 20,
                     HIT_MENU_PWR_X1 - HIT_MENU_PWR_X0 - 20, 56, "关机", true);
    ui_render_button(HIT_MENU_STATS_X0 + 10, 20,
                     HIT_MENU_STATS_X1 - HIT_MENU_STATS_X0 - 20, 56, "统计", false);
    ui_render_button(HIT_MENU_SET_X0 + 10, 20,
                     HIT_MENU_SET_X1 - HIT_MENU_SET_X0 - 10, 56, "设置", false);
    ui_render_hline(0, HIT_HEADER_H - 1, DISP_WIDTH, UI_GRAY_BLACK);

    /* 词书列表：每行 96px，书名 + 概要行。
     * 概要用紧凑格式（"总3739 学12 复34"）：38px 大字库下完整标签会超宽 */
    char sum[80];
    for (int i = 0; i < count; i++) {
        int y = HIT_MENU_ROW_Y0 + i * HIT_MENU_ROW_H;
        ui_render_text_cn(40, y + MENU_ROW_NAME_DY, items[i].name);
        snprintf(sum, sizeof(sum), "总%u 学%u 复%u",
                 (unsigned)items[i].total, (unsigned)items[i].learned,
                 (unsigned)items[i].due);
        ui_render_text_cn(40, y + MENU_ROW_SUM_DY, sum);
        if (items[i].current) {
            ui_render_text_cn(DISP_WIDTH - 100, y + MENU_ROW_NAME_DY, "当前");
        }
        ui_render_hline(0, y + HIT_MENU_ROW_H - 1, DISP_WIDTH, UI_GRAY_LIGHT);
    }

    /* 底部：多页时翻页按钮，否则操作提示 */
    if (pages > 1) {
        ui_render_button(HIT_MENU_PREV_X0 + 20, HIT_MENU_BOTTOM_Y0 + 20,
                         HIT_MENU_PREV_X1 - HIT_MENU_PREV_X0 - 40, 56, "上一页", false);
        ui_render_button(HIT_MENU_NEXT_X0 + 20, HIT_MENU_BOTTOM_Y0 + 20,
                         HIT_MENU_NEXT_X1 - HIT_MENU_NEXT_X0 - 40, 56, "下一页", false);
        char pg[24];
        snprintf(pg, sizeof(pg), "%d/%d", page + 1, pages);
        ui_render_text_cn_center(DISP_WIDTH / 2, HIT_MENU_BOTTOM_Y0 + 32, pg);
    } else {
        ui_render_text_cn_center(DISP_WIDTH / 2, HIT_MENU_BOTTOM_Y0 + 40,
                                 "点击词书开始今日复习");
    }
    ui_render_commit();
}

void ui_settings_render(uint16_t daily_new, uint8_t default_mode, uint32_t now_unix,
                        uint8_t word_scale2)
{
    ui_render_clear();

    /* 标题栏 + 右上 [返回] */
    ui_render_text_cn(40, MENU_TITLE_Y, "设置");
    ui_render_button(HIT_SET_BACK_X0 + 10, 20,
                     DISP_WIDTH - HIT_SET_BACK_X0 - 20, 56, "返回", false);
    ui_render_hline(0, HIT_HEADER_H - 1, DISP_WIDTH, UI_GRAY_BLACK);

    char buf[80];

    /* 行 0：每日新词上限 [-] 值 [+] */
    int y0 = HIT_SET_ROW_Y0 + HIT_SET_ROW_DAILY * HIT_SET_ROW_H;
    ui_render_text_cn(40, y0 + 30, "每日新词上限");
    snprintf(buf, sizeof(buf), "%u", (unsigned)daily_new);
    ui_render_text_cn_center((HIT_SET_MINUS_X1 + HIT_SET_PLUS_X0) / 2, y0 + 30, buf);
    ui_render_button(HIT_SET_MINUS_X0, y0 + 20, HIT_SET_MINUS_X1 - HIT_SET_MINUS_X0, 56, "-", false);
    ui_render_button(HIT_SET_PLUS_X0, y0 + 20, HIT_SET_PLUS_X1 - HIT_SET_PLUS_X0, 56, "+", false);
    ui_render_hline(0, y0 + HIT_SET_ROW_H - 1, DISP_WIDTH, UI_GRAY_LIGHT);

    /* 行 1：默认复习模式 [模式A 看词回忆] [模式B 看义选词]，激活项反色 */
    int y1 = HIT_SET_ROW_Y0 + HIT_SET_ROW_MODE * HIT_SET_ROW_H;
    ui_render_text_cn(40, y1 + 30, "默认复习模式");
    ui_render_button(HIT_SET_MODE_A_X0, y1 + 20, HIT_SET_MODE_A_X1 - HIT_SET_MODE_A_X0 - 10,
                     56, "模式A", default_mode == 0);
    ui_render_button(HIT_SET_MODE_B_X0, y1 + 20, HIT_SET_MODE_B_X1 - HIT_SET_MODE_B_X0,
                     56, "模式B", default_mode == 1);
    ui_render_hline(0, y1 + HIT_SET_ROW_H - 1, DISP_WIDTH, UI_GRAY_LIGHT);

    /* 行 2：时间设置（FR-7c 首次手动设时）：当前时间 + 四个步进按钮 */
    int y2 = HIT_SET_ROW_Y0 + HIT_SET_ROW_TIME * HIT_SET_ROW_H;
    ui_render_text_cn(40, y2 + 4, "时间设置");
    time_t t = (time_t)now_unix;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    ui_render_text_cn(280, y2 + 4, buf);
    int by = y2 + HIT_SET_TIME_BTN_DY;
    ui_render_button(HIT_SET_TIME_HM_X0, by, HIT_SET_TIME_BTN_W, HIT_SET_TIME_BTN_H, "-1时", false);
    ui_render_button(HIT_SET_TIME_HP_X0, by, HIT_SET_TIME_BTN_W, HIT_SET_TIME_BTN_H, "+1时", false);
    ui_render_button(HIT_SET_TIME_MM_X0, by, HIT_SET_TIME_BTN_W, HIT_SET_TIME_BTN_H, "-1分", false);
    ui_render_button(HIT_SET_TIME_MP_X0, by, HIT_SET_TIME_BTN_W, HIT_SET_TIME_BTN_H, "+1分", false);
    ui_render_hline(0, y2 + HIT_SET_ROW_H - 1, DISP_WIDTH, UI_GRAY_LIGHT);

    /* 行 3：单词字号 [-] 值 [+]（×2 存储，显示为 x.x 倍） */
    int y3 = HIT_SET_ROW_Y0 + HIT_SET_ROW_WORD * HIT_SET_ROW_H;
    ui_render_text_cn(40, y3 + 30, "单词字号");
    snprintf(buf, sizeof(buf), "%u.%ux", (unsigned)(word_scale2 / 2),
             (unsigned)(word_scale2 % 2) * 5);
    ui_render_text_cn_center((HIT_SET_MINUS_X1 + HIT_SET_PLUS_X0) / 2, y3 + 30, buf);
    ui_render_button(HIT_SET_MINUS_X0, y3 + 20, HIT_SET_MINUS_X1 - HIT_SET_MINUS_X0, 56, "-", false);
    ui_render_button(HIT_SET_PLUS_X0, y3 + 20, HIT_SET_PLUS_X1 - HIT_SET_PLUS_X0, 56, "+", false);
    ui_render_hline(0, y3 + HIT_SET_ROW_H - 1, DISP_WIDTH, UI_GRAY_LIGHT);

    ui_render_text_cn_center(DISP_WIDTH / 2, HIT_MENU_BOTTOM_Y0 + 40,
                             "首次使用请先校准时间");
    ui_render_commit();
}
