/**
 * app_internal.h —— 应用层内部共享上下文（仅 app/ 与 ui/ 使用，不对 hal/srs 暴露）
 *
 * 设计要点：
 * - 全局唯一上下文 g_app，所有状态转移只在 main_fsm_task 单任务内发生（无锁）；
 * - 大数组一律走 moka_ps_malloc（PSRAM 优先，失败回退内部 RAM 并记日志）；
 * - recs 为"全量记录数组"：长度 = 词书 word_count，按下标即 word_id 直查。
 *   原因：srs_queue_build 只接受记录数组，而从未评分的新词没有 .srs 记录，
 *   由本层在打开词书时补默认记录（ef=2.5，state=新词），见 wordbook.cpp。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

#include "srs_engine.h"
#include "srs_queue.h"
#include "wbx.h"
#include "font.h"

/* ===== 路径常量（全离线，仅 SD 卡本地路径） ===== */
#define MOKA_WORDBOOK_DIR   "/sdcard/wordbook"          /* 词书目录（docs/srs_persistence.md §1） */
#define MOKA_STATS_PATH     "/sdcard/wordbook/.stats"   /* 全局每日统计（词书无关） */
#define MOKA_SETTINGS_PATH  "/sdcard/wordbook/moka.cfg" /* 设置文件 key=value */
#define MOKA_IPA_FONT_PATH  "/sdcard/font/ipa.bin"      /* IPA 音标字体（NFR-5，可缺失降级） */
#define MOKA_CN_FONT_PATH   "/sdcard/font/cn.bin"       /* 中文字体（SD 优先，便于换字体；缺失回退 font 分区） */

/* RTC 时间有效性下限：2025-01-01 00:00:00 UTC。低于此值视为 BM8563 未设时（FR-7c） */
#define MOKA_MIN_VALID_UNIX 1735689600u

/* 闲置休眠阈值（FR-7）：120 秒无触摸 */
#define MOKA_IDLE_SLEEP_MS  (120u * 1000u)

/* 每日新词上限取值范围（设置页可调） */
#define MOKA_DAILY_NEW_MIN  1
#define MOKA_DAILY_NEW_MAX  100
#define MOKA_DAILY_NEW_DEF  20   /* 默认 20（FR-4 §3） */

/* 单词字号取值范围（×2 存储：2=1.0x … 8=4.0x，步进 1 = 0.5x） */
#define MOKA_WORD_SCALE2_MIN  2
#define MOKA_WORD_SCALE2_MAX  8
#define MOKA_WORD_SCALE2_DEF  4    /* 默认 2.0x（1.0x = 原始小字） */

/* PSRAM 优先分配；失败回退 malloc（小对象兜底），调用方仍需判空 */
static inline void *moka_ps_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(n);
    return p;
}

/* ===== FSM 状态（app_fsm.h 注释的状态 + 设置页/提示页两个内部态） ===== */
typedef enum {
    ST_BOOT = 0,
    ST_MENU,           /* 开机菜单/词书列表 */
    ST_SETTINGS,       /* 设置页（新词上限/默认模式/手动设时） */
    ST_REVIEW_FRONT,   /* 卡片正面（模式 A 看词 / 模式 B 选择题面） */
    ST_REVIEW_BACK,    /* 卡片背面（模式 A 释义 + 三自评按钮） */
    ST_STATS,          /* 统计页（FR-6） */
    ST_MESSAGE,        /* 简单提示页（今日完成/生词本为空/致命错误） */
    ST_SLEEP,          /* 休眠收尾（进入轻睡前瞬态） */
} app_state_t;

/* ===== 复习会话类型 ===== */
typedef enum {
    REVIEW_NORMAL = 0,   /* 今日队列：到期词 + 新词（srs_queue 调度，FR-4 §3） */
    REVIEW_DIFFICULT,    /* 生词本专场（lapses>=2，FR-6），队列只由生词构成 */
    REVIEW_REMEDIAL,     /* 收卡补救轮：flags.bit1 的词各补一次模式 A（FR-3/sm2 §4） */
} review_kind_t;

/* ===== 消息（队列驱动，深度见 app_fsm.cpp） ===== */
typedef enum {
    MSG_NONE = 0,
    MSG_TOUCH,         /* 触摸事件（kind 取 touch_event_kind_t） */
} app_msg_type_t;

typedef struct {
    app_msg_type_t type;
    uint8_t  touch_kind;   /* TOUCH_EV_PRESS / RELEASE / DOUBLE */
    int16_t  x, y;         /* 逻辑坐标 540x960 */
} app_msg_t;

/* ===== 词书概要（菜单显示用，惰性计算 + 缓存） ===== */
typedef struct {
    char     path[96];       /* /sdcard/wordbook/xxx.wbx */
    char     srs_path[112];  /* path + ".srs" */
    char     name[32];       /* 词书名（wbx 头 name 字段） */
    uint32_t total;          /* 总词数（扫描时从 wbx 头读取） */
    uint32_t learned;        /* 已学 = srs 记录中 state!=新词 的条数 */
    uint32_t due;            /* 待复习 = state!=新词 且 due_unix<=now 的条数 */
    bool     summary_valid;  /* learned/due 是否已计算（复习后失效重算） */
} book_info_t;

/* ===== 全局应用上下文 ===== */
typedef struct {
    app_state_t state;
    int         menu_page;   /* 菜单翻页页码 */

    /* 词书 */
    book_info_t *books;      /* ps_malloc 数组 */
    size_t       book_count;
    int          cur_book;   /* -1 = 未打开任何词书 */
    wbx_book_t  *book;       /* 当前打开的 .wbx */
    srs_record_t *recs;      /* 全量记录数组（ps_malloc），长度 rec_count，下标=word_id */
    size_t       rec_count;

    /* 复习会话 */
    review_kind_t review_kind;
    srs_queue_t  *queue;                 /* REVIEW_NORMAL 用 */
    uint32_t     *id_list;               /* DIFFICULT/REMEDIAL 用（ps_malloc） */
    size_t        id_count, id_idx, id_cap;
    uint32_t      cur_word;              /* 当前卡片 word_id */
    bool          cur_valid;

    /* 当前卡片文本（ps_malloc，随卡片切换释放重配） */
    char *t_word, *t_phon, *t_gloss, *t_ex;
    uint8_t cur_pos;                     /* 词性枚举（wbx_entry_t.pos） */

    /* 模式 B 选项（mb_words 为 ps_malloc 一块 4*64B） */
    bool      mode_b;
    int       mb_count;        /* 实际选项数 2..4（干扰项不足时降级少选项） */
    int       mb_correct_pos;  /* 打乱后正确项下标 */
    char     *mb_words;

    /* 设置项（settings.cpp 维护） */
    uint16_t daily_new;        /* 每日新词上限，默认 20 */
    uint8_t  default_mode;     /* 默认复习模式 0=A 1=B */
    uint8_t  word_scale2;      /* 单词字号 ×2（2=1.0x … 8=4.0x，默认 4=2.0x） */

    /* 复习会话进度（复习页顶部"今日待学/待复习"与退出弹窗用） */
    uint16_t sess_new_total, sess_new_done;    /* 今日新词：总数/已学 */
    uint16_t sess_due_total, sess_due_done;    /* 今日到期：总数/已复习 */
    bool     exit_confirm;                     /* true = 退出确认弹窗打开中 */

    /* 字体（font_ipa 可为 NULL = 音标行降级隐藏，NFR-5） */
    moka_font_t *font_cn;
    moka_font_t *font_ipa;

    /* ST_MESSAGE 提示页文本 */
    char msg1[64];
    char msg2[64];
    bool msg_fatal;            /* true = 致命页（不响应触摸，只能电源键关机） */

    int64_t  last_activity_ms; /* 闲置计时（esp_timer_get_time/1000，FR-7） */
    uint32_t session_start_unix;
} app_ctx_t;

extern app_ctx_t g_app;

/* ===== 跨模块导航接口（由 app_fsm.cpp 实现，供各页面模块调用） ===== */
void app_navigate(app_state_t st);                          /* 切页并渲染 */
void app_set_message(const char *l1, const char *l2, bool fatal);
bool app_fsm_post(const app_msg_t *m);                      /* 投递消息（输入任务/内部） */
