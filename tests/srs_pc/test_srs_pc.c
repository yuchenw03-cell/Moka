/**
 * test_srs_pc.c —— 墨卡 SM-2 调度引擎 PC 单测（阶段 2 验收）
 *
 * 覆盖 docs/sm2_state_machine.md §6 全部验收映射：
 *   case 1  T1 新词认识        case 2  T2 新词模糊
 *   case 3  T3/T6 答错         case 4  T4 第二次答对
 *   case 5  T5 第三次答对      case 6  EF 下限
 *   case 7  队列构成           case 8  答错重插队尾
 *   case 9  持久化 + 断电恢复  case 10 模式B干扰项零跨桶
 *   case 11 stats streak
 * 每个用例打印 PASS/FAIL 与实际值；任一失败则进程退出码非 0。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "srs_engine.h"
#include "srs_queue.h"
#include "srs_store.h"
#include "wbx.h"

static int g_fail = 0;

#define CHECK(case_no, cond, ...) do {                                  \
        if (!(cond)) {                                                  \
            g_fail++;                                                   \
            printf("  FAIL case %d: ", case_no);                        \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
        }                                                               \
    } while (0)

/* 新词记录初始化 */
static srs_record_t mk_new(uint32_t id)
{
    srs_record_t r;
    memset(&r, 0, sizeof(r));
    r.word_id = id;
    r.ef_x100 = 250;
    r.state = 0;
    return r;
}

static int rec_eq(const srs_record_t *a, const srs_record_t *b)
{
    return a->word_id == b->word_id && a->ef_x100 == b->ef_x100 &&
           a->reps == b->reps && a->interval_days == b->interval_days &&
           a->due_unix == b->due_unix &&
           a->last_review_unix == b->last_review_unix &&
           a->lapses == b->lapses && a->state == b->state &&
           a->last_quality == b->last_quality && a->flags == b->flags &&
           a->mode_hint == b->mode_hint;
}

static const srs_record_t *find_rec(const srs_record_t *r, size_t n, uint32_t id)
{
    size_t i;
    for (i = 0; i < n; i++) if (r[i].word_id == id) return &r[i];
    return NULL;
}

/* ---------- case 1: T1 新词认识 ---------- */
static void case1(void)
{
    int before = g_fail;
    uint32_t now = 1700000000u;
    srs_record_t r = mk_new(1);
    srs_engine_rate(&r, SRS_Q_KNOWN, now);
    CHECK(1, r.reps == 1, "reps=%u 期望1", r.reps);
    CHECK(1, r.interval_days == 1, "interval=%u 期望1", r.interval_days);
    CHECK(1, r.ef_x100 == 260, "ef=%u 期望260", r.ef_x100);
    CHECK(1, r.state == 1, "state=%u 期望1(学习中)", r.state);
    CHECK(1, r.due_unix == now + 86400u, "due=%u 期望%u", r.due_unix, now + 86400u);
    CHECK(1, r.last_review_unix == now, "last_review=%u 期望%u", r.last_review_unix, now);
    CHECK(1, r.last_quality == 5, "last_q=%u 期望5", r.last_quality);
    if (g_fail == before) printf("PASS case 1: T1 新词认识 (reps=1 interval=1 ef=250->260)\n");
}

/* ---------- case 2: T2 新词模糊 ---------- */
static void case2(void)
{
    int before = g_fail;
    uint32_t now = 1700000000u;
    srs_record_t r = mk_new(2);
    srs_engine_rate(&r, SRS_Q_BLURRY, now);
    CHECK(2, r.reps == 1, "reps=%u 期望1", r.reps);
    CHECK(2, r.interval_days == 1, "interval=%u 期望1", r.interval_days);
    CHECK(2, r.ef_x100 == 236, "ef=%u 期望236", r.ef_x100);
    CHECK(2, r.state == 1, "state=%u 期望1", r.state);
    CHECK(2, r.due_unix == now + 86400u, "due=%u", r.due_unix);
    if (g_fail == before) printf("PASS case 2: T2 新词模糊 (ef=250->236 interval=1)\n");
}

/* ---------- case 3: T3 新词忘记 + T6 复习中忘记 ---------- */
static void case3(void)
{
    int before = g_fail;
    uint32_t now = 1700000000u;
    srs_record_t r = mk_new(3);

    /* T3：新词忘记 */
    srs_engine_rate(&r, SRS_Q_FORGOT, now);
    CHECK(3, r.reps == 0, "T3 reps=%u 期望0", r.reps);
    CHECK(3, r.interval_days == 1, "T3 interval=%u 期望1", r.interval_days);
    CHECK(3, r.ef_x100 == 230, "T3 ef=%u 期望230", r.ef_x100);
    CHECK(3, r.lapses == 1, "T3 lapses=%u 期望1", r.lapses);
    CHECK(3, r.state == 0, "T3 state=%u 期望0(保持新词)", r.state);
    CHECK(3, (r.flags & SRS_FLAG_FORCE_A) != 0, "T3 flags.bit1 未置位 flags=0x%02X", r.flags);
    CHECK(3, r.due_unix == now + 86400u, "T3 due=%u", r.due_unix);

    /* T6：复习中忘记（reps=3 state=2） */
    memset(&r, 0, sizeof(r));
    r.word_id = 33; r.ef_x100 = 250; r.reps = 3; r.interval_days = 16; r.state = 2;
    srs_engine_rate(&r, SRS_Q_FORGOT, now);
    CHECK(3, r.reps == 0, "T6 reps=%u 期望0", r.reps);
    CHECK(3, r.interval_days == 1, "T6 interval=%u 期望1", r.interval_days);
    CHECK(3, r.ef_x100 == 230, "T6 ef=%u 期望230", r.ef_x100);
    CHECK(3, r.lapses == 1, "T6 lapses=%u 期望1", r.lapses);
    CHECK(3, r.state == 1, "T6 state=%u 期望1(学习中)", r.state);
    CHECK(3, (r.flags & SRS_FLAG_FORCE_A) != 0, "T6 flags.bit1 未置位");

    /* 收卡消费 bit1 */
    srs_engine_clear_force_mode_a(&r);
    CHECK(3, (r.flags & SRS_FLAG_FORCE_A) == 0, "clear_force_mode_a 后 bit1 仍置位");

    /* lapses>=2 → 生词本 */
    memset(&r, 0, sizeof(r));
    r.word_id = 34; r.ef_x100 = 250; r.reps = 2; r.state = 2;
    srs_engine_rate(&r, SRS_Q_FORGOT, now);
    srs_engine_rate(&r, SRS_Q_FORGOT, now + 100);
    CHECK(3, r.lapses == 2 && (r.flags & SRS_FLAG_DIFFICULT) != 0 && srs_engine_is_difficult(&r),
          "lapses=%u flags=0x%02X 期望生词本", r.lapses, r.flags);

    if (g_fail == before)
        printf("PASS case 3: T3/T6 答错 (reps=0 interval=1 ef=250->230 lapses+1 bit1 置位/消费)\n");
}

/* ---------- case 4: T4 第二次答对 interval 1->6 ---------- */
static void case4(void)
{
    int before = g_fail;
    uint32_t t0 = 1700000000u, t1 = t0 + 86400u;
    srs_record_t r = mk_new(4);
    srs_engine_rate(&r, SRS_Q_KNOWN, t0);   /* T1 */
    srs_engine_rate(&r, SRS_Q_KNOWN, t1);   /* T4 */
    CHECK(4, r.reps == 2, "reps=%u 期望2", r.reps);
    CHECK(4, r.interval_days == 6, "interval=%u 期望6", r.interval_days);
    CHECK(4, r.ef_x100 == 270, "ef=%u 期望270", r.ef_x100);
    CHECK(4, r.state == 2, "state=%u 期望2(复习中)", r.state);
    CHECK(4, r.due_unix == t1 + 6u * 86400u, "due=%u 期望%u", r.due_unix, t1 + 6u * 86400u);
    if (g_fail == before) printf("PASS case 4: T4 第二次答对 (interval 1->6)\n");
}

/* ---------- case 5: T5 第三次答对 interval=round(6×2.6)=16 ---------- */
static void case5(void)
{
    int before = g_fail;
    uint32_t t0 = 1700000000u;
    srs_record_t r = mk_new(5);
    srs_engine_rate(&r, SRS_Q_KNOWN, t0);               /* T1: interval 1 */
    srs_engine_rate(&r, SRS_Q_KNOWN, t0 + 86400u);      /* T4: interval 6 */
    srs_engine_rate(&r, SRS_Q_KNOWN, t0 + 7u * 86400u); /* T5: round(6×ef) */
    CHECK(5, r.reps == 3, "reps=%u 期望3", r.reps);
    CHECK(5, r.interval_days == 16, "interval=%u 期望16 (=round(6×2.6))", r.interval_days);
    CHECK(5, r.state == 2, "state=%u 期望2", r.state);
    if (g_fail == before)
        printf("PASS case 5: T5 第三次答对 (interval 1->6->16, 实际=%u)\n", r.interval_days);
}

/* ---------- case 6: EF 下限 ---------- */
static void case6(void)
{
    int before = g_fail;
    uint32_t now = 1700000000u;
    int i;
    srs_record_t r = mk_new(6);
    for (i = 0; i < 20; i++)
        srs_engine_rate(&r, SRS_Q_FORGOT, now + (uint32_t)i * 100u);
    CHECK(6, r.ef_x100 == 130, "ef=%u 期望130(下限)", r.ef_x100);
    CHECK(6, r.lapses == 20, "lapses=%u 期望20", r.lapses);
    CHECK(6, r.reps == 0, "reps=%u 期望0", r.reps);
    if (g_fail == before) printf("PASS case 6: EF 下限 (连错20次 ef=%u>=130)\n", r.ef_x100);
}

/* ---------- case 7: 队列构成（到期 interval 升序 + 新词上限） ---------- */
static void case7(void)
{
    int before = g_fail;
    uint32_t now = 2000u;
    srs_record_t recs[34];
    srs_queue_t *q;
    uint32_t id;
    size_t i;
    int ok;

    memset(recs, 0, sizeof(recs));
    recs[0] = mk_new(1); recs[0].state = 2; recs[0].interval_days = 9; recs[0].due_unix = 1000;
    recs[1] = mk_new(2); recs[1].state = 1; recs[1].interval_days = 2; recs[1].due_unix = 500;
    recs[2] = mk_new(3); recs[2].state = 2; recs[2].interval_days = 5; recs[2].due_unix = 900;
    recs[3] = mk_new(4); recs[3].state = 2; recs[3].interval_days = 7; recs[3].due_unix = 99999; /* 未到期 */
    for (i = 0; i < 30; i++) recs[4 + i] = mk_new(100 + (uint32_t)i);

    /* 默认上限 20（daily_new=0） */
    q = srs_queue_build(recs, 34, now, 0);
    CHECK(7, q != NULL, "build 返回 NULL");
    CHECK(7, srs_queue_remaining(q) == 23, "remaining=%zu 期望23(3到期+20新词)",
          srs_queue_remaining(q));
    ok = srs_queue_next(q, &id) && id == 2;   /* interval 2 最先 */
    ok = ok && srs_queue_next(q, &id) && id == 3;   /* interval 5 */
    ok = ok && srs_queue_next(q, &id) && id == 1;   /* interval 9 */
    CHECK(7, ok, "到期词顺序非 interval 升序 (期望 2,3,1)");
    ok = srs_queue_next(q, &id) && id == 100;       /* 到期词做完才做新词 */
    CHECK(7, ok, "首个新词 id=%u 期望100", id);
    /* 抽干：新词只能有 20 个（100..119），第 24 个 next 应失败 */
    {
        size_t cnt = 4;   /* 已消费 3 个到期 + 1 个新词 */
        while (srs_queue_next(q, &id)) cnt++;
        CHECK(7, cnt == 23, "队列总数=%zu 期望23（新词上限20生效）", cnt);
        CHECK(7, id == 119, "最后一个新词 id=%u 期望119（词120+ 被上限截断）", id);
    }
    srs_queue_free(q);

    /* 自定义上限 3 */
    q = srs_queue_build(recs, 34, now, 3);
    CHECK(7, srs_queue_remaining(q) == 6, "daily_new=3 remaining=%zu 期望6",
          srs_queue_remaining(q));
    srs_queue_free(q);

    if (g_fail == before)
        printf("PASS case 7: 队列构成 (到期按 interval 升序 2,3,1；新词默认上限20/自定义3 生效)\n");
}

/* ---------- case 8: 答错重插队尾 ---------- */
static void case8(void)
{
    int before = g_fail;
    srs_record_t recs[3];
    srs_queue_t *q;
    uint32_t id, first, last = 0;
    uint32_t seq[4];
    int n = 0;

    recs[0] = mk_new(11); recs[0].state = 2; recs[0].interval_days = 1; recs[0].due_unix = 0;
    recs[1] = mk_new(22); recs[1].state = 2; recs[1].interval_days = 2; recs[1].due_unix = 0;
    recs[2] = mk_new(33); recs[2].state = 2; recs[2].interval_days = 3; recs[2].due_unix = 0;

    q = srs_queue_build(recs, 3, 10, 0);
    CHECK(8, srs_queue_next(q, &first) && first == 11, "首词=%u 期望11", first);
    srs_queue_requeue_tail(q, first);   /* 答错重插 */
    while (srs_queue_next(q, &id) && n < 4) { seq[n++] = id; last = id; }
    CHECK(8, n == 3, "重插后总出队=%d 期望3", n);
    CHECK(8, n == 3 && seq[0] == 22 && seq[1] == 33 && seq[2] == 11,
          "重插后顺序=%u,%u,%u 期望22,33,11", seq[0], seq[1], seq[2]);
    CHECK(8, last == first, "重插词未在队尾 last=%u", last);
    CHECK(8, srs_queue_remaining(q) == 0, "remaining=%zu 期望0", srs_queue_remaining(q));
    CHECK(8, srs_queue_next(q, &id) == 0, "空队列 next 应返回0");
    srs_queue_free(q);

    if (g_fail == before)
        printf("PASS case 8: 答错重插队尾 (顺序 22,33,11，重插词 %u 在最后)\n", first);
}

/* ---------- case 9: 持久化 append/compact/断电恢复 ---------- */
static void case9(void)
{
    int before = g_fail;
    const char *P = "t9.srs";
    srs_record_t mem[3];
    srs_record_t *ld = NULL;
    size_t cnt = 0;
    uint32_t t = 1700000000u;
    int i, jl = 0;
    FILE *f;

    remove(P); remove("t9.srs.tmp"); remove("t9tmp_src.srs"); remove("t9tmp_src.srs.tmp");

    mem[0] = mk_new(10);
    mem[1] = mk_new(20);
    mem[2] = mk_new(30);

    /* 初始合并建文件 */
    CHECK(9, srs_store_compact(P, mem, 3) == 0, "初始 compact 失败");

    /* 两次评分 + 追加日志，验证回放 */
    srs_engine_rate(&mem[0], SRS_Q_KNOWN, t);
    jl = srs_store_append_journal(P, &mem[0]);
    CHECK(9, jl == 1, "journal_len=%d 期望1", jl);
    srs_engine_rate(&mem[0], SRS_Q_KNOWN, t + 50);
    jl = srs_store_append_journal(P, &mem[0]);
    CHECK(9, jl == 2, "journal_len=%d 期望2", jl);

    CHECK(9, srs_store_load(P, &ld, &cnt) == 0 && cnt == 3,
          "load 失败 cnt=%zu", cnt);
    if (ld) {
        const srs_record_t *w10 = find_rec(ld, cnt, 10);
        CHECK(9, w10 && rec_eq(w10, &mem[0]),
              "日志回放后与内存不一致 (reps=%u interval=%u 期望2/6)",
              w10 ? w10->reps : 0, w10 ? w10->interval_days : 0);
        free(ld); ld = NULL;
    }

    /* 再补 18 条，第 20 条触发合并 */
    for (i = 2; i < SRS_STORE_JOURNAL_LIMIT; i++) {
        srs_engine_rate(&mem[0], SRS_Q_KNOWN, t + (uint32_t)i * 50u);
        jl = srs_store_append_journal(P, &mem[0]);
        CHECK(9, jl == i + 1, "journal_len=%d 期望%d", jl, i + 1);
    }
    CHECK(9, jl == SRS_STORE_JOURNAL_LIMIT, "满20未达 journal_len=%d", jl);
    CHECK(9, srs_store_compact(P, mem, 3) == 0, "满20 compact 失败");

    /* compact 后读回与内存态一致 */
    CHECK(9, srs_store_load(P, &ld, &cnt) == 0 && cnt == 3, "compact 后 load 失败");
    if (ld) {
        int all = 1, k;
        for (k = 0; k < 3; k++) {
            const srs_record_t *w = find_rec(ld, cnt, mem[k].word_id);
            if (!w || !rec_eq(w, &mem[k])) all = 0;
        }
        CHECK(9, all, "compact 后读回与内存态不一致");
        free(ld); ld = NULL;
    }
    jl = srs_store_append_journal(P, &mem[1]);
    CHECK(9, jl == 1, "compact 后 journal_len 未归零 (append 返回 %d)", jl);

    /* 断电模拟 A：tmp 写完未 rename（合并中途断电），recover_tmp 应转正 */
    mem[1].ef_x100 = 280;
    CHECK(9, srs_store_compact("t9tmp_src.srs", mem, 3) == 0, "构造 tmp 镜像失败");
    CHECK(9, rename("t9tmp_src.srs", "t9.srs.tmp") == 0, "rename 构造断电现场失败");
    CHECK(9, srs_store_recover_tmp(P) == 1, "recover_tmp 未转正合法 tmp");
    CHECK(9, srs_store_load(P, &ld, &cnt) == 0 && cnt == 3, "转正后 load 失败");
    if (ld) {
        const srs_record_t *w20 = find_rec(ld, cnt, 20);
        CHECK(9, w20 && w20->ef_x100 == 280, "转正后 ef=%u 期望280",
              w20 ? w20->ef_x100 : 0);
        free(ld); ld = NULL;
    }

    /* 断电模拟 B：tmp 损坏，recover_tmp 应丢弃并保留旧 .srs */
    f = fopen("t9.srs.tmp", "wb");
    CHECK(9, f != NULL, "构造损坏 tmp 失败");
    if (f) {
        uint8_t junk[100];
        memset(junk, 0xA5, sizeof(junk));
        fwrite(junk, 1, sizeof(junk), f);
        fclose(f);
    }
    CHECK(9, srs_store_recover_tmp(P) == -1, "损坏 tmp 未被丢弃");
    f = fopen("t9.srs.tmp", "rb");
    CHECK(9, f == NULL, "损坏 tmp 未被删除");
    if (f) fclose(f);
    CHECK(9, srs_store_load(P, &ld, &cnt) == 0 && cnt == 3, "丢弃 tmp 后 load 失败");
    if (ld) {
        const srs_record_t *w20 = find_rec(ld, cnt, 20);
        CHECK(9, w20 && w20->ef_x100 == 280, "丢弃 tmp 后旧数据丢失 ef=%u",
              w20 ? w20->ef_x100 : 0);
        free(ld); ld = NULL;
    }

    remove(P); remove("t9.srs.tmp"); remove("t9tmp_src.srs"); remove("t9tmp_src.srs.tmp");
    if (g_fail == before)
        printf("PASS case 9: 持久化 (append 20 条触发 compact；断电 tmp 转正/丢弃；读回==内存态)\n");
}

/* ---------- case 10: 模式B干扰项（100 次抽取跨桶=0） ---------- */

/* 测试内自带的 CRC32（与生成端 zlib.crc32 一致），用于构造 fixture */
static uint32_t t_crc32(const void *data, size_t len)
{
    static uint32_t tab[256];
    static int ready = 0;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu, i;
    int k;
    if (!ready) {
        for (i = 0; i < 256; i++) {
            uint32_t x = i;
            for (k = 0; k < 8; k++)
                x = (x & 1) ? (0xEDB88320u ^ (x >> 1)) : (x >> 1);
            tab[i] = x;
        }
        ready = 1;
    }
    for (i = 0; i < len; i++) c = tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* 构造 12 词 fixture：id 0-5 桶3，id 6-11 桶7；distract 同桶环形填充。
   词 0 的 distract[0] 故意埋一个跨桶 id=8，验证固件端过滤。 */
static int make_wbx_fixture(const char *path)
{
    wbx_header_t h;
    wbx_entry_t e[12];
    FILE *f;
    long hdr_pos, i;
    uint32_t text_ofs_run = 0;
    long fsize;

    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "WBX1", 4);
    h.version = 1;
    h.word_count = 12;
    h.meta_ofs = 0x40;
    h.text_ofs = 0x40 + 12 * (uint32_t)sizeof(wbx_entry_t);
    {
        const char *nm = "测试词书";
        h.name_len = (uint32_t)strlen(nm) + 1;
        memcpy(h.name, nm, h.name_len);
    }

    memset(e, 0, sizeof(e));
    for (i = 0; i < 12; i++) {
        char word[16], gloss[32];
        int d;
        snprintf(word, sizeof(word), "w%02d", (int)i);
        snprintf(gloss, sizeof(gloss), "gloss%02d", (int)i);
        e[i].word_id = (uint32_t)i;
        e[i].freq_bucket = (i < 6) ? 3 : 7;
        e[i].pos = 1;
        e[i].word_len = (uint16_t)(strlen(word) + 1);
        e[i].phon_len = 0;
        e[i].gloss_len = (uint16_t)(strlen(gloss) + 1);
        e[i].ex_len = 0;
        e[i].text_ofs = text_ofs_run;
        text_ofs_run += e[i].word_len + e[i].phon_len + e[i].gloss_len + e[i].ex_len;
        /* 同桶 5 个干扰项（每桶恰好 6 词，其余 5 个全填） */
        for (d = 0; d < 5; d++) {
            uint32_t base = (i < 6) ? 0 : 6;
            uint32_t rel = ((uint32_t)i - base + 1 + (uint32_t)d) % 6;
            e[i].distract[d] = base + rel;
        }
    }
    e[0].distract[0] = 8;   /* 埋一个跨桶 id，验证固件端过滤 */

    f = fopen(path, "w+b");   /* 需要回读算 CRC，不能只写模式 */
    if (!f) return -1;
    hdr_pos = ftell(f);
    if (fwrite(&h, sizeof(h), 1, f) != 1) { fclose(f); return -1; }
    if (fwrite(e, sizeof(e), 1, f) != 1) { fclose(f); return -1; }
    for (i = 0; i < 12; i++) {
        char word[16], gloss[32];
        snprintf(word, sizeof(word), "w%02d", (int)i);
        snprintf(gloss, sizeof(gloss), "gloss%02d", (int)i);
        if (fwrite(word, 1, e[i].word_len, f) != e[i].word_len) { fclose(f); return -1; }
        if (fwrite(gloss, 1, e[i].gloss_len, f) != e[i].gloss_len) { fclose(f); return -1; }
    }
    /* 回填 CRC32（crc 字段置 0 计算） */
    fsize = ftell(f);
    {
        uint8_t *img = (uint8_t *)malloc((size_t)fsize);
        uint32_t crc;
        if (!img) { fclose(f); return -1; }
        if (fseek(f, 0, SEEK_SET) != 0 ||
            fread(img, 1, (size_t)fsize, f) != (size_t)fsize) {
            free(img); fclose(f); return -1;
        }
        crc = t_crc32(img, (size_t)fsize);
        free(img);
        if (fseek(f, hdr_pos + 0x38, SEEK_SET) != 0 ||
            fwrite(&crc, 4, 1, f) != 1) { fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

static void case10(void)
{
    int before = g_fail;
    const char *P = "t10_fixture.wbx";
    wbx_book_t *b;
    wbx_entry_t e;
    char word[64], gloss[64];
    int iter, cross = 0, picks_total = 0;

    remove(P); remove("t10_bad.wbx");
    CHECK(10, make_wbx_fixture(P) == 0, "fixture 生成失败");

    b = wbx_open(P);
    CHECK(10, b != NULL, "wbx_open 失败（CRC 自检不过？）");
    if (!b) { printf("  FAIL case 10: 无法继续\n"); return; }

    CHECK(10, wbx_word_count(b) == 12, "word_count=%u 期望12", wbx_word_count(b));
    CHECK(10, strcmp(wbx_book_name(b), "测试词书") == 0, "name=%s", wbx_book_name(b));

    /* 定点读索引 + 文本区 */
    CHECK(10, wbx_read_entry(b, 3, &e) == 0 && e.word_id == 3 && e.freq_bucket == 3,
          "read_entry(3) 失败");
    CHECK(10, wbx_read_texts(b, &e, word, NULL, gloss, NULL) == 0 &&
          strcmp(word, "w03") == 0 && strcmp(gloss, "gloss03") == 0,
          "read_texts 内容错误 word=%s gloss=%s", word, gloss);
    CHECK(10, wbx_find_entry(b, 7, &e) == 0 && e.freq_bucket == 7,
          "find_entry(7) 失败或桶错误");

    /* 100 次抽取，跨桶次数必须为 0（验收标准） */
    for (iter = 0; iter < 100; iter++) {
        uint32_t ids[3] = {0, 0, 0};
        wbx_entry_t main_e, de;
        int np, k;
        CHECK(10, wbx_read_entry(b, (uint32_t)(iter % 12), &main_e) == 0,
              "read_entry iter=%d 失败", iter);
        np = wbx_pick_distractors(b, &main_e, ids);
        picks_total += np;
        for (k = 0; k < np; k++) {
            if (wbx_find_entry(b, ids[k], &de) != 0 ||
                de.freq_bucket != main_e.freq_bucket)
                cross++;
        }
    }
    CHECK(10, cross == 0, "100 次抽取跨桶次数=%d 期望0（验收不通过）", cross);
    CHECK(10, picks_total == 300, "总抽取数=%d 期望300", picks_total);
    wbx_close(b);

    /* 损坏文件必须拒绝打开 */
    {
        FILE *fb = fopen(P, "rb");
        FILE *fo;
        long sz;
        uint8_t *img;
        int bad_rejected = 0;
        if (fb) {
            fseek(fb, 0, SEEK_END); sz = ftell(fb); fseek(fb, 0, SEEK_SET);
            img = (uint8_t *)malloc((size_t)sz);
            if (img && fread(img, 1, (size_t)sz, fb) == (size_t)sz) {
                img[sz - 1] ^= 0xFF;   /* 翻转最后一个字节破坏 CRC */
                fo = fopen("t10_bad.wbx", "wb");
                if (fo) {
                    fwrite(img, 1, (size_t)sz, fo);
                    fclose(fo);
                    bad_rejected = (wbx_open("t10_bad.wbx") == NULL);
                }
            }
            free(img);
            fclose(fb);
        }
        CHECK(10, bad_rejected, "CRC 损坏的 .wbx 未被拒绝");
    }

    remove(P); remove("t10_bad.wbx");
    if (g_fail == before)
        printf("PASS case 10: 模式B干扰项 (100 次抽取跨桶=0；含故意埋雷槽被过滤)\n");
}

/* ---------- case 11: stats streak 连续/断签 ---------- */
static void case11(void)
{
    int before = g_fail;
    const char *P = "t11.stats";
    uint32_t d0 = 1780000000u;
    srs_stat_day_t days[30];
    int n;

    remove(P);
    /* 第 1 天：1 新词答对 + 1 复习答错 */
    CHECK(11, srs_stats_update_today(P, d0, 1, SRS_Q_KNOWN) == 0, "day1 update1 失败");
    CHECK(11, srs_stats_update_today(P, d0, 0, SRS_Q_FORGOT) == 0, "day1 update2 失败");
    /* 第 2 天（连续）：1 复习答对 */
    CHECK(11, srs_stats_update_today(P, d0 + 86400u, 0, SRS_Q_KNOWN) == 0, "day2 失败");
    /* 第 4 天（第 3 天断签）：1 复习答错 */
    CHECK(11, srs_stats_update_today(P, d0 + 3u * 86400u, 0, SRS_Q_FORGOT) == 0, "day4 失败");

    n = srs_stats_load_recent(P, days, 30);
    CHECK(11, n == 3, "load_recent n=%d 期望3", n);
    if (n == 3) {
        CHECK(11, days[0].streak_days == 1, "day1 streak=%u 期望1", days[0].streak_days);
        CHECK(11, days[0].new_learned == 1 && days[0].reviewed == 1 &&
              days[0].correct == 1 && days[0].wrong == 1,
              "day1 计数错误 new=%u rev=%u ok=%u bad=%u",
              days[0].new_learned, days[0].reviewed, days[0].correct, days[0].wrong);
        CHECK(11, days[1].streak_days == 2, "day2 streak=%u 期望2(连续)", days[1].streak_days);
        CHECK(11, days[2].streak_days == 1, "day4 streak=%u 期望1(断签重计)", days[2].streak_days);
        CHECK(11, days[0].date_yyyymmdd < days[1].date_yyyymmdd &&
              days[1].date_yyyymmdd < days[2].date_yyyymmdd,
              "日期未升序 %u %u %u",
              days[0].date_yyyymmdd, days[1].date_yyyymmdd, days[2].date_yyyymmdd);
    }

    remove(P);
    if (g_fail == before)
        printf("PASS case 11: stats streak (1->2 连续，断签后重计为 1)\n");
}

int main(void)
{
    case1();
    case2();
    case3();
    case4();
    case5();
    case6();
    case7();
    case8();
    case9();
    case10();
    case11();

    if (g_fail == 0) {
        printf("\n== 全部 11 个用例 PASS ==\n");
        return 0;
    }
    printf("\n== 共 %d 项断言失败 ==\n", g_fail);
    return 1;
}
