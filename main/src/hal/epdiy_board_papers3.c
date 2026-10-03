/**
 * epdiy_board_papers3.c —— M5 Paper S3 板级实现（epdiy v2.1.3）
 *
 * v2 板级模型（依据 components/epdiy/src/epd_board.h:30-87）：
 * EpdBoardDefinition 只含函数指针，**没有任何引脚字段**——
 * v1 时代的 data/clk/le/sth/stv/ckv/cfg_* 字段表已不存在。
 * S3 上引脚通过 init() 里的 epd_lcd_init() 交给 LCD_CAM 外设
 * （依据 output_lcd/lcd_driver.h:13-39 的 lcd_bus_config_t / LcdEpdConfig_t）。
 *
 * 引脚（官方原理图 sch_papers3_V1.0.pdf 逐脚核对，权威）：
 *   DB0..DB7 = G6 G14 G7 G12 G9 G11 G8 G10
 *   XSTL(STH)=G13  XLE(LEH)=G15  XCL(CKH)=G16(经R14)  SPV(STV)=G17  CKV=G18
 *   EPD_PWR=G45（面板逻辑 VDD + XOE + MODE1 同一网络）
 *   BST_EN=G46（MT9700 负载开关 → MT3608 升压 → VPOS/VNEG，
 *               再经 D8/D9 与 D11-D13 电荷泵得 VGH/VGL；不拉高则无高压，
 *               波形照跑但屏幕纹丝不动——首刷"假死"的根因）
 *
 * 控制语义：PaperS3 无 CFG 移位寄存器，也没有独立 OE/MODE 线——
 * LE/STH/STV 由 LCD 外设时序直接驱动、CKV 由 RMT 驱动、CKH 即像素时钟
 * （依据 output_lcd/lcd_driver.c init_bus_gpio() / epd_lcd_start_frame()）。
 * 因此 set_ctrl 必须存在但为空操作：epdiy.c 的 epd_control_reg_init()/
 * epd_set_mode() 会无条件调用它，留 NULL 必崩。
 */
#include "epdiy_board_papers3.h"
#include "../log.h"

#include "epdiy.h"                  /* epd_get_display() */
#include "output_lcd/lcd_driver.h"  /* epd_lcd_init(), LcdEpdConfig_t */

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "moka.board"

/* ---- PaperS3 EPD 引脚（依据官方原理图 sch_papers3_V1.0.pdf 逐脚核对）---- */
#define PAPERS3_PIN_EPD_PWR   GPIO_NUM_45   /* EPD_PWR：面板逻辑 VDD + XOE + MODE1 使能，高有效 */
#define PAPERS3_PIN_BST_EN    GPIO_NUM_46   /* BST_EN：MT9700→MT3608 升压使能（产 VPOS/VNEG/VGH/VGL），高有效 */
#define PAPERS3_PIN_EPD_CKH   GPIO_NUM_16   /* EPD_XCL 像素时钟（经 R14 100R，原理图左排 22 脚） */
#define PAPERS3_PIN_EPD_LEH   GPIO_NUM_15   /* EPD_XLE：行锁存 */
#define PAPERS3_PIN_EPD_STH   GPIO_NUM_13   /* EPD_XSTL：行移位寄存器起始脉冲 */
#define PAPERS3_PIN_EPD_STV   GPIO_NUM_17   /* EPD_SPV：帧起始脉冲（驱动器内由 GPIO 直控） */
#define PAPERS3_PIN_EPD_CKV   GPIO_NUM_18   /* EPD_CKV：行移位时钟（驱动器内由 RMT 输出） */

/* DB0..DB7（8bit 总线，ED047TC1 bus_width=8，见 displays.c:48-55） */
#define PAPERS3_EPD_DATA_PINS { \
    GPIO_NUM_6, GPIO_NUM_14, GPIO_NUM_7, GPIO_NUM_12, \
    GPIO_NUM_9, GPIO_NUM_11, GPIO_NUM_8, GPIO_NUM_10, \
    GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, \
    GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC }

/* EPD 逻辑电源上电稳定等待（VDD/XOE 同时上电） */
#define PAPERS3_EPD_PWR_STABLE_MS 10
/* 升压电路上电稳定等待：MT3608 升压 + VGH/VGL 电荷泵建立需要时间，
 * 太短会导致首帧波形电压不足、刷屏无效 */
#define PAPERS3_BST_STABLE_MS 20

/* LCD 时序参数（单位 1/10us；与 epdiy 自带 lilygo_board_s3/v7 板一致，
 * 见 board/lilygo_board_s3.c:147-154，ED047TC1 同量级屏通用，真机可调） */
#define PAPERS3_CKV_HIGH_TIME    60
#define PAPERS3_LINE_FRONT_PORCH 4
#define PAPERS3_LE_HIGH_TIME     4

static void papers3_init(uint32_t epd_row_width, const EpdInitConfig *init_config)
{
    (void)epd_row_width;   /* LCD 路径用 display->width，不需要行宽参数 */
    (void)init_config;     /* PaperS3 无板载 I2C 电源管理芯片，配置为空 */

    /* 与上游板一致：解除深睡后可能残留的 CKH 保持（v7 板同款处理） */
    gpio_hold_dis(PAPERS3_PIN_EPD_CKH);

    /* EPD 两组电源脚：默认断电，刷屏流程按需上电。
     * 上电时序（poweron）：先 EPD_PWR（逻辑 VDD）后 BST_EN（高压），
     * 掉电相反——避免逻辑未就绪时高压已加面板的冲击。 */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PAPERS3_PIN_EPD_PWR) | (1ULL << PAPERS3_PIN_BST_EN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "EPD 电源 GPIO45/46 配置失败: %s", esp_err_to_name(err));
    }
    gpio_set_level(PAPERS3_PIN_BST_EN, 0);
    gpio_set_level(PAPERS3_PIN_EPD_PWR, 0);

    /* 把引脚/时序交给 LCD_CAM 驱动（总线宽度与像素时钟取显示屏参数） */
    const EpdDisplay_t *disp = epd_get_display();
    LcdEpdConfig_t lcd_cfg = {
        .pixel_clock      = (size_t)disp->bus_speed * 1000 * 1000,
        .ckv_high_time    = PAPERS3_CKV_HIGH_TIME,
        .line_front_porch = PAPERS3_LINE_FRONT_PORCH,
        .le_high_time     = PAPERS3_LE_HIGH_TIME,
        .bus_width        = disp->bus_width,
        .bus = {
            .data        = PAPERS3_EPD_DATA_PINS,
            .clock       = PAPERS3_PIN_EPD_CKH,
            .ckv         = PAPERS3_PIN_EPD_CKV,
            .start_pulse = PAPERS3_PIN_EPD_STH,
            .leh         = PAPERS3_PIN_EPD_LEH,
            .stv         = PAPERS3_PIN_EPD_STV,
        },
    };
    epd_lcd_init(&lcd_cfg, disp->width, disp->height);
    MOKA_LOGI(TAG, "PaperS3 板级初始化完成：LCD_CAM %dbit @%dMHz",
              disp->bus_width, disp->bus_speed);
}

static void papers3_deinit(void)
{
    epd_lcd_deinit();
    gpio_set_level(PAPERS3_PIN_BST_EN, 0);
    gpio_set_level(PAPERS3_PIN_EPD_PWR, 0);
    gpio_reset_pin(PAPERS3_PIN_EPD_PWR);
    gpio_reset_pin(PAPERS3_PIN_BST_EN);
}

/* PaperS3 无配置移位寄存器：epdiy 的控制位（OE/MODE/STV/STH）在本板上
 * 全部由 LCD/RMT 外设时序承载，无对应 GPIO 可拉。空操作即可，但必须存在
 * ——epd_board.c 的 epd_set_mode()/epd_control_reg_init() 不做 NULL 检查。 */
static void papers3_set_ctrl(epd_ctrl_state_t *state, const epd_ctrl_state_t *const mask)
{
    (void)state;
    (void)mask;
}

/* v2 的 poweron 返回 void（依据 epd_board.h:46），参数为控制状态指针 */
static void papers3_poweron(epd_ctrl_state_t *state)
{
    (void)state;
    /* 先逻辑电（VDD/XOE/MODE1），后高压（BST_EN→MT3608→VPOS/VNEG→VGH/VGL） */
    gpio_set_level(PAPERS3_PIN_EPD_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(PAPERS3_EPD_PWR_STABLE_MS));
    gpio_set_level(PAPERS3_PIN_BST_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(PAPERS3_BST_STABLE_MS));
}

static void papers3_poweroff(epd_ctrl_state_t *state)
{
    (void)state;
    /* 先断高压，后断逻辑电 */
    gpio_set_level(PAPERS3_PIN_BST_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(PAPERS3_PIN_EPD_PWR, 0);
}

/* 字段表依据 epd_board.h:30-87 的 EpdBoardDefinition 填写：
 * PaperS3 无 VCOM 移位寄存器/TPS65185/温度传感器/GPIO 扩展口，
 * measure_vcom / set_vcom / get_temperature / gpio_* 一律置 NULL
 * （epdiy.c 的 epd_set_vcom/epd_ambient_temperature 均有 NULL 保护；
 *  measure_vcom 全库无外部调用方，仅 v6/v7 板实现）。 */
const EpdBoardDefinition epd_board_papers3 = {
    .init             = papers3_init,
    .deinit           = papers3_deinit,
    .set_ctrl         = papers3_set_ctrl,
    .poweron          = papers3_poweron,
    .measure_vcom     = NULL,
    .poweroff         = papers3_poweroff,
    .set_vcom         = NULL,
    .get_temperature  = NULL,
    .gpio_set_direction = NULL,
    .gpio_read        = NULL,
    .gpio_write       = NULL,
};
