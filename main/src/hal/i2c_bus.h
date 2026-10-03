/**
 * i2c_bus.h —— 共享 I2C 总线单例（内部模块，hal 外不可见）
 *
 * M5 Paper S3 上 GT911(触摸) / BM8563(RTC) / BMI270(IMU) 挂在同一条
 * I2C 总线：SDA=GPIO41, SCL=GPIO42, 400kHz（M5 官方 PinMap）。
 * touch.c 与 rtc.c 通过本模块共用一条总线句柄，避免各自建 bus 冲突。
 *
 * 设计约束：
 * - 幂等初始化（多次调用 i2c_bus_init 只建一次 bus）
 * - 线程安全：内部互斥锁，寄存器读写是"锁内完成的原子事务"
 *   （寄存器地址 + 数据在同一把锁里发完，不会被另一任务插队拆散）
 * - 锁粒度：一次事务一锁，调用方不能跨事务持锁（够用且不死锁）
 */
#ifndef MOKA_HAL_I2C_BUS_H
#define MOKA_HAL_I2C_BUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 总线参数（M5 官方 PinMap，已核实）---- */
#define MOKA_I2C_SDA_GPIO       41        /* SDA */
#define MOKA_I2C_SCL_GPIO       42        /* SCL */
#define MOKA_I2C_FREQ_HZ        400000    /* 400kHz，GT911/BM8563 都支持 */

/* ---- 挂载设备的 7 位地址 ---- */
#define MOKA_I2C_ADDR_GT911_A   0x5D      /* GT911 上电时 INT 为高 → 0x5D */
#define MOKA_I2C_ADDR_GT911_B   0x14      /* GT911 上电时 INT 为低  → 0x14 */
#define MOKA_I2C_ADDR_BM8563    0x51      /* RTC 固定地址 */
#define MOKA_I2C_ADDR_BMI270    0x68      /* IMU（本阶段不使用，仅记录） */

bool i2c_bus_init(void);   /* 幂等；失败打 MOKA_LOGE 返回 false */

/**
 * 探测某 7 位地址是否有设备应答（内部会短暂加锁）。
 * 返回 ESP_OK=在线；其他=不在线或总线错误。
 */
esp_err_t i2c_bus_probe(uint8_t addr);

/**
 * 写寄存器：reg(大端 reg_len 字节) + data。锁内完成整个事务。
 * GT911 用 16 位寄存器地址（reg_len=2），BM8563 用 8 位（reg_len=1）。
 */
esp_err_t i2c_bus_write_reg(uint8_t dev_addr, uint32_t reg, uint8_t reg_len,
                            const uint8_t *data, size_t data_len);

/* 读寄存器：先写 reg，再重复起始条件读 len 字节。锁内完成。 */
esp_err_t i2c_bus_read_reg(uint8_t dev_addr, uint32_t reg, uint8_t reg_len,
                           uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
#endif
