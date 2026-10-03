/**
 * sdcard.h —— SD 卡抽象（1-bit SDMMC：CLK=GPIO39 CMD=GPIO38 D0=GPIO40，
 * PaperS3 硬件只接了 D0，禁止尝试 4-bit；挂载失败可回退 SPI 8MHz）
 * 断电纪律：sdcard_flush_and_sleep() 内部 delay 500ms 等卡内缓存落盘
 * （M5ReadPaper 实测：100ms 会丢数据）。
 */
#ifndef MOKA_HAL_SDCARD_H
#define MOKA_HAL_SDCARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool sdcard_mount(void);          /* 挂载到 /sdcard */
bool sdcard_is_mounted(void);
void sdcard_flush_and_sleep(void);/* fsync 全部句柄 + delay(500) + 卸载 */

#ifdef __cplusplus
}
#endif
#endif
