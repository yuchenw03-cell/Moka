/**
 * review.h —— 复习流程编排（模式 A / 模式 B / 生词本 / 收卡补救轮）
 */
#pragma once

#include "app_internal.h"

/* 开启一场复习；队列为空/无可复习内容时切到提示页并返回 false */
bool review_start(review_kind_t kind);

/* REVIEW_FRONT / REVIEW_BACK 态的触摸分发（由 app_fsm 调用） */
void review_handle_touch(const app_msg_t *m);

/* 唤醒恢复（电源键 EN 复位重启后的断点续读，FR-7a）：
 * 重建今日队列并把断点词作为当前卡片 */
bool review_resume_card(uint32_t word_id);

/* 释放会话资源（队列/id_list/卡片文本），词书保持打开 */
void review_cleanup(void);
