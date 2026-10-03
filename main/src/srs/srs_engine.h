/**
 * srs_engine.h —— SM-2 引擎（纯 C99，规范 §五）
 *
 * 约束：
 * - 不 include 任何 ESP-IDF / Arduino / M5 头文件；
 * - 不读 RTC——now 由调用方注入（单元测试可伪造时间）；
 * - 除 malloc 外无动态行为，PC gcc 可直接编译单测。
 *
 * 为什么 ef 用 ×100 定点（ef_x100）：跨平台位级一致，
 * 与 .srs 文件布局一一对应（docs/srs_persistence.md §2）。
 */
#ifndef MOKA_SRS_ENGINE_H
#define MOKA_SRS_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 自评等级（FR-4 锁定，不扩展） */
typedef enum {
    SRS_Q_FORGOT  = 1,   /* 忘记 */
    SRS_Q_BLURRY  = 3,   /* 模糊 */
    SRS_Q_KNOWN   = 5    /* 认识 */
} srs_quality_t;

/* 学习记录 flags 位定义（.srs 64B 记录 flags 字段，docs/srs_persistence.md §2） */
#define SRS_FLAG_REQUEUED   0x01   /* bit0 今日队列重插中 */
#define SRS_FLAG_FORCE_A    0x02   /* bit1 收卡前强制模式A */
#define SRS_FLAG_DIFFICULT  0x04   /* bit2 生词本（lapses>=2 派生） */

/* 学习记录（内存态，与 .srs 文件 64B 记录对应，结构见 docs/srs_persistence.md） */
typedef struct {
    uint32_t word_id;
    uint16_t ef_x100;         /* EF×100 定点，初始 250 */
    uint16_t reps;
    uint32_t interval_days;
    uint32_t due_unix;
    uint32_t last_review_unix;
    uint16_t lapses;
    uint8_t  state;           /* 0=新词 1=学习中 2=复习中 3=搁置 */
    uint8_t  last_quality;
    uint8_t  flags;           /* bit0=队列重插 bit1=收卡强制模式A bit2=生词本 */
    uint8_t  mode_hint;       /* 0=A 1=B */
} srs_record_t;

/* 核心转移（docs/sm2_state_machine.md §2 的 T1–T6） */
void srs_engine_rate(srs_record_t *rec, srs_quality_t q, uint32_t now);

/* 收卡补救：消费 flags.bit1 的强制模式A（模式 B 答错触发，FR-3） */
void srs_engine_clear_force_mode_a(srs_record_t *rec);

/* 生词本判定（lapses>=2 时置 flags.bit2） */
int  srs_engine_is_difficult(const srs_record_t *rec);

#ifdef __cplusplus
}
#endif
#endif /* MOKA_SRS_ENGINE_H */
