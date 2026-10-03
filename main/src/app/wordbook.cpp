/**
 * wordbook.cpp —— 多词书管理实现
 *
 * 关键决策：
 * - g_app.recs 是全量数组（长度=词书 word_count，下标=word_id）。
 *   .srs 只存"评过分的词"，新词没有记录；为了把新词喂给 srs_queue_build，
 *   打开词书时先生成全默认记录（ef=2.5、state=新词，初值见 docs/sm2_state_machine.md §1），
 *   再用 srs_store_load 的结果按 word_id 覆盖。合并/写盘也用这个全量数组。
 * - 菜单概要的 已学/待复习 需读每本书的 .srs，惰性计算并缓存，
 *   复习结束后 wordbook_invalidate_summary 失效重算。
 */
#include "wordbook.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "rtc.h"
#include "srs_store.h"
#include "log.h"

#define TAG "wordbook"

/* 新词默认记录初值：ef=2.5(x100=250)、state=新词（docs/sm2_state_machine.md §1） */
static void record_init_default(srs_record_t *r, uint32_t word_id)
{
    memset(r, 0, sizeof(*r));
    r->word_id = word_id;
    r->ef_x100 = 250;
    r->state   = 0; /* 新词 */
}

/* 后缀匹配大小写不敏感：IDF FATFS 未开 LFN 时 readdir 返回 8.3 大写短名
 * （真机实测 CET4.wbx 被读成 "CET4.WBX"），纯小写比较会误判无词书 */
static bool has_wbx_suffix(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && name[n - 4] == '.'
        && tolower((unsigned char)name[n - 3]) == 'w'
        && tolower((unsigned char)name[n - 2]) == 'b'
        && tolower((unsigned char)name[n - 1]) == 'x';
}

/* 按词书名排序（简单插入排序，词书数量小） */
static void sort_books(void)
{
    for (size_t i = 1; i < g_app.book_count; i++) {
        book_info_t tmp = g_app.books[i];
        size_t j = i;
        while (j > 0 && strcmp(g_app.books[j - 1].name, tmp.name) > 0) {
            g_app.books[j] = g_app.books[j - 1];
            j--;
        }
        g_app.books[j] = tmp;
    }
}

int wordbook_scan(void)
{
    wordbook_free();

    DIR *d = opendir(MOKA_WORDBOOK_DIR);
    if (!d) {
        MOKA_LOGE(TAG, "打开词书目录失败: %s", MOKA_WORDBOOK_DIR);
        return -1;
    }

    /* 第一遍：数 .wbx 文件个数（IDF FATFS 的 readdir 可能报 DT_UNKNOWN，一并接受）。
     * 逐个打印目录项：真机排障期需要直接看到卡里到底有什么（文件名错/放错目录
     * 是"词书未识别"的最常见原因），词书目录项数少，日志开销可忽略。 */
    size_t count = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        MOKA_LOGI(TAG, "目录项: \"%s\" type=%d", de->d_name, (int)de->d_type);
        if (de->d_type != DT_DIR && has_wbx_suffix(de->d_name)) count++;
    }
    if (count == 0) {
        closedir(d);
        MOKA_LOGI(TAG, "未发现 .wbx 词书（目录存在但无匹配文件）");
        return 0;
    }

    g_app.books = (book_info_t *)moka_ps_malloc(count * sizeof(book_info_t));
    if (!g_app.books) {
        MOKA_LOGE(TAG, "词书列表分配失败（%u 本）", (unsigned)count);
        closedir(d);
        return -1;
    }
    memset(g_app.books, 0, count * sizeof(book_info_t));

    /* 第二遍：填充路径并读词书名/总词数（短暂打开即关，避免常驻句柄） */
    rewinddir(d);
    size_t n = 0;
    while ((de = readdir(d)) != NULL && n < count) {
        if (!has_wbx_suffix(de->d_name) || de->d_type == DT_DIR) continue;

        book_info_t *bi = &g_app.books[n];
        int w = snprintf(bi->path, sizeof(bi->path), "%s/%s", MOKA_WORDBOOK_DIR, de->d_name);
        if (w <= 0 || (size_t)w >= sizeof(bi->path)) {
            MOKA_LOGE(TAG, "路径过长，跳过: %s", de->d_name);
            continue;
        }
        snprintf(bi->srs_path, sizeof(bi->srs_path), "%s.srs", bi->path);

        wbx_book_t *b = wbx_open(bi->path);
        if (!b) {
            MOKA_LOGE(TAG, "词书校验失败，跳过: %s", bi->path);
            continue;
        }
        bi->total = wbx_word_count(b);
        const char *nm = wbx_book_name(b);
        if (nm) snprintf(bi->name, sizeof(bi->name), "%s", nm);
        else    snprintf(bi->name, sizeof(bi->name), "%.*s", (int)sizeof(bi->name) - 1, de->d_name);
        wbx_close(b);
        n++;
    }
    closedir(d);

    g_app.book_count = n;
    sort_books();
    MOKA_LOGI(TAG, "扫描到 %u 本词书", (unsigned)n);
    return (int)n;
}

void wordbook_free(void)
{
    if (g_app.book) { wbx_close(g_app.book); g_app.book = NULL; }
    if (g_app.recs) { free(g_app.recs); g_app.recs = NULL; }
    g_app.rec_count = 0;
    g_app.cur_book  = -1;
    if (g_app.books) { free(g_app.books); g_app.books = NULL; }
    g_app.book_count = 0;
}

int wordbook_open_current(int idx)
{
    if (idx < 0 || (size_t)idx >= g_app.book_count) {
        MOKA_LOGE(TAG, "词书下标越界: %d", idx);
        return -1;
    }
    book_info_t *bi = &g_app.books[idx];

    /* 关闭旧书（不释放 books 列表本身） */
    if (g_app.book) { wbx_close(g_app.book); g_app.book = NULL; }
    if (g_app.recs) { free(g_app.recs); g_app.recs = NULL; g_app.rec_count = 0; }

    wbx_book_t *b = wbx_open(bi->path);
    if (!b) {
        MOKA_LOGE(TAG, "wbx_open 失败: %s", bi->path);
        return -1;
    }
    uint32_t wc = wbx_word_count(b);
    if (wc == 0 || wc > 200000u /* 防御性上限，避免异常头耗尽内存 */) {
        MOKA_LOGE(TAG, "词书条数异常: %u", (unsigned)wc);
        wbx_close(b);
        return -1;
    }

    /* 断电恢复：若残留 .srs.tmp（上次合并中途断电），先转正/丢弃再加载
     * （docs/srs_persistence.md §3 步骤 4） */
    if (srs_store_recover_tmp(bi->srs_path) != 0) {
        MOKA_LOGE(TAG, "srs_store_recover_tmp 失败: %s（继续尝试加载）", bi->srs_path);
    }

    /* 全量记录数组：先填新词默认值，再覆盖已学记录 */
    srs_record_t *recs = (srs_record_t *)moka_ps_malloc((size_t)wc * sizeof(srs_record_t));
    if (!recs) {
        MOKA_LOGE(TAG, "记录数组分配失败（%u 条）", (unsigned)wc);
        wbx_close(b);
        return -1;
    }
    for (uint32_t i = 0; i < wc; i++) record_init_default(&recs[i], i);

    srs_record_t *loaded = NULL;
    size_t loaded_n = 0;
    int rc = srs_store_load(bi->srs_path, &loaded, &loaded_n);
    /* 契约假设：srs_store_load 返回 0 成功；文件不存在视为全空（loaded_n=0） */
    if (rc != 0) {
        MOKA_LOGE(TAG, "srs_store_load 失败 rc=%d: %s（按全新进度继续）", rc, bi->srs_path);
    } else {
        for (size_t i = 0; i < loaded_n; i++) {
            uint32_t id = loaded[i].word_id;
            if (id < wc) recs[id] = loaded[i];   /* 下标即 word_id，直查 O(1) */
            else MOKA_LOGE(TAG, ".srs 记录 word_id=%u 超出词书范围，丢弃", (unsigned)id);
        }
    }
    if (loaded) free(loaded);
    /* TODO(srs 层缺口): srs_store_load 内部 malloc 的大数组落在内部 RAM，
     * 词书很大时可能失败；需要 srs 层提供 allocator 钩子才能强制 PSRAM。 */

    g_app.book      = b;
    g_app.recs      = recs;
    g_app.rec_count = wc;
    g_app.cur_book  = idx;
    MOKA_LOGI(TAG, "已打开词书 %s（%u 词）", bi->name, (unsigned)wc);
    return 0;
}

int wordbook_compute_summary(int idx)
{
    if (idx < 0 || (size_t)idx >= g_app.book_count) return -1;
    book_info_t *bi = &g_app.books[idx];
    if (bi->summary_valid) return 0;

    srs_record_t *recs = NULL;
    size_t n = 0;
    int rc = srs_store_load(bi->srs_path, &recs, &n);
    if (rc != 0) {
        MOKA_LOGE(TAG, "概要加载失败 rc=%d: %s", rc, bi->srs_path);
        bi->learned = 0;
        bi->due     = 0;
        bi->summary_valid = true;   /* 无进度文件也是合法结果 */
        return 0;
    }

    uint32_t now = rtc_get_unix();
    uint32_t learned = 0, due = 0;
    for (size_t i = 0; i < n; i++) {
        if (recs[i].state != 0 /* 新词 */) {
            learned++;
            if (recs[i].due_unix <= now) due++;
        }
    }
    free(recs);
    bi->learned = learned;
    bi->due     = due;
    bi->summary_valid = true;
    return 0;
}

void wordbook_invalidate_summary(int idx)
{
    if (idx >= 0 && (size_t)idx < g_app.book_count) g_app.books[idx].summary_valid = false;
}
