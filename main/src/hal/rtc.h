/**
 * rtc.h —— BM8563 RTC 抽象（FR-7c：唯一时钟基准，离线走时）
 * BM8563 同时承担"真关机"：rtc_power_off() 写 alarm 让 RTC 切断电源
 * （PaperS3 的 powerOff 即 RTC 断电，这是硬件事实）。
 */
#ifndef MOKA_HAL_RTC_H
#define MOKA_HAL_RTC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 命名说明：不能用 rtc_init——ESP-IDF 的 esp_hw_support 已定义同名全局符号
 * （port/esp32s3/rtc_init.c），同名会链接冲突（multiple definition） */
bool     rtc_clock_init(void);
uint32_t rtc_get_unix(void);            /* 当前 unix 秒 */
bool     rtc_set_unix(uint32_t ts);     /* 首次手动设时写入 */
void     rtc_power_off(void);           /* RTC 断电（物理关机） */

#ifdef __cplusplus
}
#endif
#endif
