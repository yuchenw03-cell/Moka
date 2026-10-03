/**
 * srs_queue.c —— 今日队列调度（docs/sm2_state_machine.md §3）
 *
 * 构成：到期词（state≠新词 且 due<=now，按 interval 升序，同值按 word_id 升序
 * 保证确定性）+ 新词（按调用方传入数组顺序取前 daily_new 个，0=默认 20）。
 * 答错重插队尾：追加到动态数组末尾；容量不足时 malloc 新缓冲 + memcpy + free
 * 旧缓冲（约束只允许 malloc/free，不用 realloc）。
 */
#include "srs_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SRS_LOG
#define SRS_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#define SRS_DAILY_NEW_DEFAULT 20

struct srs_queue {
    uint32_t *ids;     /* word_id 序列 */
    size_t    count;   /* 有效元素数（含已消费的） */
    size_t    head;    /* 下一个待出队下标 */
    size_t    cap;     /* ids 分配容量 */
};

/* qsort 用的到期词排序键 */
typedef struct {
    uint32_t interval;
    uint32_t word_id;
} due_key_t;

static int due_cmp(const void *a, const void *b)
{
    const due_key_t *x = a, *y = b;
    if (x->interval != y->interval)
        return (x->interval < y->interval) ? -1 : 1;
    if (x->word_id != y->word_id)
        return (x->word_id < y->word_id) ? -1 : 1;
    return 0;
}

/* 扩容：只走 malloc + memcpy + free */
static int queue_grow(srs_queue_t *q, size_t need)
{
    size_t newcap = q->cap ? q->cap : 16;
    uint32_t *nids;
    while (newcap < need) newcap *= 2;
    nids = (uint32_t *)malloc(newcap * sizeof(uint32_t));
    if (!nids) {
        SRS_LOG("[srs] queue grow: OOM\n");
        return -1;
    }
    /* 顺手把已消费的头部压缩掉，降低内存占用 */
    if (q->count > q->head)
        memcpy(nids, q->ids + q->head, (q->count - q->head) * sizeof(uint32_t));
    q->count -= q->head;
    q->head = 0;
    free(q->ids);
    q->ids = nids;
    q->cap = newcap;
    return 0;
}

srs_queue_t *srs_queue_build(srs_record_t *recs, size_t rec_count,
                             uint32_t now, uint16_t daily_new)
{
    srs_queue_t *q;
    due_key_t *due = NULL;
    size_t due_n = 0, i, pos = 0;
    size_t new_limit = daily_new ? daily_new : SRS_DAILY_NEW_DEFAULT;

    q = (srs_queue_t *)malloc(sizeof(*q));   /* 约束仅允许 malloc/free */
    if (!q) {
        SRS_LOG("[srs] queue build: OOM\n");
        return NULL;
    }
    memset(q, 0, sizeof(*q));
    if (rec_count == 0 || !recs) return q;   /* 空队列 */

    q->ids = (uint32_t *)malloc(rec_count * sizeof(uint32_t));
    due = (due_key_t *)malloc(rec_count * sizeof(due_key_t));
    if (!q->ids || !due) {
        SRS_LOG("[srs] queue build: OOM\n");
        free(q->ids); free(due); free(q);
        return NULL;
    }
    q->cap = rec_count;

    /* 1) 收集到期词并排序（interval 升序） */
    for (i = 0; i < rec_count; i++) {
        if (recs[i].state != 0 && recs[i].due_unix <= now) {
            due[due_n].interval = recs[i].interval_days;
            due[due_n].word_id = recs[i].word_id;
            due_n++;
        }
    }
    qsort(due, due_n, sizeof(due_key_t), due_cmp);
    for (i = 0; i < due_n; i++) q->ids[pos++] = due[i].word_id;

    /* 2) 新词：按数组顺序取前 new_limit 个 */
    for (i = 0; i < rec_count && new_limit > 0; i++) {
        if (recs[i].state == 0) {
            q->ids[pos++] = recs[i].word_id;
            new_limit--;
        }
    }

    q->count = pos;
    q->head = 0;
    free(due);
    return q;
}

int srs_queue_next(srs_queue_t *q, uint32_t *word_id_out)
{
    if (!q || !word_id_out) return 0;
    if (q->head >= q->count) {
        /* 消费完后复位，便于复用与重插 */
        q->head = q->count = 0;
        return 0;
    }
    *word_id_out = q->ids[q->head++];
    return 1;
}

void srs_queue_requeue_tail(srs_queue_t *q, uint32_t word_id)
{
    if (!q) return;
    if (q->count >= q->cap && queue_grow(q, q->count + 1) != 0)
        return;   /* OOM 时丢弃重插，记日志即可（queue_grow 已记） */
    q->ids[q->count++] = word_id;
}

size_t srs_queue_remaining(const srs_queue_t *q)
{
    if (!q || q->head >= q->count) return 0;
    return q->count - q->head;
}

void srs_queue_free(srs_queue_t *q)
{
    if (!q) return;
    free(q->ids);
    free(q);
}
