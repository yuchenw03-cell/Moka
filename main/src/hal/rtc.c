/**
 * rtc.c —— BM8563 RTC 实现（签名见 rtc.h，不可改）
 *
 * 关键约束：
 * - civil↔unix 换算用自实现的 Howard Hinnant 天数算法，
 *   禁止 mktime/localtime（时区陷阱：固件不设 TZ，行为不可预期）
 * - rtc_power_off() 是 PaperS3 的"真关机"机制：配置 BM8563 倒计时定时器，
 *   1 秒后 INT 输出低脉冲，触发板载 PMS150G 电源芯片切断整机供电。
 *   定时器位定义已对照 PCF8563 datasheet 复核（BM8563 与其寄存器兼容）：
 *   CTRL_STAT2(0x01)：bit7 TI_TP / bit6 AF / bit5 TF / bit4 AIE / bit3 TIE；
 *   TIMER_CTRL(0x0E)：bit7 TE / bit[1:0] TD（00=4.096kHz 01=64Hz 10=1Hz 11=1/60Hz）。
 *   注意 TIE 在 bit3 而非 bit4——bit4 是报警中断 AIE，写错会误开闹钟中断。
 */
#include "rtc.h"
#include "i2c_bus.h"

#include "../log.h"

#define TAG "rtc"

/* ---- BM8563 寄存器（8 位地址）---- */
#define BM8563_REG_CTRL_STAT1   0x00    /* 控制/状态 1 */
#define BM8563_REG_CTRL_STAT2   0x01    /* 控制/状态 2：bit7 TI_TP, bit6 AF, bit5 TF, bit4 AIE, bit3 TIE */
#define BM8563_REG_SECONDS      0x02    /* 秒（BCD），bit7 = VL 电压低标志 */
#define BM8563_REG_MINUTES      0x03
#define BM8563_REG_HOURS        0x04
#define BM8563_REG_DAYS         0x05
#define BM8563_REG_WEEKDAYS     0x06
#define BM8563_REG_MONTHS       0x07    /* bit7 = 世纪位 C（0=20xx） */
#define BM8563_REG_YEARS        0x08
#define BM8563_REG_TIMER_CTRL   0x0E    /* bit7 TE 使能, bit[1:0] TD 时钟源 */
#define BM8563_REG_TIMER_VAL    0x0F    /* 倒计时初值 */

#define BM8563_REG_LEN          1
#define BM8563_TIME_BYTES       7       /* 0x02..0x08 一次连读 */

#define BM8563_SECONDS_VL       0x80    /* VL=1：曾掉电，时间不可信 */
#define BM8563_CTRL2_TI_TP      0x80    /* bit7：INT 模式（0=脉冲 1=电平保持） */
#define BM8563_CTRL2_AF         0x40    /* bit6：报警标志（写 0 清除） */
#define BM8563_CTRL2_TF         0x20    /* bit5：定时器溢出标志（写 0 清除） */
#define BM8563_CTRL2_AIE        0x10    /* bit4：报警中断使能（本固件不用，保持 0） */
#define BM8563_CTRL2_TIE        0x08    /* bit3：定时器中断使能（INT 输出脉冲） */
#define BM8563_TCTRL_TE         0x80    /* 定时器使能 */
#define BM8563_TCTRL_TD_1HZ     0x02    /* TD=10b：倒计时时钟 1Hz */

#define MOKA_RTC_YEAR_BASE      2000    /* 世纪位 C=0 时的年基 */

/* ---- BCD 编解码 ---- */
static uint8_t bcd_decode(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bcd_encode(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* ---- Howard Hinnant 天数算法（公开算法，无时区依赖）----
 * days_from_civil: 公历日期 → 1970-01-01 起的天数（可负）
 * civil_from_days: 反变换 */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);                       /* [0, 399] */
    unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;  /* [0, 365] */
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           /* [0, 146096] */
    return era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);                    /* [0, 146096] */
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  /* [0, 399] */
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);         /* [0, 365] */
    unsigned mp = (5 * doy + 2) / 153;                              /* [0, 11] */
    *d = doy - (153 * mp + 2) / 5 + 1;                              /* [1, 31] */
    *m = mp + (mp < 10 ? 3 : (unsigned)-9);                         /* [1, 12] */
    *y = (int)(yy + (*m <= 2));
}

bool rtc_clock_init(void)   /* 命名依据见 rtc.h：避开 IDF 的 rtc_init 符号冲突 */
{
    if (!i2c_bus_init()) {
        MOKA_LOGE(TAG, "I2C 总线初始化失败");
        return false;
    }
    if (i2c_bus_probe(MOKA_I2C_ADDR_BM8563) != ESP_OK) {
        MOKA_LOGE(TAG, "BM8563 无应答（addr=0x%02X）", MOKA_I2C_ADDR_BM8563);
        return false;
    }

    /* 上电解除残留的定时器/报警：出厂固件（EDC Book）会在 BM8563 里
     * 布置定时器，其 INT 脉冲触发 PMS150G 切断整机电源——不解除会造成
     * 周期性断电重启（真机首刷观察到的"反复重连"即由此而来）。 */
    esp_err_t derr;
    uint8_t zero = 0;
    derr = i2c_bus_write_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_TIMER_CTRL,
                             BM8563_REG_LEN, &zero, 1);   /* TE=0：停定时器 */
    if (derr != ESP_OK) {
        MOKA_LOGE(TAG, "关闭 BM8563 定时器失败: %s", esp_err_to_name(derr));
    }
    uint8_t ctrl2 = 0;
    derr = i2c_bus_read_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_CTRL_STAT2,
                            BM8563_REG_LEN, &ctrl2, 1);
    if (derr == ESP_OK) {
        ctrl2 &= (uint8_t)~(BM8563_CTRL2_TIE | BM8563_CTRL2_AIE |
                            BM8563_CTRL2_TF | BM8563_CTRL2_AF);
        derr = i2c_bus_write_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_CTRL_STAT2,
                                 BM8563_REG_LEN, &ctrl2, 1);
    }
    if (derr != ESP_OK) {
        MOKA_LOGE(TAG, "清除 BM8563 中断/标志失败: %s", esp_err_to_name(derr));
    }

    /* VL 标志：RTC 备用电源曾掉电 → 时间不可信，提示上层需重新设时 */
    uint8_t sec = 0;
    esp_err_t err = i2c_bus_read_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_SECONDS,
                                     BM8563_REG_LEN, &sec, 1);
    if (err != ESP_OK) {
        return false;   /* 日志已在 i2c_bus 内打过 */
    }
    if (sec & BM8563_SECONDS_VL) {
        MOKA_LOGE(TAG, "VL=1：RTC 曾掉电，时间不可信，需要 rtc_set_unix 重设");
        /* 不视为初始化失败：时钟仍能走，只是当前值不可靠 */
    }
    MOKA_LOGI(TAG, "BM8563 在线 addr=0x%02X", MOKA_I2C_ADDR_BM8563);
    return true;
}

uint32_t rtc_get_unix(void)
{
    uint8_t r[BM8563_TIME_BYTES] = {0};
    esp_err_t err = i2c_bus_read_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_SECONDS,
                                     BM8563_REG_LEN, r, sizeof(r));
    if (err != ESP_OK) {
        return 0;   /* 读失败返回 0（1970 纪元），上层可据此判无效 */
    }

    int sec  = bcd_decode(r[0] & 0x7F);   /* 剥掉 VL 位 */
    int min  = bcd_decode(r[1] & 0x7F);
    int hour = bcd_decode(r[2] & 0x3F);   /* 24h 模式只用 bit[5:0] */
    int day  = bcd_decode(r[3] & 0x3F);
    int mon  = bcd_decode(r[5] & 0x1F);       /* r[5] = months，bit7 世纪位 */
    int year = bcd_decode(r[6]) + MOKA_RTC_YEAR_BASE;

    int64_t days = days_from_civil(year, (unsigned)mon, (unsigned)day);
    int64_t ts = days * 86400 + hour * 3600 + min * 60 + sec;
    if (ts < 0) {
        MOKA_LOGE(TAG, "RTC 日期早于 1970（%d-%02d-%02d），视为无效", year, mon, day);
        return 0;
    }
    return (uint32_t)ts;
}

bool rtc_set_unix(uint32_t ts)
{
    int64_t days = (int64_t)ts / 86400;
    int64_t rem  = (int64_t)ts % 86400;
    int year;
    unsigned mon, day;
    civil_from_days(days, &year, &mon, &day);

    int hour = (int)(rem / 3600);
    int min  = (int)((rem % 3600) / 60);
    int sec  = (int)(rem % 60);

    if (year < MOKA_RTC_YEAR_BASE || year > MOKA_RTC_YEAR_BASE + 99) {
        MOKA_LOGE(TAG, "年份 %d 超出 BM8563 可写范围（%d~%d）",
                  year, MOKA_RTC_YEAR_BASE, MOKA_RTC_YEAR_BASE + 99);
        return false;
    }

    uint8_t w[BM8563_TIME_BYTES];
    w[0] = bcd_encode((uint8_t)sec);          /* VL 位写 0：时间可信 */
    w[1] = bcd_encode((uint8_t)min);
    w[2] = bcd_encode((uint8_t)hour);
    w[3] = bcd_encode((uint8_t)day);
    w[4] = 0;                                  /* weekday 不用，写 0 */
    w[5] = bcd_encode((uint8_t)mon);           /* C=0 → 20xx */
    w[6] = bcd_encode((uint8_t)(year - MOKA_RTC_YEAR_BASE));

    esp_err_t err = i2c_bus_write_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_SECONDS,
                                      BM8563_REG_LEN, w, sizeof(w));
    if (err != ESP_OK) {
        return false;   /* 日志已在 i2c_bus 内打过 */
    }
    MOKA_LOGI(TAG, "RTC 已设时 %04d-%02u-%02u %02d:%02d:%02d (unix=%lu)",
              year, mon, day, hour, min, sec, (unsigned long)ts);
    return true;
}

void rtc_power_off(void)
{
    /* PaperS3 "真关机"：BM8563 倒计时定时器溢出 → INT 拉低一个脉冲 →
     * PMS150G 检测到脉冲后切断整机电源（这是硬件链路，不是软件睡眠）。
     * 配置：倒计时值=1，时钟源 1Hz（TD=10b），TE=1 使能定时器，
     *       TIE=1 使能 INT 输出，TI_TP=0（INT 为脉冲模式而非电平保持）。
     * 因此断电发生在此函数返回约 1 秒后 —— 调用方在这之前完成 SD 落盘。
     * 位定义已按 PCF8563 datasheet 复核（BM8563 寄存器兼容，见文件头）。 */
    uint8_t v;
    esp_err_t err;

    /* 1) 清 TF 标志并确保 AIE=0 / TI_TP=0：
     * 避免残留标志直接触发、避免误开的闹钟中断抢走 INT 脉冲 */
    err = i2c_bus_read_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_CTRL_STAT2,
                           BM8563_REG_LEN, &v, 1);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "读 CTRL_STAT2 失败，放弃断电流程");
        return;
    }
    v &= (uint8_t)~(BM8563_CTRL2_TF | BM8563_CTRL2_AIE | BM8563_CTRL2_TI_TP);
    err = i2c_bus_write_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_CTRL_STAT2,
                            BM8563_REG_LEN, &v, 1);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "清 TF 标志失败，放弃断电流程");
        return;
    }

    /* 2) 倒计时初值 = 1（1Hz 时钟下即 1 秒后溢出） */
    v = 1;
    err = i2c_bus_write_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_TIMER_VAL,
                            BM8563_REG_LEN, &v, 1);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "写倒计时值失败，放弃断电流程");
        return;
    }

    /* 3) 定时器控制：TE=1 使能，TD=1Hz */
    v = BM8563_TCTRL_TE | BM8563_TCTRL_TD_1HZ;
    err = i2c_bus_write_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_TIMER_CTRL,
                            BM8563_REG_LEN, &v, 1);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "写 TIMER_CTRL 失败，放弃断电流程");
        return;
    }

    /* 4) 使能定时器中断输出（INT 低脉冲 → PMS150G 断电） */
    err = i2c_bus_read_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_CTRL_STAT2,
                           BM8563_REG_LEN, &v, 1);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "回读 CTRL_STAT2 失败，放弃断电流程");
        return;
    }
    v |= BM8563_CTRL2_TIE;
    err = i2c_bus_write_reg(MOKA_I2C_ADDR_BM8563, BM8563_REG_CTRL_STAT2,
                            BM8563_REG_LEN, &v, 1);
    if (err != ESP_OK) {
        MOKA_LOGE(TAG, "使能 TIE 失败，放弃断电流程");
        return;
    }

    MOKA_LOGI(TAG, "RTC 断电定时器已启动，约 1 秒后 PMS150G 切断电源");
}
