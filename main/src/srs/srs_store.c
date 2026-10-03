/**
 * srs_store.c —— .srs / .stats 持久化（docs/srs_persistence.md 严格布局）
 *
 * 断电安全要点（为什么这样做）：
 * - 自评只追加 64B 日志到文件末尾：FAT/SD 原位写是最危险的断电窗口，
 *   追加写最多损失最新一条，开机回放时长度校验可丢弃尾垃圾。
 * - 合并走 tmp + fsync + .bak 三段轮换：FatFs 的 rename 不覆盖已存在目标
 *  （真机实测，POSIX 覆盖语义不成立），故 path→.bak、tmp→path、删 .bak；
 *   断电窗口内 .bak 兜底，recover 时转正。ESP-IDF 下无法 fsync 目录，
 *   用 reopen + fsync 文件本身兜底（docs §3e）。
 * - 开机先调 srs_store_recover_tmp：发现 .tmp 说明上次合并中途断电，
 *   CRC 合法转正、非法丢弃；.bak 残留同理（仅主文件缺失时转正）。
 *
 * 签名不含 now（不读时钟约束），因此 compact 时 last_compact_unix 保留旧头值；
 * book_word_count 同理保留。调用方若需刷新这两个字段，应在 compact 前先写头。
 */
#include "srs_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define _POSIX_C_SOURCE 200809L   /* fsync/fileno（任务书显式允许 fsync） */
#include <unistd.h>

#ifndef SRS_LOG
#define SRS_LOG(...) fprintf(stderr, __VA_ARGS__)
#endif

/* ---------------- 磁盘布局（与 docs/srs_persistence.md §2/§4 逐字节对应） ---------------- */
#pragma pack(push, 1)
typedef struct {
    char     magic[4];          /* "SRS1" */
    uint32_t version;           /* = 1 */
    uint32_t rec_count;
    uint32_t journal_len;       /* 0..19，到 20 触发合并 */
    uint32_t book_word_count;
    uint32_t last_compact_unix;
    uint32_t crc32;             /* 主记录区 CRC32（日志区不参与） */
    uint32_t reserved;
} srs_hdr_t;                    /* 32 B */

typedef struct {
    uint32_t word_id;           /* 0x00 */
    uint16_t ef_x100;           /* 0x04 */
    uint16_t reps;              /* 0x06 */
    uint32_t interval_days;     /* 0x08 */
    uint32_t due_unix;          /* 0x0C */
    uint32_t last_review_unix;  /* 0x10 */
    uint16_t lapses;            /* 0x14 */
    uint8_t  state;             /* 0x16 */
    uint8_t  last_quality;      /* 0x17 */
    uint8_t  flags;             /* 0x18 */
    uint8_t  mode_hint;         /* 0x19 */
    uint8_t  reserved[6];       /* 0x1A */
    uint8_t  reserved2[32];     /* 0x20 */
} srs_disk_rec_t;               /* 64 B */

typedef struct {
    char     magic[4];          /* "STA1" */
    uint32_t version;
    uint32_t rec_count;
    uint32_t crc32;             /* 全文件 CRC32（本字段置 0 计算） */
} sta_hdr_t;                    /* 16 B */

typedef struct {
    uint32_t date_yyyymmdd;
    uint16_t new_learned;
    uint16_t reviewed;
    uint16_t correct;
    uint16_t wrong;
    uint16_t streak_days;
    uint16_t study_secs;
    uint8_t  reserved[16];
} sta_rec_t;                    /* 32 B */
#pragma pack(pop)

typedef char srs_hdr_size_chk[(sizeof(srs_hdr_t) == 32) ? 1 : -1];
typedef char srs_rec_size_chk[(sizeof(srs_disk_rec_t) == 64) ? 1 : -1];
typedef char sta_hdr_size_chk[(sizeof(sta_hdr_t) == 16) ? 1 : -1];
typedef char sta_rec_size_chk[(sizeof(sta_rec_t) == 32) ? 1 : -1];

/* ---------------- CRC32（zlib 多项式 0xEDB88320 查表版，与 PC/Python zlib.crc32 一致） ---------------- */
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

/* ---------------- 内存记录 <-> 磁盘记录 ---------------- */
static void rec_to_disk(const srs_record_t *m, srs_disk_rec_t *d)
{
    memset(d, 0, sizeof(*d));    /* reserved 区必须写 0 */
    d->word_id          = m->word_id;
    d->ef_x100          = m->ef_x100;
    d->reps             = m->reps;
    d->interval_days    = m->interval_days;
    d->due_unix         = m->due_unix;
    d->last_review_unix = m->last_review_unix;
    d->lapses           = m->lapses;
    d->state            = m->state;
    d->last_quality     = m->last_quality;
    d->flags            = m->flags;
    d->mode_hint        = m->mode_hint;
}

static void rec_from_disk(const srs_disk_rec_t *d, srs_record_t *m)
{
    m->word_id          = d->word_id;
    m->ef_x100          = d->ef_x100;
    m->reps             = d->reps;
    m->interval_days    = d->interval_days;
    m->due_unix         = d->due_unix;
    m->last_review_unix = d->last_review_unix;
    m->lapses           = d->lapses;
    m->state            = d->state;
    m->last_quality     = d->last_quality;
    m->flags            = d->flags;
    m->mode_hint        = d->mode_hint;
}

/* ---------------- 内部小工具 ---------------- */

/* 同步落盘：fflush + fsync，任一步失败返回 -1 */
static int flush_sync(FILE *f)
{
    if (fflush(f) != 0) {
        SRS_LOG("[srs] fflush failed\n");
        return -1;
    }
    if (fsync(fileno(f)) != 0) {
        SRS_LOG("[srs] fsync failed\n");
        return -1;
    }
    return 0;
}

/* 读并校验 .srs 头；f 需已打开。成功返回 0 且 *h 有效 */
static int read_hdr(FILE *f, srs_hdr_t *h)
{
    if (fseek(f, 0, SEEK_SET) != 0) return -1;
    if (fread(h, sizeof(*h), 1, f) != 1) return -1;
    if (memcmp(h->magic, "SRS1", 4) != 0 || h->version != 1) return -1;
    return 0;
}

/* 校验 tmp/正式文件镜像的头部 + 主记录区 CRC + 文件长度。f 定位到文件头。 */
static int validate_image(FILE *f, srs_hdr_t *h)
{
    srs_disk_rec_t *buf = NULL;
    long fsize;
    int ok = -1;

    if (read_hdr(f, h) != 0) return -1;
    if (fseek(f, 0, SEEK_END) != 0) return -1;
    fsize = ftell(f);
    if (fsize < 0) return -1;
    /* 长度必须恰好 = 头 + 主记录 + 日志（允许日志，容忍手工镜像） */
    if ((uint32_t)(fsize - (long)sizeof(srs_hdr_t)) <
        (h->rec_count + h->journal_len) * (uint32_t)sizeof(srs_disk_rec_t))
        return -1;

    if (h->rec_count > 0) {
        buf = (srs_disk_rec_t *)malloc(h->rec_count * sizeof(srs_disk_rec_t));
        if (!buf) return -1;
        if (fseek(f, (long)sizeof(srs_hdr_t), SEEK_SET) != 0) goto out;
        if (fread(buf, sizeof(srs_disk_rec_t), h->rec_count, f) != h->rec_count)
            goto out;
        if (crc32_calc(buf, h->rec_count * sizeof(srs_disk_rec_t)) != h->crc32)
            goto out;
    } else if (h->crc32 != 0) {
        goto out;   /* 空主记录区 CRC 应为 crc32("") = 0 */
    }
    ok = 0;
out:
    free(buf);
    return ok;
}

static int cmp_word_id(const void *a, const void *b)
{
    const srs_record_t *x = a, *y = b;
    if (x->word_id == y->word_id) return 0;
    return (x->word_id < y->word_id) ? -1 : 1;
}

/* ---------------- .srs 对外接口 ---------------- */

int srs_store_load(const char *path, srs_record_t **recs_out, size_t *count_out)
{
    FILE *f;
    srs_hdr_t h;
    srs_record_t *arr = NULL;
    size_t cap = 0, n = 0, i;
    int rc = -1;

    if (!path || !recs_out || !count_out) return -1;
    *recs_out = NULL;
    *count_out = 0;

    f = fopen(path, "rb");
    if (!f) return 0;                    /* 文件不存在 = 全空，正常情况 */
    if (read_hdr(f, &h) != 0) {
        SRS_LOG("[srs] load: bad header %s\n", path);
        fclose(f);
        return -1;
    }

    cap = (size_t)h.rec_count + h.journal_len + 4;
    arr = (srs_record_t *)malloc(cap * sizeof(srs_record_t));
    if (!arr) { fclose(f); return -1; }

    /* 主记录区 + CRC 校验 */
    if (h.rec_count > 0) {
        srs_disk_rec_t *disk =
            (srs_disk_rec_t *)malloc(h.rec_count * sizeof(srs_disk_rec_t));
        if (!disk) goto out;
        if (fread(disk, sizeof(srs_disk_rec_t), h.rec_count, f) != h.rec_count) {
            SRS_LOG("[srs] load: short main area\n");
            free(disk);
            goto out;
        }
        if (crc32_calc(disk, h.rec_count * sizeof(srs_disk_rec_t)) != h.crc32) {
            SRS_LOG("[srs] load: main area CRC mismatch\n");
            free(disk);
            goto out;
        }
        for (i = 0; i < h.rec_count; i++)
            rec_from_disk(&disk[i], &arr[i]);
        n = h.rec_count;
        free(disk);
    }

    /* 回放日志区：后写覆盖先写；读不满 64B 视为断电尾垃圾，直接丢弃 */
    for (i = 0; i < h.journal_len; i++) {
        srs_disk_rec_t d;
        size_t j;
        if (fread(&d, sizeof(d), 1, f) != 1) {
            SRS_LOG("[srs] load: journal tail garbage, drop %u..%u\n",
                    (unsigned)i, (unsigned)(h.journal_len - 1));
            break;
        }
        for (j = 0; j < n; j++) {
            if (arr[j].word_id == d.word_id) {
                rec_from_disk(&d, &arr[j]);   /* 覆盖旧值 */
                break;
            }
        }
        if (j == n) {                          /* 新 word_id：追加 */
            if (n >= cap) {
                srs_record_t *na;
                cap *= 2;
                na = (srs_record_t *)malloc(cap * sizeof(srs_record_t));
                if (!na) goto out;
                memcpy(na, arr, n * sizeof(srs_record_t));
                free(arr);
                arr = na;
            }
            rec_from_disk(&d, &arr[n++]);
        }
    }

    *recs_out = arr;
    *count_out = n;
    rc = 0;
out:
    if (rc != 0) free(arr);
    fclose(f);
    return rc;
}

int srs_store_recover_tmp(const char *path)
{
    char *tmp;
    char *bak;
    FILE *f;
    srs_hdr_t h;
    int valid;

    if (!path) return -2;
    tmp = (char *)malloc(strlen(path) + 5);
    bak = (char *)malloc(strlen(path) + 5);
    if (!tmp || !bak) { free(tmp); free(bak); return -2; }
    strcpy(tmp, path);
    strcat(tmp, ".tmp");
    strcpy(bak, path);
    strcat(bak, ".bak");

    /* .bak 残留恢复：上次合并"path→.bak 已发生、tmp→path 未发生"的断电窗口。
     * 仅当主文件缺失时才用 .bak 转正，避免覆盖更新的主文件 */
    f = fopen(path, "rb");
    if (!f) {
        FILE *bf = fopen(bak, "rb");
        if (bf) {
            fclose(bf);
            if (rename(bak, path) == 0) {
                SRS_LOG("[srs] recover: bak promoted\n");
            } else {
                SRS_LOG("[srs] recover: bak rename failed\n");
            }
        }
    } else {
        fclose(f);
        remove(bak);   /* 主文件在：.bak 是上个周期的残留，清掉 */
    }

    f = fopen(tmp, "rb");
    if (!f) { free(tmp); free(bak); return 0; }     /* 无残留 tmp：正常 */

    valid = (validate_image(f, &h) == 0);
    fclose(f);

    if (valid) {
        /* tmp 转正同样受"FAT rename 不覆盖"约束：先挪走旧主文件 */
        remove(bak);
        (void)rename(path, bak);
        if (rename(tmp, path) != 0) {
            SRS_LOG("[srs] recover: rename %s failed\n", tmp);
            (void)rename(bak, path);
            free(tmp);
            free(bak);
            return -2;
        }
        remove(bak);
        SRS_LOG("[srs] recover: tmp promoted (%u recs)\n", (unsigned)h.rec_count);
        free(tmp);
        free(bak);
        return 1;
    }
    SRS_LOG("[srs] recover: tmp corrupt, discard\n");
    remove(tmp);
    free(tmp);
    free(bak);
    return -1;
}

int srs_store_append_journal(const char *path, const srs_record_t *rec)
{
    FILE *f;
    srs_hdr_t h;
    srs_disk_rec_t d;
    long end;

    if (!path || !rec) return -1;

    f = fopen(path, "r+b");
    if (!f) {
        /* 首次写入：建空文件头（主记录区空，CRC=0） */
        f = fopen(path, "w+b");
        if (!f) {
            SRS_LOG("[srs] append: cannot create %s\n", path);
            return -1;
        }
        memset(&h, 0, sizeof(h));
        memcpy(h.magic, "SRS1", 4);
        h.version = 1;
        if (fwrite(&h, sizeof(h), 1, f) != 1) {
            SRS_LOG("[srs] append: header write failed\n");
            fclose(f);
            return -1;
        }
    } else if (read_hdr(f, &h) != 0) {
        SRS_LOG("[srs] append: bad header %s\n", path);
        fclose(f);
        return -1;
    }

    /* 追加位置按头部计算而非 SEEK_END：断电留下的尾垃圾不影响定位 */
    end = (long)sizeof(srs_hdr_t) +
          (long)(h.rec_count + h.journal_len) * (long)sizeof(srs_disk_rec_t);
    if (fseek(f, end, SEEK_SET) != 0) { fclose(f); return -1; }

    rec_to_disk(rec, &d);
    if (fwrite(&d, sizeof(d), 1, f) != 1) {
        SRS_LOG("[srs] append: record write failed\n");
        fclose(f);
        return -1;
    }

    h.journal_len++;
    if (fseek(f, 0, SEEK_SET) != 0 ||
        fwrite(&h, sizeof(h), 1, f) != 1) {
        SRS_LOG("[srs] append: header rewrite failed\n");
        fclose(f);
        return -1;
    }
    if (flush_sync(f) != 0) { fclose(f); return -1; }
    fclose(f);
    return (int)h.journal_len;
}

int srs_store_compact(const char *path, const srs_record_t *recs, size_t count)
{
    char *tmp = NULL;
    char *bak = NULL;
    FILE *f = NULL;
    srs_hdr_t h;
    srs_record_t *sorted = NULL;
    srs_disk_rec_t *disk = NULL;
    int rc = -1;
    size_t i;

    if (!path || (count > 0 && !recs)) return -1;

    /* 保留旧头的 book_word_count / last_compact_unix（签名无 now，不读时钟） */
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "SRS1", 4);
    h.version = 1;
    {
        FILE *old = fopen(path, "rb");
        if (old) {
            srs_hdr_t oh;
            if (read_hdr(old, &oh) == 0) {
                h.book_word_count   = oh.book_word_count;
                h.last_compact_unix = oh.last_compact_unix;
            }
            fclose(old);
        }
    }

    /* 主记录区按 word_id 升序（布局要求） */
    if (count > 0) {
        sorted = (srs_record_t *)malloc(count * sizeof(srs_record_t));
        disk = (srs_disk_rec_t *)malloc(count * sizeof(srs_disk_rec_t));
        if (!sorted || !disk) {
            SRS_LOG("[srs] compact: OOM\n");
            goto out;
        }
        memcpy(sorted, recs, count * sizeof(srs_record_t));
        qsort(sorted, count, sizeof(srs_record_t), cmp_word_id);
        for (i = 0; i < count; i++)
            rec_to_disk(&sorted[i], &disk[i]);
        h.crc32 = crc32_calc(disk, count * sizeof(srs_disk_rec_t));
    }
    h.rec_count = (uint32_t)count;
    h.journal_len = 0;

    tmp = (char *)malloc(strlen(path) + 5);
    if (!tmp) goto out;
    strcpy(tmp, path);
    strcat(tmp, ".tmp");

    f = fopen(tmp, "wb");
    if (!f) {
        SRS_LOG("[srs] compact: cannot open %s\n", tmp);
        goto out;
    }
    if (fwrite(&h, sizeof(h), 1, f) != 1) goto out;
    if (count > 0 &&
        fwrite(disk, sizeof(srs_disk_rec_t), count, f) != count) goto out;
    if (flush_sync(f) != 0) goto out;
    fclose(f);
    f = NULL;

    /* FatFs 的 rename 不覆盖已存在目标（POSIX 覆盖语义在 FAT 上不成立，
     * 真机实测 rename 直接失败），改为 .bak 三段轮换：
     *   1) 删旧 .bak；2) path → .bak（path 不存在 = 首次合并，跳过）；
     *   3) .tmp → path；4) 删 .bak。
     * 断电窗口分析：2) 后断电 → path 缺失但 .bak 完整（recover_tmp 转正）；
     * 3) 后断电 → 新数据已在 path。任何时点已合并数据不丢。 */
    bak = (char *)malloc(strlen(path) + 5);
    if (!bak) goto out;
    strcpy(bak, path);
    strcat(bak, ".bak");
    remove(bak);
    (void)rename(path, bak);
    if (rename(tmp, path) != 0) {
        SRS_LOG("[srs] compact: rename failed\n");
        (void)rename(bak, path);   /* 回滚：把旧主记录请回来 */
        goto out;
    }
    remove(bak);
    /* 目录 fsync 的替代兜底：reopen + fsync（docs §3e） */
    f = fopen(path, "rb");
    if (f) {
        (void)fsync(fileno(f));
        fclose(f);
        f = NULL;
    }
    rc = 0;
out:
    if (f) fclose(f);
    if (rc != 0 && tmp) remove(tmp);
    free(tmp);
    free(bak);
    free(sorted);
    free(disk);
    return rc;
}

/* ---------------- .stats ---------------- */

/* unix 秒 -> yyyymmdd（Howard Hinnant civil_from_days，纯整数，不读时钟） */
static uint32_t unix_to_yyyymmdd(uint32_t now)
{
    int64_t days = (int64_t)(now / 86400u);
    int64_t z = days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    uint64_t m = (mp < 10) ? (mp + 3) : (mp - 9);
    y += (m <= 2);
    return (uint32_t)((uint64_t)y * 10000u + m * 100u + d);
}

/* 读整个 .stats（校验 magic/version/CRC）。文件不存在返回 0 且 *recs=NULL,*n=0；
   损坏返回 -1（数据已释放）。 */
static int stats_load_all(const char *path, sta_rec_t **recs, uint32_t *n)
{
    FILE *f;
    sta_hdr_t h;
    long fsize;
    uint8_t *img = NULL;
    int rc = -1;

    *recs = NULL;
    *n = 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    if (fsize < (long)sizeof(sta_hdr_t) ||
        ((uint32_t)fsize - sizeof(sta_hdr_t)) % sizeof(sta_rec_t) != 0) {
        SRS_LOG("[srs] stats: bad size\n");
        fclose(f);
        return -1;
    }
    img = (uint8_t *)malloc((size_t)fsize);
    if (!img) { fclose(f); return -1; }
    if (fread(img, 1, (size_t)fsize, f) != (size_t)fsize) goto out;
    memcpy(&h, img, sizeof(h));
    if (memcmp(h.magic, "STA1", 4) != 0 || h.version != 1) {
        SRS_LOG("[srs] stats: bad magic/version\n");
        goto out;
    }
    if (h.rec_count * sizeof(sta_rec_t) + sizeof(sta_hdr_t) != (uint32_t)fsize) {
        SRS_LOG("[srs] stats: rec_count mismatch\n");
        goto out;
    }
    memset(img + 12, 0, 4);   /* crc 字段置 0 后校验 */
    if (crc32_calc(img, (size_t)fsize) != h.crc32) {
        SRS_LOG("[srs] stats: CRC mismatch\n");
        goto out;
    }
    if (h.rec_count > 0) {
        *recs = (sta_rec_t *)malloc(h.rec_count * sizeof(sta_rec_t));
        if (!*recs) goto out;
        memcpy(*recs, img + sizeof(sta_hdr_t), h.rec_count * sizeof(sta_rec_t));
    }
    *n = h.rec_count;
    rc = 0;
out:
    free(img);
    fclose(f);
    return rc;
}

/* 全量重写 .stats（记录数少，原地更新的收益不值得断电风险；整写+fsync 更简单安全） */
static int stats_write_all(const char *path, const sta_rec_t *recs, uint32_t n)
{
    FILE *f;
    sta_hdr_t h;
    size_t total = sizeof(sta_hdr_t) + (size_t)n * sizeof(sta_rec_t);
    uint8_t *img;
    int rc = -1;

    img = (uint8_t *)malloc(total);
    if (!img) return -1;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "STA1", 4);
    h.version = 1;
    h.rec_count = n;
    h.crc32 = 0;
    memcpy(img, &h, sizeof(h));
    if (n > 0)
        memcpy(img + sizeof(h), recs, (size_t)n * sizeof(sta_rec_t));
    h.crc32 = crc32_calc(img, total);
    memcpy(img, &h, sizeof(h));   /* 回填 CRC */

    f = fopen(path, "wb");
    if (!f) goto out;
    if (fwrite(img, 1, total, f) != total) { fclose(f); goto out; }
    if (flush_sync(f) != 0) { fclose(f); goto out; }
    fclose(f);
    rc = 0;
out:
    free(img);
    return rc;
}

int srs_stats_update_today(const char *stats_path, uint32_t now,
                           int is_new_word, srs_quality_t q)
{
    sta_rec_t *recs = NULL;
    sta_rec_t *cur;
    uint32_t n = 0, today, yesterday;
    int rc;

    if (!stats_path) return -1;
    if (stats_load_all(stats_path, &recs, &n) != 0) {
        SRS_LOG("[srs] stats: corrupt file, rebuild from scratch\n");
        free(recs);
        recs = NULL;
        n = 0;
    }

    today = unix_to_yyyymmdd(now);
    yesterday = (now >= 86400u) ? unix_to_yyyymmdd(now - 86400u) : 0u;

    if (n > 0 && recs[n - 1].date_yyyymmdd == today) {
        cur = &recs[n - 1];               /* 当日已有记录：原位更新 */
    } else {
        sta_rec_t *nr = (sta_rec_t *)malloc((n + 1) * sizeof(sta_rec_t));
        if (!nr) { free(recs); return -1; }
        if (n > 0) memcpy(nr, recs, n * sizeof(sta_rec_t));
        free(recs);
        recs = nr;
        cur = &recs[n];
        memset(cur, 0, sizeof(*cur));
        cur->date_yyyymmdd = today;
        /* streak：昨日有记录则昨日+1，否则重新从 1 起（断签即停滞重计） */
        if (n > 0 && recs[n - 1].date_yyyymmdd == yesterday)
            cur->streak_days = (uint16_t)(recs[n - 1].streak_days + 1);
        else
            cur->streak_days = 1;
        n++;
    }

    if (is_new_word) cur->new_learned++;
    else             cur->reviewed++;
    if (q >= SRS_Q_BLURRY) cur->correct++;
    else                   cur->wrong++;

    rc = stats_write_all(stats_path, recs, n);
    free(recs);
    return rc;
}

int srs_stats_load_recent(const char *stats_path, srs_stat_day_t *days_out,
                          size_t max_n)
{
    sta_rec_t *recs = NULL;
    uint32_t n = 0, start, i, take;

    if (!stats_path || !days_out || max_n == 0) return -1;
    if (stats_load_all(stats_path, &recs, &n) != 0) return -1;
    if (n == 0) return 0;

    take = (n < max_n) ? n : (uint32_t)max_n;
    start = n - take;
    for (i = 0; i < take; i++) {
        const sta_rec_t *s = &recs[start + i];
        days_out[i].date_yyyymmdd = s->date_yyyymmdd;
        days_out[i].new_learned   = s->new_learned;
        days_out[i].reviewed      = s->reviewed;
        days_out[i].correct       = s->correct;
        days_out[i].wrong         = s->wrong;
        days_out[i].streak_days   = s->streak_days;
        days_out[i].study_secs    = s->study_secs;
    }
    free(recs);
    return (int)take;
}
