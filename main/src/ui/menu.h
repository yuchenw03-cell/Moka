/**
 * menu.h —— 菜单页与设置页渲染（布局命中宏统一在 ui/render.h）
 * 所有函数末尾恰好一次 ui_render_commit。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 菜单列表项（当前页切片后的数据，由 app 层准备） */
typedef struct {
    const char *name;        /* 词书名 */
    uint32_t    total;       /* 总词数 */
    uint32_t    learned;     /* 已学（srs 中 state!=新词） */
    uint32_t    due;         /* 待复习（due<=now） */
    bool        current;     /* 是否为当前打开的词书 */
} ui_menu_item_t;

void ui_menu_render(const ui_menu_item_t *items, int count, int page, int pages);

/* 设置页：每日新词上限 / 默认模式 / 当前 RTC 时间（now_unix 用于显示） /
 * 单词字号（word_scale2 = 2×倍率：2=1.0x … 8=4.0x） */
void ui_settings_render(uint16_t daily_new, uint8_t default_mode, uint32_t now_unix,
                        uint8_t word_scale2);
