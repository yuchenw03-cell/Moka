/**
 * i2c_bus.c —— 共享 I2C 总线单例实现（见 i2c_bus.h）
 *
 * 实现要点：
 * - 单 bus 句柄 + 互斥锁；设备句柄（i2c_master_dev_handle_t）按需惰性创建并缓存
 * - 每次寄存器读写 = 锁内一次完整 i2c_master_transmit / transmit_receive，
 *   中途不释放锁，保证寄存器地址与数据不被其他任务插队
 */
#include "i2c_bus.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "../log.h"

#define TAG "i2c_bus"

/* 单次事务超时：400kHz 下几十字节远用不到 100ms，留足余量即可 */
#define MOKA_I2C_XFER_TIMEOUT_MS  100
/* 设备探测超时：总线上无人应答时 NACK 很快返回，50ms 足够 */
#define MOKA_I2C_PROBE_TIMEOUT_MS 50

static i2c_master_bus_handle_t s_bus;        /* NULL 表示未初始化 */
static SemaphoreHandle_t       s_lock;       /* 保护 bus 与设备缓存 */
static bool                    s_inited;

/* 设备句柄缓存：GT911 两地址 + BM8563 + BMI270，共 4 个槽位足够。
 * 槽位大小写死，避免动态分配。 */
#define MOKA_I2C_DEV_CACHE_MAX 4

typedef struct {
    uint8_t                  addr;    /* 0 = 空槽 */
    i2c_master_dev_handle_t  dev;
} dev_slot_t;

static dev_slot_t s_devs[MOKA_I2C_DEV_CACHE_MAX];

/* 在缓存里找/建设备句柄。调用前必须已持锁。 */
static esp_err_t dev_get_locked(uint8_t addr, i2c_master_dev_handle_t *out)
{
    for (int i = 0; i < MOKA_I2C_DEV_CACHE_MAX; i++) {
        if (s_devs[i].addr == addr) {
            *out = s_devs[i].dev;
            return ESP_OK;
        }
    }
    for (int i = 0; i < MOKA_I2C_DEV_CACHE_MAX; i++) {
        if (s_devs[i].addr == 0) {
            i2c_device_config_t cfg = {
                .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                .device_address  = addr,
                .scl_speed_hz    = MOKA_I2C_FREQ_HZ,
            };
            esp_err_t err = i2c_master_bus_add_device(s_bus, &cfg, &s_devs[i].dev);
            if (err != ESP_OK) {
                MOKA_LOGE(TAG, "add device 0x%02X 失败: %s", addr, esp_err_to_name(err));
                return err;
            }
            s_devs[i].addr = addr;
            *out = s_devs[i].dev;
            return ESP_OK;
        }
    }
    MOKA_LOGE(TAG, "设备句柄缓存已满（%d 槽），无法挂载 0x%02X",
              MOKA_I2C_DEV_CACHE_MAX, addr);
    return ESP_ERR_NO_MEM;
}

bool i2c_bus_init(void)
{
    if (s_inited) {
        return true;    /* 幂等 */
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            MOKA_LOGE(TAG, "创建互斥锁失败");
            return false;
        }
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port          = -1,    /* -1 = 由驱动自动选 I2C 控制器号 */
        .sda_io_num        = MOKA_I2C_SDA_GPIO,
        .scl_io_num        = MOKA_I2C_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,     /* 默认毛刺滤波 */
        .flags.enable_internal_pullup = true,   /* PaperS3 板载上拉，内部上拉兜底 */
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "i2c_new_master_bus 失败: %s", esp_err_to_name(err));
        return false;
    }

    s_inited = true;
    MOKA_LOGI(TAG, "I2C 总线就绪 SDA=%d SCL=%d @%dHz",
              MOKA_I2C_SDA_GPIO, MOKA_I2C_SCL_GPIO, MOKA_I2C_FREQ_HZ);
    return true;
}

esp_err_t i2c_bus_probe(uint8_t addr)
{
    if (!s_inited) {
        MOKA_LOGE(TAG, "总线未初始化（probe 0x%02X）", addr);
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = i2c_master_probe(s_bus, addr, MOKA_I2C_PROBE_TIMEOUT_MS);
    xSemaphoreGive(s_lock);
    return err;
}

/* 把 reg_len 字节的大端寄存器地址写入 buf，返回写入字节数。 */
static size_t reg_to_bytes(uint32_t reg, uint8_t reg_len, uint8_t *buf)
{
    for (uint8_t i = 0; i < reg_len; i++) {
        buf[i] = (uint8_t)(reg >> (8 * (reg_len - 1 - i)));
    }
    return reg_len;
}

esp_err_t i2c_bus_write_reg(uint8_t dev_addr, uint32_t reg, uint8_t reg_len,
                            const uint8_t *data, size_t data_len)
{
    if (!s_inited) {
        MOKA_LOGE(TAG, "总线未初始化（write 0x%02X）", dev_addr);
        return ESP_ERR_INVALID_STATE;
    }
    if (reg_len == 0 || reg_len > sizeof(uint32_t)) {
        MOKA_LOGE(TAG, "非法 reg_len=%u", reg_len);
        return ESP_ERR_INVALID_ARG;
    }

    /* 寄存器地址(≤4B) + 数据 拼成一帧一次发完；栈上缓冲远小于 4KB 红线 */
    uint8_t buf[sizeof(uint32_t) + 8];
    if (data_len > 8) {
        /* 本模块的调用方（touch/rtc）单次写都 ≤8B，超限视为用法错误 */
        MOKA_LOGE(TAG, "单次写 %uB 超出内部缓冲", (unsigned)data_len);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t head = reg_to_bytes(reg, reg_len, buf);
    if (data_len > 0 && data != NULL) {
        memcpy(buf + head, data, data_len);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = dev_get_locked(dev_addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit(dev, buf, head + data_len, MOKA_I2C_XFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            MOKA_LOGE(TAG, "写 0x%02X reg 0x%02lX 失败: %s",
                      dev_addr, (unsigned long)reg, esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t i2c_bus_read_reg(uint8_t dev_addr, uint32_t reg, uint8_t reg_len,
                           uint8_t *data, size_t len)
{
    if (!s_inited) {
        MOKA_LOGE(TAG, "总线未初始化（read 0x%02X）", dev_addr);
        return ESP_ERR_INVALID_STATE;
    }
    if (reg_len == 0 || reg_len > sizeof(uint32_t) || data == NULL || len == 0) {
        MOKA_LOGE(TAG, "read 参数非法 reg_len=%u len=%u", reg_len, (unsigned)len);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t reg_buf[sizeof(uint32_t)];
    size_t head = reg_to_bytes(reg, reg_len, reg_buf);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = dev_get_locked(dev_addr, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(dev, reg_buf, head, data, len,
                                          MOKA_I2C_XFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            MOKA_LOGE(TAG, "读 0x%02X reg 0x%02lX 失败: %s",
                      dev_addr, (unsigned long)reg, esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}
