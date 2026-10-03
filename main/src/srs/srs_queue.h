/**
 * srs_queue.h —— 今日队列调度（纯 C99，FR-4 §3 队列构成规则）
 * 构成：到期词（due<=now，interval 升序）+ 新词（每日上限，默认 20）。
 * 答错重插尾部（srs_queue_requeue_tail）。全部堆分配用 malloc（PC 可测）。
 */
#ifndef MOKA_SRS_QUEUE_H
#define MOKA_SRS_QUEUE_H

#include <stddef.h>
#include <stdint.h>
#include "srs_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct srs_queue srs_queue_t;   /* 不透明句柄 */

/* 从记录数组构建今日队列；daily_new=0 表示用默认值 20 */
srs_queue_t *srs_queue_build(srs_record_t *recs, size_t rec_count,
                             uint32_t now, uint16_t daily_new);

/* 取下一个词；返回 1 成功并写 *word_id_out，0 队列空 */
int  srs_queue_next(srs_queue_t *q, uint32_t *word_id_out);

/* 答错后重插队列尾部（FR-3/FR-4 T6） */
void srs_queue_requeue_tail(srs_queue_t *q, uint32_t word_id);

size_t srs_queue_remaining(const srs_queue_t *q);
void   srs_queue_free(srs_queue_t *q);

#ifdef __cplusplus
}
#endif
#endif
