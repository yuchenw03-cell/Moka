/**
 * power.h —— 电源管理（FR-7 + 三触唤醒）：闲置 120s 轻睡眠 +
 * 1100ms 内 3 触唤醒（GPIO48 非 RTC GPIO，深睡触摸唤醒物理不可用，改轻睡）
 */
#pragma once

#include "app_internal.h"

void power_on_activity(void);   /* 任意触摸事件时调用，重置闲置计时 */
void power_check_idle(void);    /* fsm 主循环每次迭代调用，超时则进轻睡流程 */

/* 电源键关机（任何状态生效，不返回）：保存 .srs → 渲染关机页（墨卡/MOKA）
 * → 全刷 → SD 卸载 → PMS150G 断电。断电后墨水屏常显关机页，
 * 再按电源键由硬件重新上电开机。 */
void power_shutdown(void);

/* 使断点快照失效：正常收卡/手动退出复习时调用，
 * 防止下次 EN 复位重启后错误复活已结束的会话 */
void power_snapshot_clear(void);

/* BOOT 期调用：触摸唤醒且 RTC 快照有效 → 恢复断点卡片并返回 true；
 * 否则（冷启动/快照失效）返回 false，走开机菜单 */
bool power_try_resume(void);
