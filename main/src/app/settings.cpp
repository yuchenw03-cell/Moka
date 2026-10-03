/**
 * settings.cpp —— 设置项读写与设置页交互
 *
 * 文件格式（纯文本 key=value，一行一项）：
 *   daily_new=20
 *   default_mode=A
 * 手动设时（FR-7c）：对 BM8563 当前时间做 ±1h/±1m 步进调整并立即 rtc_set_unix 生效。
 */
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "rtc.h"
#include "touch.h"
#include "ui/menu.h"
#include "ui/render.h"
#include "log.h"

#define TAG "settings"

static void settings_defaults(void)
{
    g_app.daily_new    = MOKA_DAILY_NEW_DEF;
    g_app.default_mode = 0; /* 默认模式 A：今日到期词先"看词回忆"（docs/sm2 §3） */
    g_app.word_scale2  = MOKA_WORD_SCALE2_DEF;
}

static uint16_t clamp_daily(long v)
{
    if (v < MOKA_DAILY_NEW_MIN) v = MOKA_DAILY_NEW_MIN;
    if (v > MOKA_DAILY_NEW_MAX) v = MOKA_DAILY_NEW_MAX;
    return (uint16_t)v;
}

static uint8_t clamp_word_scale2(long v)
{
    if (v < MOKA_WORD_SCALE2_MIN) v = MOKA_WORD_SCALE2_MIN;
    if (v > MOKA_WORD_SCALE2_MAX) v = MOKA_WORD_SCALE2_MAX;
    return (uint8_t)v;
}

void settings_load(void)
{
    settings_defaults();

    FILE *f = fopen(MOKA_SETTINGS_PATH, "r");
    if (!f) {
        MOKA_LOGI(TAG, "设置文件不存在，使用默认值: %s", MOKA_SETTINGS_PATH);
        return;
    }
    char line[64];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *val = eq + 1;
        if (strcmp(line, "daily_new") == 0) {
            g_app.daily_new = clamp_daily(atol(val));
        } else if (strcmp(line, "default_mode") == 0) {
            g_app.default_mode = (val[0] == 'B') ? 1 : 0;
        } else if (strcmp(line, "word_scale2") == 0) {
            g_app.word_scale2 = clamp_word_scale2(atol(val));
        }
    }
    fclose(f);
    MOKA_LOGI(TAG, "设置：每日新词=%u 默认模式=%c 单词字号=%u.%ux",
              (unsigned)g_app.daily_new, g_app.default_mode ? 'B' : 'A',
              (unsigned)(g_app.word_scale2 / 2), (unsigned)(g_app.word_scale2 % 2) * 5);
}

int settings_save(void)
{
    FILE *f = fopen(MOKA_SETTINGS_PATH, "w");
    if (!f) {
        MOKA_LOGE(TAG, "设置文件写入失败: %s", MOKA_SETTINGS_PATH);
        return -1;
    }
    if (fprintf(f, "daily_new=%u\ndefault_mode=%c\nword_scale2=%u\n",
                (unsigned)g_app.daily_new, g_app.default_mode ? 'B' : 'A',
                (unsigned)g_app.word_scale2) < 0) {
        MOKA_LOGE(TAG, "设置文件 fprintf 失败");
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

/* 重绘设置页（值变化后原地刷新，整页一次提交） */
static void rerender(void)
{
    ui_settings_render(g_app.daily_new, g_app.default_mode, rtc_get_unix(),
                       g_app.word_scale2);
}

/* 手动设时：在当前 RTC 时间基础上步进 */
static void adjust_time(int32_t delta_secs)
{
    uint32_t now = rtc_get_unix();
    int64_t t = (int64_t)now + delta_secs;
    if (t < 0) t = 0;
    if (!rtc_set_unix((uint32_t)t)) {
        MOKA_LOGE(TAG, "rtc_set_unix 失败 ts=%u", (unsigned)t);
    }
}

void settings_handle_touch(const app_msg_t *m)
{
    if (m->touch_kind != TOUCH_EV_PRESS) return;   /* 只认按下，防连跳 */
    int x = m->x, y = m->y;

    /* 右上 [返回]：保存并回菜单 */
    if (y < HIT_SET_BACK_Y1 && x >= HIT_SET_BACK_X0) {
        if (settings_save() != 0) MOKA_LOGE(TAG, "返回时保存设置失败");
        app_navigate(ST_MENU);
        return;
    }

    int row = (y - HIT_SET_ROW_Y0) / HIT_SET_ROW_H;
    if (y < HIT_SET_ROW_Y0 || row >= HIT_SET_ROW_COUNT) return;

    if (row == HIT_SET_ROW_DAILY) {
        if (x >= HIT_SET_MINUS_X0 && x < HIT_SET_MINUS_X1) {
            g_app.daily_new = clamp_daily((long)g_app.daily_new - 1);
            settings_save();
            rerender();
        } else if (x >= HIT_SET_PLUS_X0 && x < HIT_SET_PLUS_X1) {
            g_app.daily_new = clamp_daily((long)g_app.daily_new + 1);
            settings_save();
            rerender();
        }
    } else if (row == HIT_SET_ROW_MODE) {
        uint8_t nm = g_app.default_mode;
        if (x >= HIT_SET_MODE_A_X0 && x < HIT_SET_MODE_A_X1) nm = 0;
        else if (x >= HIT_SET_MODE_B_X0 && x < HIT_SET_MODE_B_X1) nm = 1;
        if (nm != g_app.default_mode) {
            g_app.default_mode = nm;
            settings_save();
            rerender();
        }
    } else if (row == HIT_SET_ROW_WORD) {
        if (x >= HIT_SET_MINUS_X0 && x < HIT_SET_MINUS_X1) {
            g_app.word_scale2 = clamp_word_scale2((long)g_app.word_scale2 - 1);
            settings_save();
            rerender();
        } else if (x >= HIT_SET_PLUS_X0 && x < HIT_SET_PLUS_X1) {
            g_app.word_scale2 = clamp_word_scale2((long)g_app.word_scale2 + 1);
            settings_save();
            rerender();
        }
    } else if (row == HIT_SET_ROW_TIME) {
        /* 时间行的四个步进按钮位于行内下半区（见 ui/menu.cpp 布局） */
        int by = y - (HIT_SET_ROW_Y0 + HIT_SET_ROW_TIME * HIT_SET_ROW_H);
        if (by < HIT_SET_TIME_BTN_DY || by >= HIT_SET_TIME_BTN_DY + HIT_SET_TIME_BTN_H) return;
        if      (x >= HIT_SET_TIME_HM_X0 && x < HIT_SET_TIME_HM_X0 + HIT_SET_TIME_BTN_W) adjust_time(-3600);
        else if (x >= HIT_SET_TIME_HP_X0 && x < HIT_SET_TIME_HP_X0 + HIT_SET_TIME_BTN_W) adjust_time(+3600);
        else if (x >= HIT_SET_TIME_MM_X0 && x < HIT_SET_TIME_MM_X0 + HIT_SET_TIME_BTN_W) adjust_time(-60);
        else if (x >= HIT_SET_TIME_MP_X0 && x < HIT_SET_TIME_MP_X0 + HIT_SET_TIME_BTN_W) adjust_time(+60);
        else return;
        rerender();
    }
}
