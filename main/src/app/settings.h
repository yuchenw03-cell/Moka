/**
 * settings.h —— 设置项持久化（/sdcard/wordbook/moka.cfg，key=value 文本）
 * 项：daily_new（每日新词上限，默认 20）、default_mode（默认复习模式 A/B）
 */
#pragma once

#include "app_internal.h"

void settings_load(void);   /* 文件缺失/损坏时用默认值，不算错误 */
int  settings_save(void);   /* 返回 0 成功 */

/* 设置页触摸分发（由 app_fsm 在 ST_SETTINGS 态调用） */
void settings_handle_touch(const app_msg_t *m);
