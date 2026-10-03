/**
 * display.h —— 墨水屏抽象（内部实现：epdiy，ED047TC1 直驱）
 * 帧缓冲 540×960×4bpp ≈ 259200B，强制 PSRAM（NFR-1）。
 * 帧缓冲字节约定：每字节 2 像素，偶数 x 占高半字节（font.c/main.cpp 共用此约定）。
 * 刷新纪律（NFR-2）由实现层集中管理：计数器，每 10 次局刷强制全刷，
 * 休眠前全刷。上层无权绕过 disp_push_* 直接调 epd。
 */
#ifndef MOKA_HAL_DISPLAY_H
#define MOKA_HAL_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DISP_WIDTH   540   /* 竖屏逻辑宽 */
#define DISP_HEIGHT  960   /* 竖屏逻辑高 */
#define DISP_BPP     4     /* 4 位/像素，16 灰阶 */

bool     disp_init(void);
uint8_t *disp_framebuffer(void);            /* 4bpp 帧缓冲指针（PSRAM） */
void     disp_clear(uint8_t gray);          /* 0x00 黑 … 0x0F 白 */
void     disp_push_partial(int x, int y, int w, int h);  /* 局刷（计数） */
void     disp_push_full(void);              /* 全刷（清残影；diff 驱动，同白区域不重复刷） */
void     disp_push_full_clean(void);        /* 无色差全刷：先物理刷白再 GC16（关机页用） */
void     disp_force_clear(void);            /* 强制物理刷白（绕过 diff，冷启动清残页用） */
void     disp_power_save(bool on);          /* EPD 高压电源开关 */

#ifdef __cplusplus
}
#endif
#endif
