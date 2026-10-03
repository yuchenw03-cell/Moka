/**
 * touch.h —— GT911 触摸抽象
 * 注意：GT911 的 INT 脚接 GPIO48。GPIO48 不在 ESP32-S3 的 RTC GPIO 域
 * （仅 GPIO0~21），因此 INT 无法作为深睡 ext1 唤醒源；触摸唤醒走
 * 轻睡眠 gpio_wakeup（任意数字 GPIO 可用，M5ReadPaper 同方案）。
 */
#ifndef MOKA_HAL_TOUCH_H
#define MOKA_HAL_TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOUCH_EV_NONE = 0,
    TOUCH_EV_PRESS,
    TOUCH_EV_RELEASE,
    TOUCH_EV_DOUBLE,      /* 200ms 内两次按下（M5ReadPaper 已验证的阈值） */
} touch_event_kind_t;

typedef struct {
    touch_event_kind_t kind;
    int16_t x, y;         /* 逻辑坐标（已按当前旋转方向换算） */
} touch_event_t;

bool touch_init(void);
bool touch_poll(touch_event_t *ev);   /* 10ms 轮询调用；无事件返回 false */
void touch_prepare_wakeup(void);      /* 进轻睡前：GPIO48 gpio_wakeup 低电平唤醒 */

#ifdef __cplusplus
}
#endif
#endif
