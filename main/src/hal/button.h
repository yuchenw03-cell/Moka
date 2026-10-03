/**
 * button.h —— 电源键按键检测（PaperS3）
 *
 * 硬件路径（官方原理图 sch_papers3_V1.0.pdf）：
 * 电源键 SW_PWR 只进 PMS150G 电源管理芯片，PMS150G 的 PA5 脚把按键事件
 * 转成脉冲送到 ESP32 的 GPIO0（与 SYS_LED 共网，板载 R9 10K 上拉）。
 * 因此固件侧"按键"= 轮询 GPIO0 低脉冲。注意 GPIO0 是 strapping 脚，
 * 只做输入轮询，绝不配置为输出。
 */
#ifndef MOKA_HAL_BUTTON_H
#define MOKA_HAL_BUTTON_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool button_init(void);          /* 配置 GPIO0 输入（幂等） */
bool button_pressed(void);       /* 非阻塞轮询：检出一次"按下"（带消抖）返回 true */

#ifdef __cplusplus
}
#endif
#endif
