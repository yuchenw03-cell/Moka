/**
 * buzzer.c —— 无源蜂鸣器实现（LEDC PWM，签名见 buzzer.h，不可改）
 *
 * 蜂鸣器引脚（官方原理图 sch_papers3_V1.0.pdf 确认）：
 * BUZ_PWM = GPIO21（模组左排 27 脚），PaperS3 确实有蜂鸣器。
 * 注意：GPIO21 属 RTC GPIO 域，若与触摸/其它外设冲突需再调整。
 */
#include "buzzer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/ledc.h"

#include "../log.h"

#define TAG "buzzer"

/* BUZ_PWM = GPIO21（官方原理图 sch_papers3_V1.0.pdf） */
#define MOKA_BUZZER_GPIO        GPIO_NUM_21

/* LEDC 资源分配 */
#define MOKA_BUZZER_LEDC_TIMER  LEDC_TIMER_0
#define MOKA_BUZZER_LEDC_CH     LEDC_CHANNEL_0
#define MOKA_BUZZER_LEDC_MODE   LEDC_LOW_SPEED_MODE
#define MOKA_BUZZER_DUTY_RES    LEDC_TIMER_10_BIT
#define MOKA_BUZZER_DUTY_HALF   512         /* 10-bit 50% 占空比 */
#define MOKA_BUZZER_IDLE_FREQ   2000        /* 定时器初始频率（beep 时会改） */

static bool s_ready;    /* GPIO 有效且 LEDC 已配置 */

bool buzzer_init(void)
{
    if (MOKA_BUZZER_GPIO == GPIO_NUM_NC) {
        MOKA_LOGI(TAG, "蜂鸣器 GPIO 未定义（NC），跳过初始化");
        return false;
    }
    if (s_ready) {
        return true;    /* 幂等 */
    }

    ledc_timer_config_t timer = {
        .speed_mode      = MOKA_BUZZER_LEDC_MODE,
        .duty_resolution = MOKA_BUZZER_DUTY_RES,
        .timer_num       = MOKA_BUZZER_LEDC_TIMER,
        .freq_hz         = MOKA_BUZZER_IDLE_FREQ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "LEDC 定时器配置失败: %s", esp_err_to_name(err));
        return false;
    }

    ledc_channel_config_t ch = {
        .gpio_num   = MOKA_BUZZER_GPIO,
        .speed_mode = MOKA_BUZZER_LEDC_MODE,
        .channel    = MOKA_BUZZER_LEDC_CH,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = MOKA_BUZZER_LEDC_TIMER,
        .duty       = 0,            /* 初始静默 */
        .hpoint     = 0,
    };
    err = ledc_channel_config(&ch);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "LEDC 通道配置失败: %s", esp_err_to_name(err));
        return false;
    }

    s_ready = true;
    MOKA_LOGI(TAG, "蜂鸣器就绪 GPIO%d", MOKA_BUZZER_GPIO);
    return true;
}

void buzzer_beep(uint16_t freq_hz, uint16_t duration_ms)
{
    if (!s_ready) {
        return;     /* 引脚未定/未初始化：静默跳过（init 时已打过日志） */
    }
    if (freq_hz == 0 || duration_ms == 0) {
        return;
    }

    esp_err_t err = ledc_set_freq(MOKA_BUZZER_LEDC_MODE,
                                  MOKA_BUZZER_LEDC_TIMER, freq_hz);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "设置频率 %uHz 失败: %s", freq_hz, esp_err_to_name(err));
        return;
    }
    err = ledc_set_duty(MOKA_BUZZER_LEDC_MODE, MOKA_BUZZER_LEDC_CH,
                        MOKA_BUZZER_DUTY_HALF);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "设置占空比失败: %s", esp_err_to_name(err));
        return;
    }
    err = ledc_update_duty(MOKA_BUZZER_LEDC_MODE, MOKA_BUZZER_LEDC_CH);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "使能输出失败: %s", esp_err_to_name(err));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(duration_ms));     /* 阻塞鸣叫时长 */

    err = ledc_set_duty(MOKA_BUZZER_LEDC_MODE, MOKA_BUZZER_LEDC_CH, 0);
    if (err == ESP_OK) {
        err = ledc_update_duty(MOKA_BUZZER_LEDC_MODE, MOKA_BUZZER_LEDC_CH);
    }
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "停止输出失败: %s", esp_err_to_name(err));
    }
}
