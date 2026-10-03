/**
 * review.cpp —— 复习流程编排
 *
 * 模式 A（FR-2）：正面（单词+音标+词性）→ 任意触摸翻面 → 背面（释义+例句高亮
 * + 三自评按钮）→ 自评 q=1/3/5 → srs_engine_rate → srs_store_append_journal
 * （满 20 触发 compact，FR-5）→ srs_queue_next 取下一词。
 *
 * 模式 B（FR-3）：释义 + N 个选项（1 正确 + wbx_pick_distractors 同桶干扰项，
 * 不足 3 个降级少选项，绝不跨桶）。选对 q=5；选错显示正确单词约 2 秒、q=1、
 * srs_queue_requeue_tail，flags.bit1 由 srs_engine 在 T3/T6 置位（收卡补救消费）。
 *
 * 防连跳（硬性交互约束）：翻面与自评必须是两次独立触摸——
 * 本模块只在 TOUCH_EV_PRESS 上动作，翻面后本次 RELEASE 在背面被丢弃，
 * 必须下一次 PRESS 才能自评；每次画面更新恰一次 disp_push_partial
 * （由 ui_render_commit 保证）。
 *
 * 收卡（队列空）：先扫描 flags.bit1 的词各补一轮模式 A（补救轮，随后清零），
 * 再合并 .srs、失效菜单概要缓存、回提示页。
 */
#include "review.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"

#include "rtc.h"
#include "touch.h"
#include "power.h"
#include "srs_store.h"
#include "ui/card.h"
#include "ui/render.h"
#include "wordbook.h"
#include "log.h"

#define TAG "review"

/* 模式 B 选错后停留展示正确答案的时长（FR-3，"约 2 秒"） */
#define QUIZ_WRONG_STAY_MS 2000

/* 词性枚举 → 中文（wbx_format.md §3 pos 枚举） */
static const char *pos_text(uint8_t pos)
{
    static const char *tbl[] = {
        "未知", "名词", "动词", "形容词", "副词",
        "介词", "连词", "代词", "数词", "缩写", "短语",
    };
    return (pos < sizeof(tbl) / sizeof(tbl[0])) ? tbl[pos] : tbl[0];
}

/* 当前词条索引缓存（模式 B 抽干扰项要用 distract[] 字段） */
static wbx_entry_t s_cur_entry;

/* 读 .stats 当日记录（已学新词数/已复习数）：每日额度按日累计的关键输入。
 * 文件缺失/无当日记录 → 全 0（视为新的一天） */
static void load_today_done(uint32_t now, uint32_t *new_done, uint32_t *rev_done)
{
    *new_done = 0;
    *rev_done = 0;
    srs_stat_day_t days[2];   /* 今天的记录若存在必在最近两条内 */
    int nd = srs_stats_load_recent(MOKA_STATS_PATH, days, 2);
    if (nd <= 0) return;
    time_t t = (time_t)now;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    uint32_t today = (uint32_t)((tmv.tm_year + 1900) * 10000
                     + (tmv.tm_mon + 1) * 100 + tmv.tm_mday);
    for (int i = 0; i < nd; i++) {
        if (days[i].date_yyyymmdd == today) {
            *new_done = days[i].new_learned;
            *rev_done = days[i].reviewed;
            return;
        }
    }
}

static srs_record_t *find_rec(uint32_t word_id)
{
    /* 全量数组下标即 word_id（见 wordbook.cpp），直查 O(1) */
    if (!g_app.recs || word_id >= g_app.rec_count) return NULL;
    return &g_app.recs[word_id];
}

static void free_card_texts(void)
{
    if (g_app.t_word)  { free(g_app.t_word);  g_app.t_word  = NULL; }
    if (g_app.t_phon)  { free(g_app.t_phon);  g_app.t_phon  = NULL; }
    if (g_app.t_gloss) { free(g_app.t_gloss); g_app.t_gloss = NULL; }
    if (g_app.t_ex)    { free(g_app.t_ex);    g_app.t_ex    = NULL; }
}

/* 按词读取索引与四段文本（缓冲按条目实际长度在 PSRAM 现配现用） */
static bool load_card(uint32_t word_id)
{
    free_card_texts();
    if (!g_app.book) return false;
    if (wbx_find_entry(g_app.book, word_id, &s_cur_entry) != 0) {
        MOKA_LOGE(TAG, "wbx_find_entry 失败 id=%u", (unsigned)word_id);
        return false;
    }
    const wbx_entry_t *e = &s_cur_entry;
    /* 防御：长度字段来自磁盘，异常时拒绝该词条（由上层跳过），
     * 不可截断分配——wbx_read_texts 按原始 len 写入，截断缓冲会溢出 */
    const uint32_t CAP = 4096;
    if (e->word_len == 0 || e->word_len > 64 || e->phon_len > 256 ||
        e->gloss_len == 0 || e->gloss_len > CAP || e->ex_len > CAP) {
        MOKA_LOGE(TAG, "词条长度字段异常 id=%u（word=%u phon=%u gloss=%u ex=%u），跳过",
                  (unsigned)word_id, (unsigned)e->word_len, (unsigned)e->phon_len,
                  (unsigned)e->gloss_len, (unsigned)e->ex_len);
        return false;
    }
    uint32_t wl = e->word_len, pl = e->phon_len, gl = e->gloss_len, el = e->ex_len;
    g_app.t_word  = (char *)moka_ps_malloc(wl + 1);
    g_app.t_phon  = (char *)moka_ps_malloc(pl + 1);
    g_app.t_gloss = (char *)moka_ps_malloc(gl + 1);
    g_app.t_ex    = (char *)moka_ps_malloc(el + 1);
    if (!g_app.t_word || !g_app.t_phon || !g_app.t_gloss || !g_app.t_ex) {
        MOKA_LOGE(TAG, "卡片文本缓冲分配失败 id=%u", (unsigned)word_id);
        free_card_texts();
        return false;
    }
    memset(g_app.t_word, 0, wl + 1);
    memset(g_app.t_phon, 0, pl + 1);
    memset(g_app.t_gloss, 0, gl + 1);
    memset(g_app.t_ex, 0, el + 1);
    /* 契约假设：wbx_read_texts 各字段写入 len 字节（含\0），缓冲已按 len+1 分配 */
    if (wbx_read_texts(g_app.book, e, g_app.t_word, g_app.t_phon,
                       g_app.t_gloss, g_app.t_ex) != 0) {
        MOKA_LOGE(TAG, "wbx_read_texts 失败 id=%u", (unsigned)word_id);
        free_card_texts();
        return false;
    }
    g_app.t_word[wl] = g_app.t_phon[pl] = g_app.t_gloss[gl] = g_app.t_ex[el] = '\0';
    g_app.cur_pos = e->pos;
    return true;
}

void review_cleanup(void)
{
    if (g_app.queue) { srs_queue_free(g_app.queue); g_app.queue = NULL; }
    if (g_app.id_list) { free(g_app.id_list); g_app.id_list = NULL; }
    g_app.id_count = g_app.id_idx = g_app.id_cap = 0;
    if (g_app.mb_words) { free(g_app.mb_words); g_app.mb_words = NULL; }
    free_card_texts();
    g_app.cur_valid = false;
    g_app.exit_confirm = false;
}

/* ===== 模式 B 选项准备：1 正确 + 同桶干扰项（FR-3，绝不跨桶） ===== */
static bool prepare_quiz(void)
{
    if (g_app.mb_words) { free(g_app.mb_words); g_app.mb_words = NULL; }
    g_app.mb_words = (char *)moka_ps_malloc(HIT_QUIZ_OPT_MAX * 64);
    if (!g_app.mb_words) {
        MOKA_LOGE(TAG, "选项缓冲分配失败");
        return false;
    }
    memset(g_app.mb_words, 0, HIT_QUIZ_OPT_MAX * 64);

    uint32_t dis[3] = { WBX_DISTRACT_EMPTY, WBX_DISTRACT_EMPTY, WBX_DISTRACT_EMPTY };
    int n = wbx_pick_distractors(g_app.book, &s_cur_entry, dis);
    if (n < 0) {
        MOKA_LOGE(TAG, "wbx_pick_distractors 失败 id=%u", (unsigned)s_cur_entry.word_id);
        n = 0;
    }
    g_app.mb_count = n + 1;   /* 干扰项不足 3 时降级为 2~3 选项（wbx.h 契约） */
    if (g_app.mb_count < 2) {
        MOKA_LOGE(TAG, "同桶干扰项为 0，无法出选择题 id=%u", (unsigned)s_cur_entry.word_id);
        return false;
    }
    g_app.mb_correct_pos = (int)(esp_random() % (uint32_t)g_app.mb_count);

    int di = 0;
    for (int i = 0; i < g_app.mb_count; i++) {
        char *dst = g_app.mb_words + i * 64;
        if (i == g_app.mb_correct_pos) {
            snprintf(dst, 64, "%s", g_app.t_word);
            continue;
        }
        /* 读干扰项单词文本（临时缓冲，读词后即释放） */
        wbx_entry_t de;
        if (di >= n || wbx_find_entry(g_app.book, dis[di], &de) != 0) {
            MOKA_LOGE(TAG, "干扰项读取失败 id=%u", (unsigned)(di < n ? dis[di] : 0));
            return false;
        }
        di++;
        /* 只展示干扰项单词，但 wbx_read_texts 四段都会写、长度以条目字段为准，
         * 必须为每段配足缓冲（否则非零长度段会溢出）；读词后即释放 */
        uint32_t wl = de.word_len;
        char *wtmp = (char *)moka_ps_malloc(wl + 1);
        char *ptmp = (char *)moka_ps_malloc((size_t)de.phon_len + 1);
        char *gtmp = (char *)moka_ps_malloc((size_t)de.gloss_len + 1);
        char *etmp = (char *)moka_ps_malloc((size_t)de.ex_len + 1);
        if (!wtmp || !ptmp || !gtmp || !etmp) {
            MOKA_LOGE(TAG, "干扰项缓冲分配失败");
            free(wtmp); free(ptmp); free(gtmp); free(etmp);
            return false;
        }
        int rc = wbx_read_texts(g_app.book, &de, wtmp, ptmp, gtmp, etmp);
        free(ptmp); free(gtmp); free(etmp);
        if (rc != 0) {
            MOKA_LOGE(TAG, "干扰项文本读取失败");
            free(wtmp);
            return false;
        }
        wtmp[wl] = '\0';
        snprintf(dst, 64, "%s", wtmp);
        free(wtmp);
    }
    return true;
}

/* ===== 顶部进度行文案（卡片页"今日待学 x/x · 今日待复习 x/x"） ===== */
static void make_header(char *buf, size_t n)
{
    if (g_app.review_kind == REVIEW_NORMAL) {
        /* 语义：待学=今日新词剩余/计划，待复习=今日到期剩余/总数。
         * 答错重插的词不重复扣减（进度按计划词数计，不按翻卡次数） */
        unsigned new_rem = (g_app.sess_new_total > g_app.sess_new_done)
                         ? (unsigned)(g_app.sess_new_total - g_app.sess_new_done) : 0;
        unsigned due_rem = (g_app.sess_due_total > g_app.sess_due_done)
                         ? (unsigned)(g_app.sess_due_total - g_app.sess_due_done) : 0;
        snprintf(buf, n, "今日待学 %u/%u · 今日待复习 %u/%u",
                 new_rem, (unsigned)g_app.sess_new_total,
                 due_rem, (unsigned)g_app.sess_due_total);
    } else {
        unsigned rem = (g_app.id_count > g_app.id_idx)
                     ? (unsigned)(g_app.id_count - g_app.id_idx) : 0;
        snprintf(buf, n, "%s · 剩余 %u",
                 g_app.review_kind == REVIEW_DIFFICULT ? "生词本复习" : "收卡补救", rem);
    }
}

/* ===== 渲染当前卡片（正面或选择题面），恰好一次 commit ===== */
static void render_front(void)
{
    char hdr[64];
    make_header(hdr, sizeof(hdr));
    if (g_app.mode_b) {
        ui_card_quiz_t q = { 0 };
        q.gloss = g_app.t_gloss;
        q.count = g_app.mb_count;
        q.highlight = -1;
        q.feedback = NULL;
        q.header = hdr;
        for (int i = 0; i < g_app.mb_count; i++) q.options[i] = g_app.mb_words + i * 64;
        ui_card_quiz(&q);
    } else {
        ui_card_front_t f = { 0 };
        f.word = g_app.t_word;
        /* 音标行：phon_len==0 或 IPA 字体缺失时传 NULL 隐藏（NFR-5 降级） */
        f.phon = (s_cur_entry.phon_len > 0 && ui_render_font_ipa()) ? g_app.t_phon : NULL;
        f.pos_text = pos_text(g_app.cur_pos);
        f.header = hdr;
        f.word_scale2 = g_app.word_scale2;
        ui_card_front(&f);
    }
}

/* ===== 评分与持久化（模式 A/B 共用出口） ===== */
static void rate_current(srs_quality_t q);

/* ===== 取下一词；队列空则收卡 ===== */
static bool next_card(void)
{
    uint32_t id = 0;
    for (;;) {
        bool has = false;
        if (g_app.review_kind == REVIEW_NORMAL) {
            has = g_app.queue && srs_queue_next(g_app.queue, &id) == 1;
        } else if (g_app.id_idx < g_app.id_count) {
            id = g_app.id_list[g_app.id_idx++];
            has = true;
        }
        if (!has) return false;   /* 由调用方走收卡路径 */

        if (load_card(id)) break;
        /* 词条缺失/损坏：记日志跳过，防止卡死整个会话 */
        MOKA_LOGE(TAG, "跳过损坏词条 id=%u", (unsigned)id);
    }

    g_app.cur_word  = id;
    g_app.cur_valid = true;

    /* 模式选择（docs/sm2 §3）：补救轮强制模式 A；
     * 新词（state==新词）一律模式 A 先学，其余跟随默认模式设置 */
    srs_record_t *rec = find_rec(id);
    g_app.mode_b = (g_app.default_mode == 1)
                && g_app.review_kind != REVIEW_REMEDIAL
                && !(rec && rec->state == 0);
    if (g_app.mode_b && !prepare_quiz()) {
        MOKA_LOGE(TAG, "模式 B 准备失败，降级模式 A id=%u", (unsigned)id);
        g_app.mode_b = false;
    }

    g_app.state = ST_REVIEW_FRONT;
    render_front();
    return true;
}

/* ===== 收卡：补救轮（flags.bit1）→ 合并 → 提示 ===== */
static void session_finish(void)
{
    const char *srs_path = (g_app.cur_book >= 0) ? g_app.books[g_app.cur_book].srs_path : NULL;

    if (g_app.review_kind == REVIEW_NORMAL) {
        /* sm2 §4：扫描 flags.bit1=1 的词，各追加一轮模式 A（补救轮） */
        size_t n = 0;
        for (size_t i = 0; i < g_app.rec_count; i++) {
            if (g_app.recs[i].flags & SRS_FLAG_FORCE_A) n++;
        }
        if (n > 0) {
            /* 容量 ×2+8：补救轮答错也要能重插尾部 */
            g_app.id_cap = n * 2 + 8;
            g_app.id_list = (uint32_t *)moka_ps_malloc(g_app.id_cap * sizeof(uint32_t));
            if (g_app.id_list) {
                size_t k = 0;
                for (size_t i = 0; i < g_app.rec_count; i++) {
                    if (g_app.recs[i].flags & SRS_FLAG_FORCE_A) g_app.id_list[k++] = g_app.recs[i].word_id;
                }
                g_app.id_count = n;   /* id_cap 保持 n*2+8：补救轮答错仍可重插尾部 */
                g_app.id_idx = 0;
                g_app.review_kind = REVIEW_REMEDIAL;
                MOKA_LOGI(TAG, "收卡补救轮：%u 个词补模式 A", (unsigned)n);
                if (next_card()) return;   /* 进入补救轮，页面已渲染 */
            } else {
                MOKA_LOGE(TAG, "补救轮列表分配失败（%u 词），跳过补救", (unsigned)n);
            }
        }
        /* sm2 §4：收卡合并 .srs */
        if (srs_path && srs_store_compact(srs_path, g_app.recs, g_app.rec_count) != 0) {
            MOKA_LOGE(TAG, "收卡合并失败: %s", srs_path);
        }
        app_set_message("今日学习完成", "点击返回菜单", false);
    } else if (g_app.review_kind == REVIEW_REMEDIAL) {
        if (srs_path && srs_store_compact(srs_path, g_app.recs, g_app.rec_count) != 0) {
            MOKA_LOGE(TAG, "补救轮合并失败: %s", srs_path);
        }
        app_set_message("补救复习完成", "点击返回菜单", false);
    } else {
        app_set_message("生词本轮完成", "点击返回菜单", false);
    }

    review_cleanup();
    if (g_app.cur_book >= 0) wordbook_invalidate_summary(g_app.cur_book);
    power_snapshot_clear();   /* 会话正常收卡，断点快照同步失效 */
    app_navigate(ST_MESSAGE);
}

static void rate_current(srs_quality_t q)
{
    if (!g_app.cur_valid) return;
    srs_record_t *rec = find_rec(g_app.cur_word);
    if (!rec) {
        MOKA_LOGE(TAG, "评分时记录缺失 id=%u", (unsigned)g_app.cur_word);
        if (!next_card()) session_finish();
        return;
    }

    uint32_t now = rtc_get_unix();
    int was_new = (rec->state == 0);    /* 统计用：新词首评计入 new_learned */
    rec->mode_hint = g_app.mode_b ? 1 : 0;
    srs_engine_rate(rec, q, now);

    /* 顶部进度行计数（复习页"今日待学/待复习"用） */
    if (was_new) {
        if (g_app.sess_new_done < 0xFFFF) g_app.sess_new_done++;
    } else {
        if (g_app.sess_due_done < 0xFFFF) g_app.sess_due_done++;
    }

    if (q < 3) {
        /* T3/T6：答错重插今日队列尾部 */
        if (g_app.review_kind == REVIEW_NORMAL && g_app.queue) {
            srs_queue_requeue_tail(g_app.queue, g_app.cur_word);
        } else if (g_app.id_list && g_app.id_count < g_app.id_cap) {
            g_app.id_list[g_app.id_count++] = g_app.cur_word;
        }
    }

    /* 补救轮：补完一轮即清零强制标记（sm2 §4"消费后清零"） */
    if (g_app.review_kind == REVIEW_REMEDIAL) {
        srs_engine_clear_force_mode_a(rec);
    }

    /* 持久化（FR-5）：追加日志，满 20 条触发合并 */
    const char *srs_path = g_app.books[g_app.cur_book].srs_path;
    int jl = srs_store_append_journal(srs_path, rec);
    if (jl < 0) {
        MOKA_LOGE(TAG, "日志追加失败: %s", srs_path);
    } else if (jl >= SRS_STORE_JOURNAL_LIMIT) {
        if (srs_store_compact(srs_path, g_app.recs, g_app.rec_count) != 0) {
            MOKA_LOGE(TAG, "日志满 20 合并失败: %s", srs_path);
        }
    }
    if (srs_stats_update_today(MOKA_STATS_PATH, now, was_new, q) != 0) {
        MOKA_LOGE(TAG, "当日统计更新失败: %s", MOKA_STATS_PATH);
    }

    if (!next_card()) session_finish();
}

/* ===== 模式 A：翻面 ===== */
static void flip_to_back(void)
{
    ui_card_back_t b = { 0 };
    b.gloss = g_app.t_gloss;
    b.example = (s_cur_entry.ex_len > 0) ? g_app.t_ex : NULL;
    b.btn_labels[0] = "忘记";
    b.btn_labels[1] = "模糊";
    b.btn_labels[2] = "认识";
    ui_card_back(&b);   /* 内部恰好一次 commit */
    g_app.state = ST_REVIEW_BACK;
    /* 防连跳：本次触摸的 RELEASE 将在 ST_REVIEW_BACK 中被丢弃（该态只认 PRESS） */
}

/* ===== 模式 B：选项判定 ===== */
static void quiz_on_touch(int16_t x, int16_t y)
{
    if (y < HIT_QUIZ_OPT_Y0) return;
    int rel = y - HIT_QUIZ_OPT_Y0;
    int idx = rel / HIT_QUIZ_OPT_STEP;
    if (idx >= g_app.mb_count) return;
    if (rel % HIT_QUIZ_OPT_STEP >= HIT_QUIZ_OPT_H) return;   /* 点中行距空隙，忽略 */
    if (x < HIT_QUIZ_OPT_X0 || x >= HIT_QUIZ_OPT_X1) return;

    bool correct = (idx == g_app.mb_correct_pos);

    /* 反馈渲染：选对反色选中块 + 文字；选错反色正确块 + 显示正确单词停留约 2 秒 */
    char hdr[64];
    make_header(hdr, sizeof(hdr));
    ui_card_quiz_t q = { 0 };
    q.gloss = g_app.t_gloss;
    q.count = g_app.mb_count;
    q.header = hdr;
    for (int i = 0; i < g_app.mb_count; i++) q.options[i] = g_app.mb_words + i * 64;
    char fb[96];
    if (correct) {
        q.highlight = idx;
        q.feedback = "回答正确";
    } else {
        q.highlight = g_app.mb_correct_pos;
        snprintf(fb, sizeof(fb), "正确单词：%s", g_app.t_word);
        q.feedback = fb;
    }
    ui_card_quiz(&q);   /* 恰好一次 commit */

    if (!correct) {
        vTaskDelay(pdMS_TO_TICKS(QUIZ_WRONG_STAY_MS));   /* 停留展示正确答案 */
        /* 排空停留期间积压在 GT911 缓冲里的触摸，防止误触直接翻下一张卡 */
        touch_event_t ev;
        while (touch_poll(&ev)) { /* 丢弃 */ }
        rate_current(SRS_Q_FORGOT);  /* q=1：EF 降级 + 重插队尾 + flags.bit1（引擎 T3/T6） */
    } else {
        rate_current(SRS_Q_KNOWN);   /* q=5 */
    }
}

void review_handle_touch(const app_msg_t *m)
{
    if (m->type != MSG_TOUCH || !g_app.cur_valid) return;

    /* 退出确认弹窗打开期间：只响应弹窗按钮，屏蔽下层卡片的一切触摸 */
    if (g_app.exit_confirm) {
        if (m->touch_kind != TOUCH_EV_PRESS) return;
        int x = m->x, y = m->y;
        if (y < HIT_POPUP_BTN_Y0 || y >= HIT_POPUP_BTN_Y0 + HIT_POPUP_BTN_H) return;
        if (x >= HIT_POPUP_CONTINUE_X0 && x < HIT_POPUP_CONTINUE_X1) {
            /* 继续学习：关弹窗并重绘当前卡片（正面/背面按原状态恢复） */
            g_app.exit_confirm = false;
            if (g_app.state == ST_REVIEW_BACK) flip_to_back();
            else render_front();
        } else if (x >= HIT_POPUP_EXIT_X0 && x < HIT_POPUP_EXIT_X1) {
            /* 退出：合并 .srs 落盘（防进度滞留日志区）→ 清理 → 回主页面 */
            MOKA_LOGI(TAG, "用户退出复习");
            g_app.exit_confirm = false;
            if (g_app.cur_book >= 0 && g_app.recs) {
                const char *p = g_app.books[g_app.cur_book].srs_path;
                if (srs_store_compact(p, g_app.recs, g_app.rec_count) != 0) {
                    MOKA_LOGE(TAG, "退出时合并失败: %s", p);
                }
            }
            review_cleanup();
            if (g_app.cur_book >= 0) wordbook_invalidate_summary(g_app.cur_book);
            power_snapshot_clear();   /* 会话已结束，防止 EN 复位后错误复活 */
            app_navigate(ST_MENU);
        }
        return;
    }

    if (g_app.state == ST_REVIEW_FRONT) {
        /* 只认按下：RELEASE/DOUBLE 一律忽略（防连跳第一道） */
        if (m->touch_kind != TOUCH_EV_PRESS) return;
        /* 左上角退出按钮优先于"任意处翻面"判定 */
        if (m->x >= HIT_CARD_EXIT_X0 && m->x < HIT_CARD_EXIT_X1
            && m->y >= HIT_CARD_EXIT_Y0 && m->y < HIT_CARD_EXIT_Y1) {
            g_app.exit_confirm = true;
            ui_card_exit_confirm((uint32_t)g_app.sess_new_done + g_app.sess_due_done);
            return;
        }
        if (g_app.mode_b) quiz_on_touch(m->x, m->y);
        else              flip_to_back();
    } else if (g_app.state == ST_REVIEW_BACK) {
        /* 防连跳：翻面残留的 RELEASE 在此被丢弃，必须下一次 PRESS 才能自评 */
        if (m->touch_kind != TOUCH_EV_PRESS) return;
        if (m->y < HIT_CARD_BTN_Y0) return;
        int idx = m->x / HIT_CARD_BTN_W;
        if (idx < 0) idx = 0;
        if (idx >= HIT_CARD_BTN_COUNT) idx = HIT_CARD_BTN_COUNT - 1;
        /* 按钮序与 UI 一致：[忘记][模糊][认识] → q=1/3/5（sm2 §1 锁定） */
        static const srs_quality_t qmap[HIT_CARD_BTN_COUNT] = {
            SRS_Q_FORGOT, SRS_Q_BLURRY, SRS_Q_KNOWN,
        };
        rate_current(qmap[idx]);
    }
}

bool review_start(review_kind_t kind)
{
    if (!g_app.book || !g_app.recs || g_app.cur_book < 0) {
        MOKA_LOGE(TAG, "未打开词书，无法开始复习");
        app_set_message("请先选择词书", "点击返回菜单", false);
        app_navigate(ST_MESSAGE);
        return false;
    }
    review_cleanup();
    g_app.review_kind = kind;
    uint32_t now = rtc_get_unix();

    /* 今日已完成量（.stats 当日记录）：每日新词上限按"日"累计（墨墨语义），
     * 今天已学的新词不重复给额度——否则每次重进都会再发 20 个新词 */
    uint32_t new_done_today = 0, rev_done_today = 0;
    load_today_done(now, &new_done_today, &rev_done_today);
    uint32_t new_quota = (kind == REVIEW_NORMAL
                          && g_app.daily_new > new_done_today)
                       ? g_app.daily_new - new_done_today : 0;

    /* 顶部进度行基数：今日计划总量（含已完成部分），done 预填今日已完成数。
     * done 计数在 rate_current 里随自评继续递增 */
    g_app.sess_new_done = g_app.sess_due_done = 0;
    if (kind == REVIEW_NORMAL) {
        uint32_t due = 0, fresh = 0;
        for (size_t i = 0; i < g_app.rec_count; i++) {
            if (g_app.recs[i].state != 0) {
                if (g_app.recs[i].due_unix <= now) due++;
            } else {
                fresh++;
            }
        }
        uint32_t admit = (fresh < new_quota) ? fresh : new_quota;   /* 今天还要发的新词 */
        uint32_t plan  = new_done_today + admit;                    /* 今日新词总计划 */
        uint32_t duet  = due + rev_done_today;                      /* 今日到期总量 */
        g_app.sess_new_total = (uint16_t)((plan > 0xFFFF) ? 0xFFFF : plan);
        g_app.sess_due_total = (uint16_t)((duet > 0xFFFF) ? 0xFFFF : duet);
        g_app.sess_new_done  = (uint16_t)((new_done_today > plan) ? plan : new_done_today);
        g_app.sess_due_done  = (uint16_t)((rev_done_today > duet) ? duet : rev_done_today);
    } else {
        g_app.sess_new_total = g_app.sess_due_total = 0;
    }

    if (kind == REVIEW_NORMAL) {
        g_app.queue = srs_queue_build(g_app.recs, g_app.rec_count, now, new_quota);
        if (!g_app.queue) {
            MOKA_LOGE(TAG, "今日队列构建失败");
            app_set_message("队列构建失败", "点击返回菜单", false);
            app_navigate(ST_MESSAGE);
            return false;
        }
        if (srs_queue_remaining(g_app.queue) == 0) {
            app_set_message("今日没有待复习任务", "点击返回菜单", false);
            app_navigate(ST_MESSAGE);
            return false;
        }
    } else if (kind == REVIEW_DIFFICULT) {
        /* 生词本：lapses>=2（srs_engine_is_difficult），单独成册复习（FR-6） */
        size_t n = 0;
        for (size_t i = 0; i < g_app.rec_count; i++) {
            if (srs_engine_is_difficult(&g_app.recs[i])) n++;
        }
        if (n == 0) {
            app_set_message("生词本为空", "点击返回菜单", false);
            app_navigate(ST_MESSAGE);
            return false;
        }
        /* 容量 ×2+8：答错重插尾部需要余量 */
        g_app.id_cap = n * 2 + 8;
        g_app.id_list = (uint32_t *)moka_ps_malloc(g_app.id_cap * sizeof(uint32_t));
        if (!g_app.id_list) {
            MOKA_LOGE(TAG, "生词本队列分配失败（%u 词）", (unsigned)n);
            app_set_message("内存不足", "点击返回菜单", false);
            app_navigate(ST_MESSAGE);
            return false;
        }
        for (size_t i = 0; i < g_app.rec_count; i++) {
            if (srs_engine_is_difficult(&g_app.recs[i])) {
                g_app.id_list[g_app.id_count++] = g_app.recs[i].word_id;
            }
        }
        g_app.id_idx = 0;
    }
    /* REVIEW_REMEDIAL 不从此入口启动（由 session_finish 内部转入） */

    g_app.session_start_unix = now;
    MOKA_LOGI(TAG, "复习开始 kind=%d", (int)kind);
    if (!next_card()) {
        session_finish();
        return false;
    }
    return true;
}

bool review_resume_card(uint32_t word_id)
{
    /* 唤醒恢复（FR-7）：队列按今日规则重建，断点词直接置顶为当前卡片。
     * 已知取舍：断点词若仍在队列中会被再遇到一次（视为巩固），注释备查。 */
    if (!g_app.book || !g_app.recs) return false;
    review_cleanup();
    g_app.review_kind = REVIEW_NORMAL;
    uint32_t now2 = rtc_get_unix();
    /* 每日新词上限按日累计（与 review_start 同规则，见该处注释） */
    uint32_t new_done_today = 0, rev_done_today = 0;
    load_today_done(now2, &new_done_today, &rev_done_today);
    uint32_t new_quota = (g_app.daily_new > new_done_today)
                       ? g_app.daily_new - new_done_today : 0;
    g_app.queue = srs_queue_build(g_app.recs, g_app.rec_count, now2, new_quota);
    if (!g_app.queue) {
        MOKA_LOGE(TAG, "唤醒恢复时队列构建失败（继续仅恢复断点卡）");
    }
    /* 顶部进度行基数（与 review_start 同规则） */
    {
        uint32_t due = 0, fresh = 0;
        for (size_t i = 0; i < g_app.rec_count; i++) {
            if (g_app.recs[i].state != 0) {
                if (g_app.recs[i].due_unix <= now2) due++;
            } else {
                fresh++;
            }
        }
        uint32_t admit = (fresh < new_quota) ? fresh : new_quota;
        uint32_t plan  = new_done_today + admit;
        uint32_t duet  = due + rev_done_today;
        g_app.sess_due_total = (uint16_t)((duet > 0xFFFF) ? 0xFFFF : duet);
        g_app.sess_new_total = (uint16_t)((plan > 0xFFFF) ? 0xFFFF : plan);
        g_app.sess_new_done  = (uint16_t)((new_done_today > plan) ? plan : new_done_today);
        g_app.sess_due_done  = (uint16_t)((rev_done_today > duet) ? duet : rev_done_today);
    }
    if (!load_card(word_id)) return false;
    g_app.cur_word  = word_id;
    g_app.cur_valid = true;
    g_app.mode_b    = false;   /* 恢复到模式 A 正面，状态最简单 */
    g_app.state = ST_REVIEW_FRONT;
    render_front();
    MOKA_LOGI(TAG, "已恢复断点卡片 id=%u", (unsigned)word_id);
    return true;
}
