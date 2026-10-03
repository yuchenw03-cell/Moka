/**
 * app_fsm.cpp —— 顶层状态机（实现 app_fsm.h）
 *
 * 状态：BOOT → MENU ⇄ SETTINGS / REVIEW_FRONT ⇄ REVIEW_BACK / STATS → SLEEP
 *
 * 任务模型（架构 §2）：
 * - main_fsm_task（core 1，16KB 栈）：唯一消费者，所有状态转移/队列调度/
 *   持久化在此串行发生，无锁；
 * - 消息队列深度 16：M5ReadPaper 队列深度 10 在高频触摸下丢消息的教训（架构 §6），
 *   放宽到 16 仍单任务串行消费；
 * - 触摸采集：本循环 10ms 超时轮询 touch_poll，事件投入队列后统一消费；
 *   app_fsm_post 同时暴露给未来的独立输入任务（架构 §2 DeviceInterruptTask）。
 *
 * 刷屏纪律：所有页面渲染函数末尾恰好一次 disp_push_partial
 * （ui_render_commit），本文件不直接调用刷屏 API。
 */
#include "app_fsm.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "app_internal.h"
#include "wordbook.h"
#include "review.h"
#include "settings.h"
#include "power.h"
#include "ui/render.h"
#include "ui/menu.h"
#include "ui/stats.h"
#include "ui/card.h"

#include "display.h"
#include "touch.h"
#include "button.h"
#include "sdcard.h"
#include "rtc.h"
#include "srs_store.h"
#include "srs_engine.h"
#include "log.h"

#define TAG "fsm"

/* 队列深度 16（M5ReadPaper 深度 10 丢消息教训，见 app_fsm.h 注释） */
#define MSG_QUEUE_DEPTH   16
/* 输入轮询周期 10ms（架构 §2） */
#define POLL_PERIOD_MS    10
/* 主任务栈 16KB、core 1（架构 §2） */
#define FSM_STACK_BYTES   16384
#define FSM_TASK_PRIO     5
#define FSM_TASK_CORE     1

app_ctx_t g_app;

static QueueHandle_t s_msgq;

bool app_fsm_post(const app_msg_t *m)
{
    if (!s_msgq || !m) return false;
    return xQueueSend(s_msgq, m, 0) == pdTRUE;   /* 不阻塞：满则丢，记日志由调用方 */
}

void app_set_message(const char *l1, const char *l2, bool fatal)
{
    snprintf(g_app.msg1, sizeof(g_app.msg1), "%s", l1 ? l1 : "");
    snprintf(g_app.msg2, sizeof(g_app.msg2), "%s", l2 ? l2 : "");
    g_app.msg_fatal = fatal;
}

/* ===== 菜单页渲染（概要惰性计算：未缓存的项现场读 .srs） ===== */
static void render_menu(void)
{
    int total_pages = 1;
    if (g_app.book_count > 0) {
        total_pages = ((int)g_app.book_count + HIT_MENU_PAGE_ROWS - 1) / HIT_MENU_PAGE_ROWS;
    }
    if (g_app.menu_page >= total_pages) g_app.menu_page = total_pages - 1;
    if (g_app.menu_page < 0) g_app.menu_page = 0;

    int first = g_app.menu_page * HIT_MENU_PAGE_ROWS;
    int n = (int)g_app.book_count - first;
    if (n > HIT_MENU_PAGE_ROWS) n = HIT_MENU_PAGE_ROWS;
    if (n < 0) n = 0;

    ui_menu_item_t items[HIT_MENU_PAGE_ROWS];   /* 8×~20B，栈上远小于 4KB 上限 */
    for (int i = 0; i < n; i++) {
        book_info_t *bi = &g_app.books[first + i];
        if (!bi->summary_valid) wordbook_compute_summary(first + i);
        items[i].name    = bi->name;
        items[i].total   = bi->total;
        items[i].learned = bi->learned;
        items[i].due     = bi->due;
        items[i].current = ((int)(first + i) == g_app.cur_book);
    }
    ui_menu_render(items, n, g_app.menu_page, total_pages);
}

/* ===== 统计页数据汇集（FR-6）：队列/.stats/生词本扫描 ===== */
static void render_stats(void)
{
    uint32_t now = rtc_get_unix();
    uint32_t due = 0, difficult = 0;
    if (g_app.recs) {
        for (size_t i = 0; i < g_app.rec_count; i++) {
            const srs_record_t *r = &g_app.recs[i];
            if (r->state != 0 && r->due_unix <= now) due++;
            if (srs_engine_is_difficult(r)) difficult++;
        }
    }

    srs_stat_day_t days[30];   /* 30×32B=960B，栈上安全 */
    int nd = srs_stats_load_recent(MOKA_STATS_PATH, days, 30);
    if (nd < 0) {
        MOKA_LOGE(TAG, "统计读取失败: %s", MOKA_STATS_PATH);
        nd = 0;
    }
    if (nd > 30) nd = 30;   /* 防御：柱状图缓冲上限 */

    /* 今日记录（date_yyyymmdd 匹配）里取 新学/已复习/连续打卡 */
    time_t t = (time_t)now;
    struct tm tmv;
    gmtime_r(&t, &tmv);
    uint32_t today = (uint32_t)((tmv.tm_year + 1900) * 10000
                     + (tmv.tm_mon + 1) * 100 + tmv.tm_mday);
    uint32_t new_today = 0, reviewed_today = 0, streak = 0;
    for (int i = 0; i < nd; i++) {
        if (days[i].date_yyyymmdd == today) {
            new_today      = days[i].new_learned;
            reviewed_today = days[i].reviewed;
            streak         = days[i].streak_days;
        }
    }
    /* 今日尚无记录时，连续打卡取最近一条历史记录的 streak（断签停滞语义，sm2 §4） */
    if (streak == 0 && nd > 0) streak = days[nd - 1].streak_days;

    ui_stats_render(due, new_today, reviewed_today, streak, days, nd, difficult);
}

/* ===== 跨模块导航：切状态 + 渲染（REVIEW 各态由 review.cpp 自渲染） ===== */
void app_navigate(app_state_t st)
{
    g_app.state = st;
    switch (st) {
    case ST_MENU:
        render_menu();
        break;
    case ST_SETTINGS:
        ui_settings_render(g_app.daily_new, g_app.default_mode, rtc_get_unix(),
                           g_app.word_scale2);
        break;
    case ST_STATS:
        render_stats();
        break;
    case ST_MESSAGE:
        ui_card_message(g_app.msg1, g_app.msg2);
        break;
    default:
        break;
    }
}

/* ===== 菜单页触摸 ===== */
static void menu_on_touch(const app_msg_t *m)
{
    if (m->touch_kind != TOUCH_EV_PRESS) return;
    int x = m->x, y = m->y;

    if (y < HIT_HEADER_H) {
        if (x >= HIT_MENU_PWR_X0 && x < HIT_MENU_PWR_X1) {
            /* 屏幕关机键（物理电源键为 PMS150G 硬件复位不可拦截，见 render.h）：
             * 保存全部状态 → 墨卡关机页全刷 → GPIO44 断电，不返回 */
            power_shutdown();
        } else if (x >= HIT_MENU_STATS_X0 && x < HIT_MENU_STATS_X1) {
            app_navigate(ST_STATS);
        } else if (x >= HIT_MENU_SET_X0 && x < HIT_MENU_SET_X1) {
            app_navigate(ST_SETTINGS);
        }
        return;
    }

    if (y >= HIT_MENU_BOTTOM_Y0) {
        /* 底部翻页栏（仅多页时有效） */
        int total_pages = ((int)g_app.book_count + HIT_MENU_PAGE_ROWS - 1) / HIT_MENU_PAGE_ROWS;
        if (total_pages > 1) {
            if (x >= HIT_MENU_PREV_X0 && x < HIT_MENU_PREV_X1 && g_app.menu_page > 0) {
                g_app.menu_page--;
                render_menu();
            } else if (x >= HIT_MENU_NEXT_X0 && x < HIT_MENU_NEXT_X1
                       && g_app.menu_page < total_pages - 1) {
                g_app.menu_page++;
                render_menu();
            }
        }
        return;
    }

    /* 词书行：点击 = 选中并立即开始今日复习（对标墨墨"点书学习"） */
    int row = (y - HIT_MENU_ROW_Y0) / HIT_MENU_ROW_H;
    int idx = g_app.menu_page * HIT_MENU_PAGE_ROWS + row;
    if (idx < 0 || (size_t)idx >= g_app.book_count) return;
    if ((size_t)idx >= (size_t)(g_app.menu_page * HIT_MENU_PAGE_ROWS)
                          + HIT_MENU_PAGE_ROWS) return;
    if (idx != g_app.cur_book) {
        if (wordbook_open_current(idx) != 0) {
            app_set_message("词书打开失败", "点击返回", false);
            app_navigate(ST_MESSAGE);
            return;
        }
    }
    review_start(REVIEW_NORMAL);
}

/* ===== 统计页触摸：右上返回；底部生词本入口 ===== */
static void stats_on_touch(const app_msg_t *m)
{
    if (m->touch_kind != TOUCH_EV_PRESS) return;
    int x = m->x, y = m->y;

    if (y < HIT_STATS_BACK_Y1 && x >= HIT_STATS_BACK_X0) {
        app_navigate(ST_MENU);
        return;
    }
    if (y >= HIT_STATS_DIFF_Y0 && y < HIT_STATS_DIFF_Y1
        && x >= HIT_STATS_DIFF_X0 && x < HIT_STATS_DIFF_X1) {
        /* 生词本单独成册复习（复用复习流程，队列只由生词构成，FR-6） */
        review_start(REVIEW_DIFFICULT);
    }
}

/* ===== 消息分发（单任务串行消费） ===== */
static void dispatch(const app_msg_t *m)
{
    if (m->type != MSG_TOUCH) return;
    power_on_activity();   /* 任何触摸事件重置闲置计时（FR-7） */

    switch (g_app.state) {
    case ST_MENU:     menu_on_touch(m);        break;
    case ST_SETTINGS: settings_handle_touch(m); break;
    case ST_REVIEW_FRONT:
    case ST_REVIEW_BACK:
        review_handle_touch(m);
        break;
    case ST_STATS:    stats_on_touch(m);       break;
    case ST_MESSAGE:
        /* 致命错误页不响应触摸（触摸误触率实测过高），
         * 由主循环轮询的电源键走 power_shutdown 关机；
         * 非致命提示仍允许触摸返回菜单。 */
        if (m->touch_kind == TOUCH_EV_PRESS && !g_app.msg_fatal) {
            app_navigate(ST_MENU);
        }
        break;
    default: break;
    }
}

/* ===== BOOT：初始化 + 唤醒恢复 + 首次设时检查 ===== */
static void boot(void)
{
    /* 顺序（main.cpp 注释）：RTC → SD → 显示 → 字体 → 设置 → 词书 → 触摸 */
    if (!rtc_clock_init()) MOKA_LOGE(TAG, "RTC 初始化失败（时间语义将不可用）");

    button_init();   /* 电源键轮询（GPIO0），主循环任何状态按下即关机 */

    bool sd_ok = sdcard_mount();
    if (!sd_ok) MOKA_LOGE(TAG, "SD 卡挂载失败");

    if (!disp_init()) MOKA_LOGE(TAG, "显示初始化失败");
    disp_clear(UI_GRAY_WHITE);
    /* 每次开机（含唤醒重启）都强制物理清屏：墨水屏断电后仍保留上一画面
     *（如前固件 EDC 书页），高层刷新的 diff 机制会把它当成"无需更新"
     * 跳过驱屏导致残影叠加，必须用低层直驱刷白（disp_force_clear）。
     * 代价是开机多一次约 1s 的清屏闪烁，换取任何前态下画面干净。 */
    disp_force_clear();

    /* 中文字体：SD 卡 /sdcard/font/cn.bin 优先（免烧分区即可换字体），
     * 缺失时回退 font 分区字库（兜底保证有字可显） */
    g_app.font_cn = font_open_sd(MOKA_CN_FONT_PATH);
    if (g_app.font_cn) {
        MOKA_LOGI(TAG, "中文字体：SD 卡 %s", MOKA_CN_FONT_PATH);
    } else {
        MOKA_LOGI(TAG, "SD 中文字体缺失，回退 font 分区");
        g_app.font_cn = font_open_partition("font");
    }
    if (!g_app.font_cn) MOKA_LOGE(TAG, "中文字体加载失败（SD 与分区均不可用）");
    g_app.font_ipa = font_open_sd(MOKA_IPA_FONT_PATH);   /* 可缺失：音标行降级（NFR-5） */
    if (!g_app.font_ipa) MOKA_LOGI(TAG, "IPA 字体缺失，音标行将隐藏");
    ui_render_init(g_app.font_cn, g_app.font_ipa);

    settings_load();

    if (!sd_ok) {
        app_set_message("SD 卡挂载失败", "请插卡后按电源键关机", true);
        app_navigate(ST_MESSAGE);
        if (!touch_init()) MOKA_LOGE(TAG, "触摸初始化失败");
        return;
    }

    int nb = wordbook_scan();
    if (nb < 0) {
        app_set_message("词书目录读取失败", "请按电源键关机", true);
        app_navigate(ST_MESSAGE);
        if (!touch_init()) MOKA_LOGE(TAG, "触摸初始化失败");
        return;
    }

    /* 唤醒恢复（FR-7b）：触摸唤醒 + RTC 快照有效 → 直接断点续读 */
    if (nb > 0 && power_try_resume()) {
        if (!touch_init()) MOKA_LOGE(TAG, "触摸初始化失败");
        return;
    }

    if (nb == 0) {
        /* 无词书不阻塞：直接进菜单（空列表），设置/统计等基础功能照常可用。
         * 词书缺失提示打在日志里；菜单空白本身即提示。 */
        MOKA_LOGE(TAG, "未找到词书，请将 .wbx 放入 /sdcard/wordbook/（进菜单降级运行）");
        app_navigate(ST_MENU);
    } else {
        /* 默认打开第一本，供统计页/生词本直接用 */
        if (wordbook_open_current(0) != 0) {
            MOKA_LOGE(TAG, "默认词书打开失败");
        }
        /* 首次启动时间检查：BM8563 无有效时间 → 进设置页手动设时（FR-7c） */
        if (rtc_get_unix() < MOKA_MIN_VALID_UNIX) {
            MOKA_LOGE(TAG, "RTC 时间无效（<2025），请先手动设时");
            app_navigate(ST_SETTINGS);
        } else {
            app_navigate(ST_MENU);
        }
    }

    /* 触摸最后初始化：开早了会吃进初始化阶段的乱触摸（main.cpp 注释） */
    if (!touch_init()) MOKA_LOGE(TAG, "触摸初始化失败");
}

static void fsm_task(void *arg)
{
    (void)arg;
    memset(&g_app, 0, sizeof(g_app));
    g_app.cur_book = -1;

    boot();
    power_on_activity();

    for (;;) {
        app_msg_t m;
        if (xQueueReceive(s_msgq, &m, pdMS_TO_TICKS(POLL_PERIOD_MS)) == pdTRUE) {
            dispatch(&m);
        } else {
            /* 10ms 轮询：采集→投递，消费仍走队列（单消费者） */
            touch_event_t ev;
            while (touch_poll(&ev)) {
                app_msg_t tm = {
                    .type = MSG_TOUCH,
                    .touch_kind = (uint8_t)ev.kind,
                    .x = ev.x, .y = ev.y,
                };
                if (!app_fsm_post(&tm)) {
                    MOKA_LOGE(TAG, "消息队列满，丢弃触摸事件 kind=%d", (int)ev.kind);
                }
            }
        }
        /* 电源键 = 关机（任何状态全局生效；已无"重启"概念）。
         * 单任务模型保证此检查只发生在两次 dispatch 之间，
         * 不会打断 SRS 写盘等临界段；power_shutdown 不返回。 */
        if (button_pressed()) {
            power_shutdown();
        }
        power_check_idle();
    }
}

void app_fsm_start(void)
{
    s_msgq = xQueueCreate(MSG_QUEUE_DEPTH, sizeof(app_msg_t));
    if (!s_msgq) {
        MOKA_LOGE(TAG, "消息队列创建失败");
        return;
    }
    if (xTaskCreatePinnedToCore(fsm_task, "main_fsm", FSM_STACK_BYTES, NULL,
                                FSM_TASK_PRIO, NULL, FSM_TASK_CORE) != pdPASS) {
        MOKA_LOGE(TAG, "main_fsm_task 创建失败");
    }
}
