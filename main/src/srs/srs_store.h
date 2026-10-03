/**
 * srs_store.h —— 持久化（纯 C99，仅用 stdio；布局见 docs/srs_persistence.md）
 * 断电安全：日志追加写 + 满 20 条/休眠前合并到 .tmp + rename 原子替换。
 * 所有路径由调用方传入（如 "/sdcard/wordbook/cet6.wbx.srs"），本模块不感知挂载点。
 */
#ifndef MOKA_SRS_STORE_H
#define MOKA_SRS_STORE_H

#include <stddef.h>
#include <stdint.h>
#include "srs_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SRS_STORE_JOURNAL_LIMIT 20   /* 日志区满 20 条触发合并（FR-5） */

/* 每日统计记录（.stats，32B 定长，布局见 docs/srs_persistence.md §4） */
typedef struct {
    uint32_t date_yyyymmdd;
    uint16_t new_learned;
    uint16_t reviewed;
    uint16_t correct;
    uint16_t wrong;
    uint16_t streak_days;
    uint16_t study_secs;
} srs_stat_day_t;

/* 加载：读头+主记录区+回放日志区；文件不存在视为全空并返回 0 条 */
int  srs_store_load(const char *path, srs_record_t **recs_out, size_t *count_out);

/* 开机恢复：检测残留 .srs.tmp（合并中途断电），CRC 合法则转正，否则丢弃；
 * 同时处理 .bak 残留（三段轮换中途断电：主文件缺失时 .bak 转正） */
int  srs_store_recover_tmp(const char *path);

/* 每次自评后调用：追加 64B 日志；返回当前日志区长度（达到上限由调用方触发合并） */
int  srs_store_append_journal(const char *path, const srs_record_t *rec);

/* 合并：全量写 .srs.tmp → fsync → .bak 三段轮换转正（FAT rename 不覆盖，
 * 见 srs_store.c 文件头）；journal_len 归零 */
int  srs_store_compact(const char *path, const srs_record_t *recs, size_t count);

/* 统计：更新当日记录（原位更新/跨天追加），维护 streak_days（FR-6） */
int  srs_stats_update_today(const char *stats_path, uint32_t now,
                            int is_new_word, srs_quality_t q);

/* 取最近 max_n 天统计（柱状图用，FR-6），返回实际天数 */
int  srs_stats_load_recent(const char *stats_path, srs_stat_day_t *days_out, size_t max_n);

#ifdef __cplusplus
}
#endif
#endif
