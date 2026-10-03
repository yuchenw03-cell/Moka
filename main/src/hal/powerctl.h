/**
 * powerctl.h —— PMS150G 整机断电控制（PaperS3）
 */
#ifndef MOKA_HAL_POWERCTL_H
#define MOKA_HAL_POWERCTL_H

#ifdef __cplusplus
extern "C" {
#endif

/* 触发 PMS150G 切断整机电源（不返回）。
 * 若断电脉冲未生效（板改版极性差异等），内部降级为
 * "电源键 ext0 唤醒的深睡眠"，保证再按电源键可开机的语义不丢。 */
void powerctl_power_off(void);

#ifdef __cplusplus
}
#endif
#endif
