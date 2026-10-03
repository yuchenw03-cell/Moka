/**
 * srs_engine.c —— SM-2 核心转移实现（docs/sm2_state_machine.md §2 T1–T6）
 *
 * 关键设计说明：
 * 1. EF 用整数定点直接运算，不经过 float：
 *    q=5 时增量 0.1-(0)*(...)=+0.10 → +10（×100 定点下精确）；
 *    q=3 时 0.1-2*(0.08+2*0.02)=-0.14 → -14（精确）；
 *    q=1（答错）固定 -0.20 → -20（精确）。
 *    三个等级的增量在 ×100 定点下都是整数，因此整数运算与浮点公式位级一致，
 *    同时避免了 PC/ESP32 浮点舍入差异（为什么这样做：FR-5 要求跨平台一致）。
 * 2. interval = round(interval × ef) 用 (interval*ef_x100 + 50)/100 实现
 *    四舍五入（round-half-up），uint64_t 中间量防溢出。
 * 3. interval 使用「本次评分更新前」的 ef：验收用例 T5 要求
 *    新词连对 3 次后 interval = round(6×2.6) = 16（此时 ef 尚未应用本次 +0.1，
 *    若先用新 ef=2.8 则会得到 17，与验收值矛盾——以 §6 验收用例为准）。
 */
#include "srs_engine.h"

#include <stdio.h>

#ifndef SRS_LOG
#define SRS_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

#define SRS_EF_MIN_X100   130   /* EF 下限 1.3 */
#define SRS_EF_INIT_X100  250   /* 新词初始 EF 2.5 */
#define SRS_SECS_PER_DAY  86400u

/* flags 位常量见 srs_engine.h（SRS_FLAG_*，与 .srs 布局共用一处定义） */

void srs_engine_rate(srs_record_t *rec, srs_quality_t q, uint32_t now)
{
    if (!rec) {
        SRS_LOG("[srs] rate: NULL rec\n");
        return;
    }

    if (q >= SRS_Q_BLURRY) {
        /* ---- 答对分支（T1/T2/T4/T5） ---- */
        rec->reps++;

        /* 先算 interval：用本次 ef 更新前的值（验收 T5 要求 16 而非 17） */
        if (rec->reps == 1) {
            rec->interval_days = 1;              /* T1/T2：第一次答对 */
        } else if (rec->reps == 2) {
            rec->interval_days = 6;              /* T4：第二次答对 */
        } else {
            /* T5：round(interval × ef)，四舍五入 */
            uint64_t num = (uint64_t)rec->interval_days * rec->ef_x100;
            rec->interval_days = (uint32_t)((num + 50u) / 100u);
        }

        /* 再算 ef：q=5 → +10，q=3 → -14（×100 定点下与浮点公式精确一致） */
        {
            int delta = (q == SRS_Q_KNOWN) ? 10 : -14;
            int ef = (int)rec->ef_x100 + delta;
            if (ef < SRS_EF_MIN_X100) ef = SRS_EF_MIN_X100;
            rec->ef_x100 = (uint16_t)ef;
        }

        /* 状态迁移：新词→学习中（T1/T2）；reps>=2 →复习中（T4/T5） */
        if (rec->state == 0) {
            rec->state = 1;
        } else if (rec->reps >= 2) {
            rec->state = 2;
        }
    } else {
        /* ---- 答错分支（T3/T6） ---- */
        int ef;
        rec->reps = 0;
        rec->interval_days = 1;
        ef = (int)rec->ef_x100 - 20;             /* ef - 0.2 */
        if (ef < SRS_EF_MIN_X100) ef = SRS_EF_MIN_X100;
        rec->ef_x100 = (uint16_t)ef;
        rec->lapses++;
        /* T3：新词答错保持新词；T6：学习中/复习中答错 →学习中 */
        if (rec->state != 0) {
            rec->state = 1;
        }
        rec->flags |= SRS_FLAG_FORCE_A;          /* 收卡时强制补一次模式A */
        if (rec->lapses >= 2) {
            rec->flags |= SRS_FLAG_DIFFICULT;    /* 生词本派生冗余位 */
        }
    }

    /* 最后写 due：now + interval×86400，不按日历日零点对齐（规范 §5） */
    rec->due_unix = now + rec->interval_days * SRS_SECS_PER_DAY;
    rec->last_review_unix = now;
    rec->last_quality = (uint8_t)q;
}

void srs_engine_clear_force_mode_a(srs_record_t *rec)
{
    if (!rec) return;
    rec->flags &= (uint8_t)~SRS_FLAG_FORCE_A;
}

int srs_engine_is_difficult(const srs_record_t *rec)
{
    if (!rec) return 0;
    return rec->lapses >= 2;
}
