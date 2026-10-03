/**
 * wbx.c —— .wbx 词书读取（docs/wbx_format.md v1.0）
 *
 * 设计：固定 48B 索引定点 fread，文本区按需读取，不做全量载入；
 * 仅打开时为校验 CRC32 读取一次全文件（词书为只读资源，校验发生在开机加载时）。
 *
 * 干扰项（FR-3）：只从同桶 distract[5] 非空槽取；即使生成端出错混入跨桶 id，
 * 固件端逐条回读校验 freq_bucket，不匹配的槽直接跳过——跨桶次数验收要求为 0，
 * 不能只信生成端自觉。
 */
#include "wbx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SRS_LOG
#define SRS_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

struct wbx_book {
    FILE        *f;
    wbx_header_t h;
};

/* CRC32 查表版（与 srs_store.c 同一实现，static 各自编译单元内独立） */
static uint32_t s_crc_tab[256];
static int      s_crc_ready = 0;

static void crc32_init(void)
{
    uint32_t i;
    int k;
    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        s_crc_tab[i] = c;
    }
    s_crc_ready = 1;
}

static uint32_t crc32_calc(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    if (!s_crc_ready) crc32_init();
    for (i = 0; i < len; i++)
        c = s_crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

wbx_book_t *wbx_open(const char *path)
{
    wbx_book_t *b;
    FILE *f;
    long fsize;
    uint8_t *img;

    if (!path) return NULL;
    f = fopen(path, "rb");
    if (!f) {
        SRS_LOG("[wbx] open: cannot open %s\n", path);
        return NULL;
    }

    b = (wbx_book_t *)malloc(sizeof(*b));    /* 约束仅允许 malloc/free */
    if (!b) { fclose(f); return NULL; }
    memset(b, 0, sizeof(*b));

    if (fread(&b->h, sizeof(b->h), 1, f) != 1) goto fail;
    if (memcmp(b->h.magic, "WBX1", 4) != 0 || b->h.version != 1) {
        SRS_LOG("[wbx] open: bad magic/version\n");
        goto fail;
    }

    /* 全文件 CRC32 校验（crc32 字段置 0 计算，见 docs §2） */
    if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) goto fail;
    if (fsize < (long)sizeof(wbx_header_t) ||
        (uint32_t)fsize < b->h.text_ofs ||
        (uint32_t)fsize < b->h.meta_ofs + b->h.word_count * (uint32_t)sizeof(wbx_entry_t)) {
        SRS_LOG("[wbx] open: truncated file\n");
        goto fail;
    }
    img = (uint8_t *)malloc((size_t)fsize);
    if (!img) goto fail;
    if (fread(img, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(img);
        goto fail;
    }
    memset(img + 0x38, 0, 4);   /* crc32 字段置 0 */
    if (crc32_calc(img, (size_t)fsize) != b->h.crc32) {
        SRS_LOG("[wbx] open: CRC mismatch\n");
        free(img);
        goto fail;
    }
    free(img);

    b->f = f;
    return b;

fail:
    fclose(f);
    free(b);
    return NULL;
}

void wbx_close(wbx_book_t *b)
{
    if (!b) return;
    if (b->f) fclose(b->f);
    free(b);
}

uint32_t wbx_word_count(const wbx_book_t *b)
{
    return b ? b->h.word_count : 0;
}

const char *wbx_book_name(const wbx_book_t *b)
{
    if (!b) return "";
    /* name_len 含 \0 且 ≤32，由生成端保证；这里兜底强制结尾 */
    ((wbx_book_t *)b)->h.name[31] = '\0';
    return b->h.name;
}

int wbx_read_entry(wbx_book_t *b, uint32_t idx, wbx_entry_t *out)
{
    long ofs;
    if (!b || !out || idx >= b->h.word_count) return -1;
    ofs = (long)b->h.meta_ofs + (long)idx * (long)sizeof(wbx_entry_t);
    if (fseek(b->f, ofs, SEEK_SET) != 0) return -1;
    if (fread(out, sizeof(*out), 1, b->f) != 1) return -1;
    return 0;
}

int wbx_read_texts(wbx_book_t *b, const wbx_entry_t *e,
                   char *word, char *phon, char *gloss, char *example)
{
    long ofs;
    if (!b || !e) return -1;
    ofs = (long)b->h.text_ofs + (long)e->text_ofs;
    if (fseek(b->f, ofs, SEEK_SET) != 0) return -1;

    /* 四段连续存放：word\0 phon\0 gloss\0 ex\0；长度可为 0（无音标/例句） */
    if (e->word_len > 0) {
        if (!word || fread(word, 1, e->word_len, b->f) != e->word_len) return -1;
    } else if (word) word[0] = '\0';

    if (e->phon_len > 0) {
        if (!phon || fread(phon, 1, e->phon_len, b->f) != e->phon_len) return -1;
    } else if (phon) phon[0] = '\0';

    if (e->gloss_len > 0) {
        if (!gloss || fread(gloss, 1, e->gloss_len, b->f) != e->gloss_len) return -1;
    } else if (gloss) gloss[0] = '\0';

    if (e->ex_len > 0) {
        if (!example || fread(example, 1, e->ex_len, b->f) != e->ex_len) return -1;
    } else if (example) example[0] = '\0';

    return 0;
}

int wbx_find_entry(wbx_book_t *b, uint32_t word_id, wbx_entry_t *out)
{
    uint32_t i;
    if (!b || !out) return -1;

    /* 规范约定 word_id == 索引序号（0..N-1），先直接定点命中 */
    if (word_id < b->h.word_count) {
        if (wbx_read_entry(b, word_id, out) == 0 && out->word_id == word_id)
            return 0;
    }
    /* 兜底线性扫描（生成端未按约定排序时仍能工作） */
    for (i = 0; i < b->h.word_count; i++) {
        if (wbx_read_entry(b, i, out) == 0 && out->word_id == word_id)
            return 0;
    }
    return -1;
}

int wbx_pick_distractors(wbx_book_t *b, const wbx_entry_t *e, uint32_t out_ids[3])
{
    static uint32_t s_lcg = 0x2F6E2B1u;  /* 进程级 LCG，避免连续调用总取同三个 */
    uint32_t cand[5];
    int ncand = 0, picked = 0, i;

    if (!b || !e || !out_ids) return 0;

    /* 收集合法候选：非空槽 + 回读校验同桶（固件端不信任生成端，FR-3 验收跨桶=0） */
    for (i = 0; i < 5; i++) {
        wbx_entry_t ce;
        uint32_t cid = e->distract[i];
        if (cid == WBX_DISTRACT_EMPTY) continue;
        if (cid == e->word_id) continue;
        if (wbx_find_entry(b, cid, &ce) != 0) continue;
        if (ce.freq_bucket != e->freq_bucket) {
            SRS_LOG("[wbx] distractor %u cross-bucket, skip\n", (unsigned)cid);
            continue;
        }
        cand[ncand++] = cid;
    }

    /* LCG 无放回抽 3 个；不足时返回实际数量（上层降级为 2~3 选项） */
    while (picked < 3 && ncand > 0) {
        int idx;
        s_lcg = s_lcg * 1664525u + 1013904223u + e->word_id;
        idx = (int)((s_lcg >> 16) % (uint32_t)ncand);
        out_ids[picked++] = cand[idx];
        cand[idx] = cand[--ncand];   /* 交换删除 */
    }
    return picked;
}
