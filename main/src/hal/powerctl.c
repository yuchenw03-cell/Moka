/**
 * powerctl.c —— PMS150G 整机断电控制（PaperS3）
 *
 * 硬件路径（官方原理图 sch_papers3_V1.0.pdf）：GPIO44 接 PMS150G 的
 * PWROFF_PLUSE 网络，脉冲触发电源管理芯片切断整机供电。断电后墨水屏
 * 保持最后刷新画面（关机页"墨卡"），再按电源键由 PMS150G 重新上电开机
 * ——开机动作是纯硬件行为，固件无需干预。
 *
 * 脉冲极性本地无权威文档：net 名 PWROFF_PLUSE 暗示有效高脉冲，先试
 * 200ms 高脉冲；500ms 后仍存活（说明极性/宽度不对）再试低脉冲；
 * 两次都未断（不应发生）则降级为 ext0（GPIO0 低电平 = 电源键脉冲）
 * 唤醒的深睡眠，保住"再按电源键开机"语义。
 *
 * 注意 GPIO44 同时是 U0RXD 默认脚：本机日志走 USB-Serial/JTAG，
 * UART0 未启用，可安全占用。
 */
#include "powerctl.h"

#include "driver/gpio.h"
#include "esp_sleep.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "../log.h"

#define TAG "powerctl"

#define MOKA_PWROFF_GPIO        GPIO_NUM_44   /* PWROFF_PLUSE → PMS150G */
#define MOKA_PWROFF_PULSE_MS    200           /* 单次脉冲宽度 */
#define MOKA_PWROFF_WAIT_MS     500           /* 脉冲后等待断电生效的窗口 */

void powerctl_power_off(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << MOKA_PWROFF_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "GPIO%d 配置失败: %s", MOKA_PWROFF_GPIO, esp_err_to_name(err));
    } else {
        /* 尝试 1：高脉冲（net 名 PWROFF_PLUSE 暗示 active-high） */
        MOKA_LOGI(TAG, "触发断电：GPIO%d 高脉冲 %dms", MOKA_PWROFF_GPIO, MOKA_PWROFF_PULSE_MS);
        gpio_set_level(MOKA_PWROFF_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(MOKA_PWROFF_PULSE_MS));
        gpio_set_level(MOKA_PWROFF_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(MOKA_PWROFF_WAIT_MS));  /* 断电成功则不会走到这里 */

        /* 尝试 2：低脉冲（板改版极性兜底） */
        MOKA_LOGE(TAG, "高脉冲未断电，尝试低脉冲 %dms", MOKA_PWROFF_PULSE_MS);
        gpio_set_level(MOKA_PWROFF_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(MOKA_PWROFF_WAIT_MS / 10));   /* 先拉高制造下降沿 */
        gpio_set_level(MOKA_PWROFF_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(MOKA_PWROFF_PULSE_MS));
        gpio_set_level(MOKA_PWROFF_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(MOKA_PWROFF_WAIT_MS));
    }

    /* 两次脉冲都未断电：降级为深睡眠，电源键（GPIO0 低脉冲）ext0 唤醒。
     * GPIO0 是 RTC GPIO 且有板载 10K 上拉（button.h 注释），ext0 低电平
     * 唤醒可用；GPIO44 先释放为输入，避免深睡期间倒灌 PMS150G。 */
    MOKA_LOGE(TAG, "断电脉冲未生效，降级深睡眠（电源键 ext0 唤醒）");
    gpio_set_direction(MOKA_PWROFF_GPIO, GPIO_MODE_INPUT);
    err = esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "ext0 唤醒配置失败: %s（深睡后仅 PMS150G 硬复位可唤醒）",
                  esp_err_to_name(err));
    }
    esp_deep_sleep_start();
}
