/**
 * touch.c —— GT911 触摸实现（签名见 touch.h，不可改）
 *
 * 硬件事实（官方原理图 sch_papers3_V1.0.pdf + GT911 datasheet）：
 * - TP_INT = GPIO48（模组右排 pad36，丝印 GPIO48/SPICLK_N；本机 Flash
 *   是 Quad NOR 走 GPIO32-35，Octal PSRAM 占用 GPIO33-37，SPICLK_P/N
 *   复用脚在 R8 模组上空闲，故 47/48 可用作 CARD_CS/TP_INT——
 *   M5ReadPaper 的 TOUCH_INT_PIN 48 与此一致）。
 *   GPIO48 不在 ESP32-S3 的 RTC GPIO 域（0~21），深睡 ext1 触摸唤醒
 *   物理不可用；浅睡 gpio_wakeup 可用任意数字 GPIO（备选方案）。
 * - I2C 地址由"上电瞬间 INT 电平"决定（GT911 datasheet）：INT 高 → 0x5D，
 *   INT 低 → 0x14。PaperS3 实际落到哪个地址无本地权威资料，固件两个都探测，
 *   探测日志会直接打出实际地址（真机首跑确认即可）
 * - 坐标寄存器：0x814E = 状态（bit7=数据就绪，bit[3:0]=触点数），
 *   0x8150 起为触点 1（XL,XH,YL,YH,SizeL,SizeH）；读完后必须写 0 清 0x814E，
 *   否则 GT911 不再更新坐标缓冲。
 * - 原生坐标系：真机实测（2026-09-29）为竖屏 540×960，原点=物理左上角，
 *   与屏幕逻辑坐标同向，恒等映射。方向换算由 MOKA_TOUCH_SWAP_XY /
 *   MOKA_TOUCH_INV_X / MOKA_TOUCH_INV_Y 三个宏集中控制，换板时只许改这三个宏。
 * - 唤醒：GPIO48 非 RTC GPIO，深睡 ext1 不可用；轻睡 gpio_wakeup 可用
 *   任意数字 GPIO，触摸唤醒走轻睡（touch_prepare_wakeup，M5ReadPaper 同款）。
 */
#include "touch.h"
#include "i2c_bus.h"

#include "../log.h"

#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_timer.h"

#define TAG "touch"

/* ---- 引脚与寄存器 ---- */
#define MOKA_TOUCH_INT_GPIO     GPIO_NUM_48   /* TP_INT（官方原理图 pad36 = GPIO48/SPICLK_N） */
#define GT911_REG_PRODUCT_ID    0x8140    /* 4 字节 ASCII，应为 "911" */
#define GT911_REG_CONFIG        0x8047    /* 配置区：版本(1B) + X分辨率(2B LE) + Y分辨率(2B LE) */
#define GT911_CONFIG_BYTES      5         /* 0x8047~0x804B */
#define GT911_REG_STATUS        0x814E    /* 状态：bit7=就绪, bit[3:0]=触点数 */
#define GT911_REG_POINT1        0x8150    /* 触点 1：XL XH YL YH SzL SzH */
#define GT911_STATUS_READY      0x80
#define GT911_STATUS_POINTS     0x0F
#define GT911_POINT_BYTES       6         /* 单触点 6 字节 */
#define GT911_REG_LEN           2         /* GT911 寄存器地址 16 位 */

/* ---- 坐标系 ----
 * 真机实测（2026-09-29，四角点击）：GT911 原生输出即竖屏 540×960，
 * 原点 = 物理左上角，X 左→右、Y 上→下，与屏幕逻辑坐标系完全同向。
 * （此前按横屏 960×540 假设并交换 XY，导致坐标被钳位到边缘、
 *   顶部按钮数学上永远点不到，表现为"全屏无反应"。） */
#define MOKA_TOUCH_NATIVE_W     540       /* GT911 原生竖屏宽 */
#define MOKA_TOUCH_NATIVE_H     960       /* 原生竖屏高 */
#define MOKA_TOUCH_LOGIC_W      540       /* 逻辑竖屏宽 */
#define MOKA_TOUCH_LOGIC_H      960       /* 逻辑竖屏高 */

/* 方向换算开关（唯一合法修改点，真机核实后定稿）：
 * 实测结论 = 恒等映射，三个开关全 0。 */
#define MOKA_TOUCH_SWAP_XY      0         /* 1 = 横转竖交换 XY */
#define MOKA_TOUCH_INV_X        0         /* 1 = 交换后翻转逻辑 X */
#define MOKA_TOUCH_INV_Y        0         /* 1 = 交换后翻转逻辑 Y */

/* 抬起迟滞：清 0x814E 后 GT911 重新填数有数 ms 窗口期，期间状态读为 0，
 * 不做迟滞会把一次按住误判成 PRESS/RELEASE 抖动风暴（实测 ~90Hz）。
 * 真实抬起的状态会稳定保持，40ms 迟滞可完全吸收刷新窗口。 */
#define MOKA_TOUCH_RELEASE_MS    40

/* 双击判定窗口：200ms 内第二次 PRESS 记为 DOUBLE（M5ReadPaper 验证值） */
#define MOKA_TOUCH_DOUBLE_MS    200
/* 按住期间的诊断日志节流间隔（真机调试坐标系用，定稿后可改 0 关闭） */
#define MOKA_TOUCH_DBG_MS       150

static uint8_t  s_addr;          /* 实际探测到的 I2C 地址，0 = 未初始化 */
static bool     s_pressed;       /* 上一次轮询时手指是否按下 */
static int16_t  s_last_x, s_last_y;
static int64_t  s_last_press_us; /* 上一次 PRESS 的时间戳（双击判定用） */
static int64_t  s_last_dbg_us;   /* 上一次诊断日志时间戳（节流） */
static int64_t  s_release_since; /* 抬起迟滞起点（us），0 = 无待决抬起 */

/* 原生坐标 → 逻辑竖屏坐标。三个宏在此集中生效。 */
static void map_coords(int32_t nx, int32_t ny, int16_t *lx, int16_t *ly)
{
#if MOKA_TOUCH_SWAP_XY
    int32_t x = ny;
    int32_t y = nx;
#else
    int32_t x = nx;
    int32_t y = ny;
#endif
#if MOKA_TOUCH_INV_X
    x = (MOKA_TOUCH_LOGIC_W - 1) - x;
#endif
#if MOKA_TOUCH_INV_Y
    y = (MOKA_TOUCH_LOGIC_H - 1) - y;
#endif
    /* 钳位防越界（GT911 边缘偶发溢出值） */
    if (x < 0) x = 0;
    if (x >= MOKA_TOUCH_LOGIC_W) x = MOKA_TOUCH_LOGIC_W - 1;
    if (y < 0) y = 0;
    if (y >= MOKA_TOUCH_LOGIC_H) y = MOKA_TOUCH_LOGIC_H - 1;
    *lx = (int16_t)x;
    *ly = (int16_t)y;
}

bool touch_init(void)
{
    if (s_addr != 0) {
        return true;    /* 幂等 */
    }
    if (!i2c_bus_init()) {
        MOKA_LOGE(TAG, "I2C 总线初始化失败");
        return false;
    }

    /* INT 平时为输入（带上拉）；进轻睡前 touch_prepare_wakeup() 会改配置 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << MOKA_TOUCH_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "INT GPIO%d 配置失败: %s", MOKA_TOUCH_INT_GPIO,
                  esp_err_to_name(err));
        return false;
    }

    /* 地址二选一：上电时 INT 电平决定 0x5D / 0x14 */
    if (i2c_bus_probe(MOKA_I2C_ADDR_GT911_A) == ESP_OK) {
        s_addr = MOKA_I2C_ADDR_GT911_A;
    } else if (i2c_bus_probe(MOKA_I2C_ADDR_GT911_B) == ESP_OK) {
        s_addr = MOKA_I2C_ADDR_GT911_B;
    } else {
        MOKA_LOGE(TAG, "GT911 探测失败（0x%02X / 0x%02X 均无应答）",
                  MOKA_I2C_ADDR_GT911_A, MOKA_I2C_ADDR_GT911_B);
        return false;
    }

    /* 读产品 ID 验证通信（应回 "911"），失败不算致命但必须打日志 */
    uint8_t pid[4] = {0};
    err = i2c_bus_read_reg(s_addr, GT911_REG_PRODUCT_ID, GT911_REG_LEN, pid, sizeof(pid));
    if (err == ESP_OK) {
        MOKA_LOGI(TAG, "GT911 在线 addr=0x%02X ProductID=%c%c%c%c",
                  s_addr, pid[0], pid[1], pid[2], pid[3]);
    } else {
        MOKA_LOGE(TAG, "读 ProductID 失败（addr=0x%02X）: %s",
                  s_addr, esp_err_to_name(err));
        return false;
    }

    /* 读芯片配置区的原生坐标分辨率：这是方向三宏（尤其是否交换 XY）的
     * 唯一权威依据——GT911 固件配置决定坐标输出范围，面板厂可烧横可烧竖。
     * 若读出 X=540 Y=960 说明芯片已按竖屏配置，不应再交换 XY。 */
    uint8_t cfg[GT911_CONFIG_BYTES] = {0};
    err = i2c_bus_read_reg(s_addr, GT911_REG_CONFIG, GT911_REG_LEN, cfg, sizeof(cfg));
    if (err == ESP_OK) {
        uint16_t res_x = (uint16_t)cfg[1] | ((uint16_t)cfg[2] << 8);
        uint16_t res_y = (uint16_t)cfg[3] | ((uint16_t)cfg[4] << 8);
        MOKA_LOGI(TAG, "GT911 配置版本=0x%02X 原生分辨率 X=%u Y=%u（宏设定 %dx%d）",
                  cfg[0], res_x, res_y, MOKA_TOUCH_NATIVE_W, MOKA_TOUCH_NATIVE_H);
    } else {
        MOKA_LOGE(TAG, "读 GT911 配置区失败: %s（按宏假定 %dx%d 继续）",
                  esp_err_to_name(err), MOKA_TOUCH_NATIVE_W, MOKA_TOUCH_NATIVE_H);
    }

    s_pressed = false;
    s_last_press_us = 0;
    s_release_since = 0;
    return true;
}

bool touch_poll(touch_event_t *ev)
{
    if (s_addr == 0 || ev == NULL) {
        return false;
    }

    /* 不以 INT 电平做门控：GT911 的 INT 默认是"短脉冲"而非持续低电平，
     * 10ms 轮询很可能错过脉冲导致触摸全无响应；直接读 0x814E 状态寄存器
     * 才是可靠做法（M5GFX 驱动同样如此），一次 I2C 读开销可忽略。 */
    uint8_t status = 0;
    if (i2c_bus_read_reg(s_addr, GT911_REG_STATUS, GT911_REG_LEN,
                         &status, 1) != ESP_OK) {
        return false;   /* 错误已在 i2c_bus 内打日志，本轮跳过 */
    }
    bool ready  = (status & GT911_STATUS_READY) != 0;
    int  points = status & GT911_STATUS_POINTS;

    if (ready && points > 0) {
        uint8_t pt[GT911_POINT_BYTES] = {0};
        if (i2c_bus_read_reg(s_addr, GT911_REG_POINT1, GT911_REG_LEN,
                             pt, sizeof(pt)) != ESP_OK) {
            return false;
        }
        int32_t nx = (int32_t)pt[0] | ((int32_t)pt[1] << 8);
        int32_t ny = (int32_t)pt[2] | ((int32_t)pt[3] << 8);
        map_coords(nx, ny, &s_last_x, &s_last_y);

        /* 真机诊断：按住期间节流打印 原生→逻辑 坐标，用于核定方向三宏 */
        int64_t now_dbg = esp_timer_get_time();
        if (now_dbg - s_last_dbg_us >= (int64_t)MOKA_TOUCH_DBG_MS * 1000) {
            s_last_dbg_us = now_dbg;
            MOKA_LOGI(TAG, "触点 st=0x%02X 原生(%ld,%ld) -> 逻辑(%d,%d)",
                      status, (long)nx, (long)ny, (int)s_last_x, (int)s_last_y);
        }
    }

    /* 读完必须写 0 清 0x814E，否则坐标缓冲不再更新 */
    if (status != 0) {
        uint8_t zero = 0;
        if (i2c_bus_write_reg(s_addr, GT911_REG_STATUS, GT911_REG_LEN,
                              &zero, 1) != ESP_OK) {
            return false;
        }
    }

    /* 手指抬起判定（带 40ms 迟滞，抗 GT911 刷新窗口抖动）：
     * 无触点且持续超过 MOKA_TOUCH_RELEASE_MS 才确认 RELEASE。 */
    if (!ready || points == 0) {
        if (s_pressed) {
            int64_t now = esp_timer_get_time();
            if (s_release_since == 0) {
                s_release_since = now;      /* 首次观察到无触点，开始计时 */
                return false;
            }
            if (now - s_release_since < (int64_t)MOKA_TOUCH_RELEASE_MS * 1000) {
                return false;               /* 窗口期内，暂不判抬起 */
            }
            s_release_since = 0;
            s_pressed = false;
            ev->kind = TOUCH_EV_RELEASE;
            ev->x = s_last_x;
            ev->y = s_last_y;
            MOKA_LOGI(TAG, "RELEASE 逻辑(%d,%d)", (int)ev->x, (int)ev->y);
            return true;
        }
        return false;
    }

    s_release_since = 0;    /* 有触点：取消任何待决抬起 */

    if (s_pressed) {
        return false;   /* 持续按住中：不重复发 PRESS，也无 MOVE 事件 */
    }

    /* 新的一次按下：200ms 内第二次 PRESS → DOUBLE */
    s_pressed = true;
    int64_t now = esp_timer_get_time();
    bool is_double = (s_last_press_us != 0) &&
                     ((now - s_last_press_us) < (int64_t)MOKA_TOUCH_DOUBLE_MS * 1000);
    s_last_press_us = now;

    ev->kind = is_double ? TOUCH_EV_DOUBLE : TOUCH_EV_PRESS;
    ev->x = s_last_x;
    ev->y = s_last_y;
    MOKA_LOGI(TAG, "%s 逻辑(%d,%d)", is_double ? "DOUBLE" : "PRESS",
              (int)ev->x, (int)ev->y);
    return true;
}

void touch_prepare_wakeup(void)
{
    /* 轻睡触摸唤醒（M5ReadPaper 同款方案，setup.cpp 实测可用）：
     * GPIO48 不在 ESP32-S3 的 RTC GPIO 域（0~21），深睡 ext1 物理不可用；
     * 但轻睡眠的 gpio_wakeup 支持任意数字 GPIO——GT911 INT 平时高、
     * 触摸时拉低，配成低电平唤醒源即可。唤醒后由 power 层做
     * "1100ms 内 3 触"确认，防误触。 */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << MOKA_TOUCH_INT_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* INT 开漏，睡眠期间需保持高电平 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "唤醒前 GPIO%d 配置失败: %s", MOKA_TOUCH_INT_GPIO,
                  esp_err_to_name(err));
        return;
    }
    err = gpio_wakeup_enable(MOKA_TOUCH_INT_GPIO, GPIO_INTR_LOW_LEVEL);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "gpio_wakeup_enable 失败: %s", esp_err_to_name(err));
        return;
    }
    err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "esp_sleep_enable_gpio_wakeup 失败: %s", esp_err_to_name(err));
        return;
    }
    MOKA_LOGI(TAG, "触摸唤醒就绪：GPIO%d 低电平（轻睡 gpio_wakeup）", MOKA_TOUCH_INT_GPIO);
}
