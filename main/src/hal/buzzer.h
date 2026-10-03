/**
 * buzzer.h —— 无源蜂鸣器
 * 已核实 PaperS3 无蜂鸣器硬件（依据见 buzzer.c 文件头），
 * 实现内固定 MOKA_BUZZER_GPIO=GPIO_NUM_NC 优雅禁用；
 * 若未来换用带蜂鸣器的板型，只在 buzzer.c 改这一个宏。
 */
#ifndef MOKA_HAL_BUZZER_H
#define MOKA_HAL_BUZZER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool buzzer_init(void);
void buzzer_beep(uint16_t freq_hz, uint16_t duration_ms);

#ifdef __cplusplus
}
#endif
#endif
