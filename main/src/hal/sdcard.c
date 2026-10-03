/**
 * sdcard.c —— SD 卡实现（签名见 sdcard.h，不可改）
 *
 * 挂载策略（两级降级）：
 *   1. 1-bit SDMMC：CLK=G39 CMD=G38 D0=G40（PaperS3 硬件只接了 D0，
 *      禁止 4-bit —— 会踩到未连接的 D1/D2/D3 而挂载失败）
 *   2. 降级 SPI：SCK=39 MOSI=38 MISO=40 CS=47 @ 8MHz
 *      （注意 SCK/MOSI/MISO 与 SDMMC 的 CLK/CMD/D0 是同三根线复用）
 *   3. 再失败 → 返回 false，上层按"无卡降级"处理
 *
 * 断电纪律：sdcard_flush_and_sleep() 里 delay 500ms 再卸载——
 * M5ReadPaper 实测 100ms 不够，卡内缓存没落盘会丢数据。
 */
#include "sdcard.h"

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"

#include "../log.h"

#define TAG "sdcard"

#define MOKA_SD_MOUNT_POINT     "/sdcard"

/* ---- 1-bit SDMMC 引脚（M5 官方 PinMap，已核实）---- */
#define MOKA_SD_SDMMC_CLK       GPIO_NUM_39
#define MOKA_SD_SDMMC_CMD       GPIO_NUM_38
#define MOKA_SD_SDMMC_D0        GPIO_NUM_40

/* ---- SPI 降级引脚（与 SDMMC 同线复用 + 独立 CS）---- */
#define MOKA_SD_SPI_SCK         GPIO_NUM_39
#define MOKA_SD_SPI_MOSI        GPIO_NUM_38
#define MOKA_SD_SPI_MISO        GPIO_NUM_40
#define MOKA_SD_SPI_CS          GPIO_NUM_47
#define MOKA_SD_SPI_FREQ_KHZ    8000      /* 8MHz，低速保兼容 */
#define MOKA_SD_SPI_HOST        SPI2_HOST

/* 卡内缓存落盘等待：500ms（M5ReadPaper 实测 100ms 会丢数据） */
#define MOKA_SD_FLUSH_WAIT_MS   500

static sdmmc_card_t *s_card;        /* 挂载成功后驱动返回的卡句柄 */
static bool          s_mounted;
static bool          s_using_spi;   /* 记录走哪条路径挂上的，卸载时要对应清理 */

static bool mount_sdmmc(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;      /* 硬件只接了 D0，禁止 4-bit */
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = MOKA_SD_SDMMC_CLK;
    slot.cmd   = MOKA_SD_SDMMC_CMD;
    slot.d0    = MOKA_SD_SDMMC_D0;
    slot.width = 1;                         /* 与 FLAG_1BIT 一致 */

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed  = false,   /* 永不自动格式化，防误清空词库 */
        .max_files               = 4,       /* 词库 + .srs + tmp + 日志 足够 */
        .allocation_unit_size    = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(MOKA_SD_MOUNT_POINT, &host,
                                            &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "SDMMC 1-bit 挂载失败: %s", esp_err_to_name(err));
        return false;
    }
    s_using_spi = false;
    MOKA_LOGI(TAG, "SD 挂载成功（SDMMC 1-bit）：%s", MOKA_SD_MOUNT_POINT);
    return true;
}

static bool mount_spi(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num     = MOKA_SD_SPI_MOSI,
        .miso_io_num     = MOKA_SD_SPI_MISO,
        .sclk_io_num     = MOKA_SD_SPI_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4092,    /* 一次 DMA 传输上限（内部 SRAM） */
    };
    esp_err_t err = spi_bus_initialize(MOKA_SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "SPI 总线初始化失败: %s", esp_err_to_name(err));
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = MOKA_SD_SPI_HOST;
    host.max_freq_khz = MOKA_SD_SPI_FREQ_KHZ;

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = MOKA_SD_SPI_CS;
    dev.host_id = MOKA_SD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed  = false,
        .max_files               = 4,
        .allocation_unit_size    = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(MOKA_SD_MOUNT_POINT, &host, &dev,
                                  &mount_cfg, &s_card);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "SPI 挂载失败: %s", esp_err_to_name(err));
        esp_err_t err2 = spi_bus_free(MOKA_SD_SPI_HOST);
        if (err2 != ESP_OK) {
            MOKA_LOGE(TAG, "SPI 总线释放失败: %s", esp_err_to_name(err2));
        }
        return false;
    }
    s_using_spi = true;
    MOKA_LOGI(TAG, "SD 挂载成功（SPI %dKHz 降级路径）：%s",
              MOKA_SD_SPI_FREQ_KHZ, MOKA_SD_MOUNT_POINT);
    return true;
}

bool sdcard_mount(void)
{
    if (s_mounted) {
        return true;    /* 幂等 */
    }
    if (!mount_sdmmc() && !mount_spi()) {
        MOKA_LOGE(TAG, "SD 两条路径均挂载失败，进入无卡降级");
        return false;
    }
    s_mounted = true;
    return true;
}

bool sdcard_is_mounted(void)
{
    return s_mounted;
}

void sdcard_flush_and_sleep(void)
{
    if (!s_mounted) {
        return;
    }

    /* 冲刷 stdio 层所有已开句柄的缓冲（fflush(NULL) = 冲刷全部流）。
     * 注意：上层对关键文件（.srs tmp+rename）应自己 fsync 到 FAT 层，
     * 这里只能保证 stdio 缓冲全部落到 VFS/FAT。 */
    fflush(NULL);

    /* 等卡内缓存落盘：100ms 不够（M5ReadPaper 实测丢数据），固定 500ms */
    vTaskDelay(pdMS_TO_TICKS(MOKA_SD_FLUSH_WAIT_MS));

    esp_err_t err = esp_vfs_fat_sdcard_unmount(MOKA_SD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "卸载失败: %s", esp_err_to_name(err));
        /* 失败也继续走 SPI 清理并标记未挂载——深睡前不能卡死在这里 */
    }

    if (s_using_spi) {
        err = spi_bus_free(MOKA_SD_SPI_HOST);
        if (err != ESP_OK) {
            MOKA_LOGE(TAG, "SPI 总线释放失败: %s", esp_err_to_name(err));
        }
        s_using_spi = false;
    }

    s_card = NULL;
    s_mounted = false;
    MOKA_LOGI(TAG, "SD 已落盘并卸载");
}
