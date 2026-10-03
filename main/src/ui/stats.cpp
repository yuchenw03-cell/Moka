/**
 * stats.cpp —— 统计页渲染实现（FR-6）
 * 柱状图：近 30 天每日 (reviewed + new_learned)，纯黑柱（16 灰阶中的 0x00）。
 */
#include "stats.h"

#include <stdio.h>

#include "render.h"

/* 柱状图几何（仅绘制用；命中区宏在 render.h） */
#define CHART_X0      40
#define CHART_X1      (DISP_WIDTH - 40)
#define CHART_BASE_Y  560              /* 基线 y */
#define CHART_MAX_H   220              /* 柱最大高度 */
#define CHART_DAYS    30
#define CHART_BAR_W   ((CHART_X1 - CHART_X0) / CHART_DAYS)   /* 16 */
#define CHART_BAR_PAD 4                /* 柱间留白 */

void ui_stats_render(uint32_t due_today, uint32_t new_today, uint32_t reviewed_today,
                     uint32_t streak_days,
                     const srs_stat_day_t *days, int day_count,
                     uint32_t difficult_count)
{
    ui_render_clear();

    /* 标题栏 + 右上 [返回] */
    ui_render_text_cn(40, 28, "统计");
    ui_render_button(HIT_STATS_BACK_X0 + 10, 20,
                     DISP_WIDTH - HIT_STATS_BACK_X0 - 20, 56, "返回", false);
    ui_render_hline(0, HIT_HEADER_H - 1, DISP_WIDTH, UI_GRAY_BLACK);

    char buf[80];

    /* 今日概况（数据由 app 层从队列与 .stats 汇集） */
    snprintf(buf, sizeof(buf), "今日待复习：%u 个", (unsigned)due_today);
    ui_render_text_cn(40, 140, buf);
    snprintf(buf, sizeof(buf), "今日新学：%u 个", (unsigned)new_today);
    ui_render_text_cn(40, 200, buf);
    snprintf(buf, sizeof(buf), "今日已复习：%u 个", (unsigned)reviewed_today);
    ui_render_text_cn(300, 200, buf);
    snprintf(buf, sizeof(buf), "连续打卡：%u 天", (unsigned)streak_days);
    ui_render_text_cn(40, 260, buf);

    /* 近 30 天复习量柱状图 */
    ui_render_text_cn(40, 320, "近 30 天复习量");
    if (day_count <= 0 || !days) {
        ui_render_text_cn_center(DISP_WIDTH / 2, CHART_BASE_Y - CHART_MAX_H / 2,
                                 "暂无历史记录");
    } else {
        uint32_t vmax = 1;
        for (int i = 0; i < day_count; i++) {
            uint32_t v = (uint32_t)days[i].reviewed + days[i].new_learned;
            if (v > vmax) vmax = v;
        }
        /* 不足 30 天时右对齐（最右为今天），与日历直觉一致 */
        int start = CHART_DAYS - day_count;
        for (int i = 0; i < day_count; i++) {
            uint32_t v = (uint32_t)days[i].reviewed + days[i].new_learned;
            int h = (int)(v * CHART_MAX_H / vmax);
            int x = CHART_X0 + (start + i) * CHART_BAR_W + CHART_BAR_PAD / 2;
            if (h > 0) {
                ui_render_fill_rect(x, CHART_BASE_Y - h,
                                    CHART_BAR_W - CHART_BAR_PAD, h, UI_GRAY_BLACK);
            }
        }
    }
    ui_render_hline(CHART_X0, CHART_BASE_Y, CHART_X1 - CHART_X0, UI_GRAY_BLACK);

    /* 生词本入口（lapses>=2 的词，FR-6）；为空时画灰框提示 */
    if (difficult_count > 0) {
        snprintf(buf, sizeof(buf), "生词本（%u 词）", (unsigned)difficult_count);
        ui_render_button(HIT_STATS_DIFF_X0, HIT_STATS_DIFF_Y0,
                         HIT_STATS_DIFF_X1 - HIT_STATS_DIFF_X0,
                         HIT_STATS_DIFF_Y1 - HIT_STATS_DIFF_Y0, buf, false);
    } else {
        ui_render_rect_border(HIT_STATS_DIFF_X0, HIT_STATS_DIFF_Y0,
                              HIT_STATS_DIFF_X1 - HIT_STATS_DIFF_X0,
                              HIT_STATS_DIFF_Y1 - HIT_STATS_DIFF_Y0, UI_GRAY_LIGHT);
        ui_render_text_cn_center(DISP_WIDTH / 2,
                                 (HIT_STATS_DIFF_Y0 + HIT_STATS_DIFF_Y1) / 2 - 15,
                                 "生词本为空（答错两次自动收录）");
    }
    ui_render_commit();
}
