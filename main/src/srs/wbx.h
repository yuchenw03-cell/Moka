/**
 * wbx.h —— .wbx 词书读取（纯 C99，仅用 stdio；格式见 docs/wbx_format.md）
 * 设计：固定 48B 索引可定点 fread，文本区按需读取，不做全量载入。
 */
#ifndef MOKA_SRS_WBX_H
#define MOKA_SRS_WBX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)
typedef struct {
    char     magic[4];       /* "WBX1" */
    uint32_t version;        /* = 1 */
    uint32_t word_count;
    uint32_t meta_ofs;       /* 固定 0x40 */
    uint32_t text_ofs;
    uint32_t name_len;
    char     name[32];
    uint32_t crc32;
    uint32_t reserved;
} wbx_header_t;              /* 64 B */

typedef struct {
    uint32_t word_id;
    uint8_t  freq_bucket;    /* 0-9 */
    uint8_t  pos;
    uint16_t word_len;       /* 含 \0 */
    uint16_t phon_len;       /* 可为 0 = 无音标 */
    uint16_t gloss_len;
    uint16_t ex_len;         /* 可为 0；含 SOH/STX 高亮包裹时 flags.bit0=1 */
    uint16_t flags;
    uint32_t text_ofs;       /* 相对文本区起点，四段连续 word\0 phon\0 gloss\0 ex\0 */
    uint32_t distract[5];    /* 同桶干扰项 word_id；0xFFFFFFFF=空槽 */
    uint8_t  reserved[8];
} wbx_entry_t;               /* 48 B */
#pragma pack(pop)

#define WBX_DISTRACT_EMPTY 0xFFFFFFFFu

typedef struct wbx_book wbx_book_t;   /* 不透明句柄 */

/* 打开并校验 magic/version/CRC32；失败返回 NULL */
wbx_book_t *wbx_open(const char *path);
void        wbx_close(wbx_book_t *b);

uint32_t    wbx_word_count(const wbx_book_t *b);
const char *wbx_book_name(const wbx_book_t *b);

/* 按序号读索引项（0 <= idx < word_count） */
int wbx_read_entry(wbx_book_t *b, uint32_t idx, wbx_entry_t *out);

/* 读四段文本到调用方缓冲（各缓冲容量须 >= 对应 len 字段） */
int wbx_read_texts(wbx_book_t *b, const wbx_entry_t *e,
                   char *word, char *phon, char *gloss, char *example);

/* 模式 B 抽 3 个干扰项：仅从同桶 distract[5] 非空槽中取（FR-3 禁止跨桶）；
   返回实际抽到的数量（桶内不足时可为 1~2，上层降级为少选项） */
int wbx_pick_distractors(wbx_book_t *b, const wbx_entry_t *e, uint32_t out_ids[3]);

/* 按 word_id 找索引项（srs 记录 word_id -> 词条） */
int wbx_find_entry(wbx_book_t *b, uint32_t word_id, wbx_entry_t *out);

#ifdef __cplusplus
}
#endif
#endif
