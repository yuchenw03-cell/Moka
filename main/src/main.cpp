/**
 * main.cpp —— 墨卡入口：仅初始化与状态机调度（规范五节：main 不放业务逻辑）
 *
 * 启动序列：日志 → PSRAM 稳定等待 → app_fsm_start()。
 * 硬件初始化（RTC/SD/显示/字体/触摸）全部在 app_fsm 的 boot() 内按需进行，
 * 顺序与失败降级策略见 app_fsm.cpp::boot() 注释。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"

#include "app/app_fsm.h"
#include "log.h"          /* MOKA_LOG* 统一日志宏（NFR-3，可一键关闭） */

static const char *TAG = "moka.main";

extern "C" void app_main(void)
{
    /* 复位原因是定位"电源键行为"的关键证据：
     * 1=上电（PMS150G 重新供电）/ 3=EN 外部复位（PMS150G 拉 EN）/
     * 4=软件复位 / 12=USB-JTAG 复位。真机按电源键后看这里即可判定
     * PMS150G 走的是断电还是 EN 复位路径。 */
    MOKA_LOGI(TAG, "墨卡 boot（复位原因=%d）", (int)esp_reset_reason());

    /* PSRAM 上电稳定期：M5ReadPaper 实测 boot 后立即访问 Octal PSRAM 偶发翻车，
     * 先等 100ms 再让各模块做大块分配（docs/stage1_architecture.md 教训条目）。 */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* 主状态机跑在 core 1 的独立任务里；app_main 返回后本任务退出，
     * 系统任务与 FSM 继续运行（IDF 允许 app_main 返回）。 */
    app_fsm_start();
}
