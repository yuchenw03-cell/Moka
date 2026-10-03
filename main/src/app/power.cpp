/**
 * power.cpp —— 电源管理实现（FR-7 + 三触唤醒机制）
 *
 * 休眠序列（闲置 120s 无触摸触发，顺序严格）：
 *   srs_store_compact → disp_push_full（清残影）→ sdcard_flush_and_sleep
 *   → 保存 RTC 快照（冷启动续读兜底）→ 轻睡眠循环
 *
 * 为什么用轻睡眠而不是深睡眠：GT911 触摸中断脚 GPIO48 不在 ESP32-S3 的
 * RTC GPIO 域（0~21），深睡 ext1 唤醒物理不可用（真机实测"休眠后唤不醒"）。
 * 轻睡的 gpio_wakeup 支持任意数字 GPIO，且 RAM/PSRAM 全程保持——
 * g_app、帧缓冲、屏幕画面原样保留，唤醒零重绘。代价是功耗高于深睡
 * （µA 级 → mA 级以下），这是触摸唤醒的必要代价（用户已裁决）。
 *
 * 唤醒确认（防误触）：触摸中断唤醒后，1100ms 内再累计满 3 次 PRESS 才
 * 正式唤醒，否则回到轻睡继续等。确认期间的触摸事件被本模块消费，
 * 不会漏给上层 UI。
 */
#include "power.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "display.h"
#include "sdcard.h"
#include "touch.h"
#include "powerctl.h"
#include "srs_store.h"
#include "review.h"
#include "wordbook.h"
#include "ui/shutdown.h"
#include "log.h"

#define TAG "power"

/* 断点快照魔数 "MKA1" */
#define RTC_SNAP_MAGIC 0x4D4B4131u

/* 三触唤醒确认参数（用户指定）：窗口 1100ms 内累计 3 次 PRESS */
#define MOKA_WAKE_TAP_COUNT    3
#define MOKA_WAKE_WINDOW_MS    1100
/* 唤醒确认轮询周期（与主循环同节奏） */
#define MOKA_WAKE_POLL_MS      10

typedef struct {
    uint32_t magic;
    uint32_t checksum;      /* 简单累加和校验（魔数+路径字节+word_id） */
    char     book_path[96]; /* 当前词书路径 */
    uint32_t cur_word;      /* 队列位置 = 断点词 word_id */
} rtc_snapshot_t;

static RTC_DATA_ATTR rtc_snapshot_t s_snap;

static uint32_t snap_checksum(const rtc_snapshot_t *s)
{
    uint32_t sum = s->magic + s->cur_word;
    for (size_t i = 0; i < sizeof(s->book_path) && s->book_path[i]; i++) {
        sum += (uint8_t)s->book_path[i];
    }
    return sum;
}

static void save_snapshot(void)
{
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.magic = RTC_SNAP_MAGIC;
    if (g_app.cur_book >= 0) {
        snprintf(s_snap.book_path, sizeof(s_snap.book_path), "%s",
                 g_app.books[g_app.cur_book].path);
    }
    s_snap.cur_word = g_app.cur_valid ? g_app.cur_word : 0xFFFFFFFFu;
    s_snap.checksum = snap_checksum(&s_snap);
}

static bool snapshot_valid(void)
{
    return s_snap.magic == RTC_SNAP_MAGIC
        && s_snap.checksum == snap_checksum(&s_snap)
        && s_snap.book_path[0] != '\0'
        && s_snap.cur_word != 0xFFFFFFFFu;
}

void power_snapshot_clear(void)
{
    s_snap.magic = 0;
    s_snap.checksum = 0;
}

void power_on_activity(void)
{
    g_app.last_activity_ms = esp_timer_get_time() / 1000;
}

/* 唤醒确认：1100ms 窗口内累计满 3 次 PRESS 返回 true。
 * 触发轻睡唤醒的那次触摸通常还留在 GT911 缓冲里，第一次轮询即可计为第 1 触。 */
static bool wait_triple_tap(void)
{
    int taps = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)MOKA_WAKE_WINDOW_MS * 1000;
    while (esp_timer_get_time() < deadline) {
        touch_event_t ev;
        while (touch_poll(&ev)) {
            if (ev.kind != TOUCH_EV_PRESS) continue;
            taps++;
            MOKA_LOGI(TAG, "唤醒确认：第 %d/%d 触", taps, MOKA_WAKE_TAP_COUNT);
            if (taps >= MOKA_WAKE_TAP_COUNT) return true;
        }
        vTaskDelay(pdMS_TO_TICKS(MOKA_WAKE_POLL_MS));
    }
    MOKA_LOGI(TAG, "唤醒确认超时（%d/%d 触），继续入睡", taps, MOKA_WAKE_TAP_COUNT);
    return false;
}

void power_check_idle(void)
{
    if (g_app.state == ST_SLEEP) return;
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - g_app.last_activity_ms < MOKA_IDLE_SLEEP_MS) return;

    app_state_t prev_state = g_app.state;
    MOKA_LOGI(TAG, "闲置 %dms，进入轻睡眠", (int)(now_ms - g_app.last_activity_ms));
    g_app.state = ST_SLEEP;

    /* 1. 合并 .srs（断电安全关键路径：tmp+rename 原子替换，FR-5） */
    if (g_app.cur_book >= 0 && g_app.recs) {
        const char *p = g_app.books[g_app.cur_book].srs_path;
        if (srs_store_compact(p, g_app.recs, g_app.rec_count) != 0) {
            MOKA_LOGE(TAG, "休眠前合并失败: %s", p);
        }
    }
    /* 2. 全刷清残影（NFR-2：休眠前必全刷） */
    disp_push_full();
    /* 3. SD 落盘并卸载（内部 delay 500ms，M5ReadPaper 断电教训） */
    sdcard_flush_and_sleep();
    /* 4. 断点快照（RTC_DATA_ATTR）：轻睡 RAM 保持用不上它，但睡中被
     * 电源键 EN 复位时，下次冷启动可凭它断点续读——成本为零，总是保存 */
    save_snapshot();

    /* 5. 轻睡眠循环：任何触摸都会唤醒芯片，但只在"1100ms 内 3 触"时
     * 才正式醒来，否则视为误触继续睡（防口袋/持握误触耗电） */
    for (;;) {
        touch_prepare_wakeup();   /* GPIO48 gpio_wakeup（轻睡可用，深睡不可用） */
        MOKA_LOGI(TAG, "轻睡眠中（%dms 内触 %d 次唤醒）",
                  MOKA_WAKE_WINDOW_MS, MOKA_WAKE_TAP_COUNT);
        esp_light_sleep_start();
        if (wait_triple_tap()) break;
    }

    /* 6. 确认唤醒：RAM/帧缓冲/屏幕画面全程保持，无需重绘；
     * 只需恢复状态、重挂 SD、重置闲置计时 */
    MOKA_LOGI(TAG, "三触确认，唤醒恢复");
    g_app.state = prev_state;
    power_on_activity();
    if (!sdcard_mount()) {
        MOKA_LOGE(TAG, "唤醒后 SD 重挂失败（下次落盘将失败，建议重启）");
    }
}

void power_shutdown(void)
{
    if (g_app.state == ST_SLEEP) return;
    g_app.state = ST_SLEEP;
    MOKA_LOGI(TAG, "电源键关机");

    /* 1. 合并 .srs（同休眠路径：tmp+rename 原子替换，FR-5） */
    if (g_app.cur_book >= 0 && g_app.recs) {
        const char *p = g_app.books[g_app.cur_book].srs_path;
        if (srs_store_compact(p, g_app.recs, g_app.rec_count) != 0) {
            MOKA_LOGE(TAG, "关机前合并失败: %s", p);
        }
    }
    /* 2. 渲染关机页（只写帧缓冲；断电后墨水屏常显此画面） */
    ui_shutdown_render();
    /* 3. 无色差全刷：先物理刷白再 GC16（diff 全刷会跳过白底造成色差，
     * 见 disp_push_full_clean 注释） */
    disp_push_full_clean();
    /* 4. SD 落盘并卸载（内部 delay 500ms，M5ReadPaper 断电教训） */
    sdcard_flush_and_sleep();
    /* 5. PMS150G 断电（不返回；脉冲失效时内部降级深睡眠，仍不返回） */
    powerctl_power_off();
}

bool power_try_resume(void)
{
    /* RTC 慢速内存在 EN 复位（电源键硬重启）后仍然保持，只在真正断电
     * （GPIO44 关机/拔电池）后才丢失——因此快照有效即可续读，
     * 不要求唤醒原因：轻睡三触唤醒不重启不会走到这里；
     * 睡中按电源键 = EN 复位重启，走到这里恢复断点卡片（FR-7a）。 */
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (!snapshot_valid()) {
        MOKA_LOGI(TAG, "冷启动（cause=%d，无有效快照），走开机菜单", (int)cause);
        return false;
    }
    MOKA_LOGI(TAG, "检测到断点快照（cause=%d），尝试续读", (int)cause);

    /* 按快照路径找回词书 */
    for (size_t i = 0; i < g_app.book_count; i++) {
        if (strcmp(g_app.books[i].path, s_snap.book_path) == 0) {
            if (wordbook_open_current((int)i) != 0) {
                MOKA_LOGE(TAG, "唤醒恢复打开词书失败: %s", s_snap.book_path);
                return false;
            }
            if (review_resume_card(s_snap.cur_word)) {
                MOKA_LOGI(TAG, "触摸唤醒，已恢复断点续读");
                return true;
            }
            MOKA_LOGE(TAG, "断点卡片恢复失败 id=%u，走开机菜单", (unsigned)s_snap.cur_word);
            return false;
        }
    }
    MOKA_LOGE(TAG, "快照词书已不存在: %s，走开机菜单", s_snap.book_path);
    return false;
}
