/**
 * button.c —— 电源键按键检测实现（签名见 button.h，不可改）
 *
 * 消抖策略：低电平持续 MOKA_BUTTON_DEBOUNCE_MS 判定为一次有效按下，
 * 松开（回高）后才允许下一次触发，防止长按连发。
 * 为什么用轮询而不是中断：FSM 主循环本来就以 10ms 周期轮询输入，
 * 按键走同一路径串行消费，无需 ISR 与队列，逻辑最简单。
 */
#include "button.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_timer.h"

#include "../log.h"

#define TAG "button"

/* 电源键信号脚：PMS150G PA5 → GPIO0（与 SYS_LED 共网，10K 上拉，低有效） */
#define MOKA_BUTTON_GPIO        GPIO_NUM_0
/* 消抖确认窗口：PMS150G 按键脉冲宽度本地无文档，50ms 曾可能漏掉短脉冲，
 * 收紧到 20ms（比毛刺长、比真实脉冲短）；真实脉宽靠边沿日志实测。 */
#define MOKA_BUTTON_DEBOUNCE_MS 20

static bool s_ready;
static bool s_latched;           /* 已检出按下、等待松开 */
static int  s_last_level = 1;    /* 上次电平（边沿诊断用） */

/* ISR 边沿捕获：10ms 轮询可能漏掉更窄的 PMS150G 脉冲（真机实测短按时
 * 轮询完全没观察到 GPIO0 动作），中断级记录每次跳变作为地面真值；
 * 日志在 button_pressed 轮询里补打（ISR 里不打印）。 */
static volatile uint32_t s_isr_edges;
static volatile int      s_isr_last_level = 1;
static uint32_t s_isr_seen;

static void IRAM_ATTR button_gpio_isr(void *arg)
{
    (void)arg;
    s_isr_edges++;
    s_isr_last_level = gpio_get_level(MOKA_BUTTON_GPIO);   /* IRAM 安全 */
}

bool button_init(void)
{
    if (s_ready) return true;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << MOKA_BUTTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,    /* 内部上拉 + 板载 R9 双保险 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,     /* 双边沿捕获（诊断 PMS150G 脉冲用） */
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "GPIO%d 配置失败: %s", MOKA_BUTTON_GPIO, esp_err_to_name(err));
        return false;
    }
    /* ISR 服务可能已被其他模块安装：ESP_ERR_INVALID_STATE 视为已就绪 */
    err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        MOKA_LOGE(TAG, "ISR 服务安装失败: %s", esp_err_to_name(err));
    }
    err = gpio_isr_handler_add(MOKA_BUTTON_GPIO, button_gpio_isr, NULL);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "ISR 挂载失败: %s（边沿诊断不可用，轮询仍工作）",
                  esp_err_to_name(err));
    }
    s_ready = true;
    s_last_level = gpio_get_level(MOKA_BUTTON_GPIO);
    MOKA_LOGI(TAG, "按键轮询就绪（GPIO%d，低有效，当前电平=%d）",
              MOKA_BUTTON_GPIO, s_last_level);
    return true;
}

bool button_pressed(void)
{
    if (!s_ready) return false;

    /* 补打 ISR 捕获到的边沿（含轮询漏掉的窄脉冲），按键行为的最终证据 */
    if (s_isr_edges != s_isr_seen) {
        s_isr_seen = s_isr_edges;
        MOKA_LOGI(TAG, "ISR 捕获 GPIO%d 边沿 #%lu，电平=%d",
                  MOKA_BUTTON_GPIO, (unsigned long)s_isr_edges, s_isr_last_level);
    }

    int level = gpio_get_level(MOKA_BUTTON_GPIO);

    /* 边沿诊断：PMS150G 是否真的给 GPIO0 发按键脉冲、脉冲多宽，
     * 本地无文档，只能靠这份实测日志确认（决定物理键关机是否可行） */
    if (level != s_last_level) {
        MOKA_LOGI(TAG, "GPIO%d 电平跳变 %d->%d", MOKA_BUTTON_GPIO, s_last_level, level);
        s_last_level = level;
    }

    if (level == 0) {
        if (!s_latched) {
            /* 原地等确认窗口后复采（调用周期 10ms，阻塞 20ms 可接受，
             * 按下后走 power_shutdown 关机流程，不再返回交互路径） */
            vTaskDelay(pdMS_TO_TICKS(MOKA_BUTTON_DEBOUNCE_MS));
            if (gpio_get_level(MOKA_BUTTON_GPIO) == 0) {
                s_latched = true;
                MOKA_LOGI(TAG, "检测到按键按下");
                return true;
            }
        }
    } else {
        s_latched = false;
    }
    return false;
}
