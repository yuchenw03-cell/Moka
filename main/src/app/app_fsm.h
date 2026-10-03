/**
 * app_fsm.h —— 顶层状态机接口（阶段 4 起实装）
 * 状态：开机菜单 → 复习(模式A/模式B) → 统计页 → 休眠
 * 消息队列驱动，队列深度 16（M5ReadPaper 的 10 深度在高并发下丢消息，
 * 故放宽；仍在单任务内串行消费，无锁）。
 */
#ifndef MOKA_APP_FSM_H
#define MOKA_APP_FSM_H

#ifdef __cplusplus
extern "C" {
#endif

void app_fsm_start(void);   /* 创建主状态机任务（core 1） */

#ifdef __cplusplus
}
#endif
#endif
