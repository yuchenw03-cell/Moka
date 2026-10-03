/**
 * stats.h —— 统计页渲染（FR-6）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "srs_store.h"   /* srs_stat_day_t */

/* 统计页：今日概况 + 连续打卡 + 近 30 天复习量柱状图 + 生词本入口
 * days 按日期升序（srs_stats_load_recent 返回序），day_count <= 30 */
void ui_stats_render(uint32_t due_today, uint32_t new_today, uint32_t reviewed_today,
                     uint32_t streak_days,
                     const srs_stat_day_t *days, int day_count,
                     uint32_t difficult_count);
