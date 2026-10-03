# 墨卡（MoKa）阶段 1：整体架构

> 目标硬件：M5 Paper S3（ESP32-S3R8，8MB Octal PSRAM，16MB Flash，4.7" 960×540 EPD_ED047TC1 直驱，GT911 触摸，BMI270 IMU，BM8563 RTC，1800mAh）
> 工具链：ESP-IDF v5.5.5（~/esp/esp-idf，本机已装好，含 esp32s3 交叉编译器/cmake/ninja/openocd/esptool）
> 全程离线：无网络请求，WiFi 默认关闭。

## 1. 整体架构图（文字版）

```
┌────────────────────────────────────────────────────────┐
│  app/   应用层                                          │
│  ├─ review.cpp   复习流程编排（模式A/模式B）            │
│  ├─ menu.cpp     开机菜单/词书切换/设置                 │
│  ├─ stats_page.cpp 统计页（今日/曲线/生词本）           │
│  └─ app_fsm.cpp  顶层状态机（消息队列驱动）             │
├────────────────────────────────────────────────────────┤
│  ui/    页面渲染（只画帧缓冲，不碰硬件寄存器）          │
│  ├─ card.cpp   卡片正面/背面渲染                        │
│  ├─ menu.cpp   列表/设置界面渲染                        │
│  └─ stats.cpp  柱状图/数字渲染                          │
├──────────────────────┬─────────────────────────────────┤
│  srs/   纯 C (C99)   │  hal/   硬件抽象层（C 风格 API）│
│  ├─ srs_engine.c     │  ├─ display.c   epdiy 封装      │
│  │  SM-2 状态计算    │  ├─ touch.c     GT911           │
│  ├─ srs_queue.c      │  ├─ sdcard.c    1-bit SDMMC     │
│  │  今日队列调度     │  ├─ rtc.c       BM8563          │
│  └─ srs_store.c      │  └─ buzzer.c    蜂鸣器（可选）  │
│     .srs 持久化      │                                │
│  * 不 include 任何   │  * 每个 I2C/SPI 调用必查返回值  │
│    Arduino/M5/IDF    │  * 对上层只暴露 C API           │
│    头文件，PC gcc    │                                │
│    可单测            │                                │
└──────────────────────┴─────────────────────────────────┘
              ESP-IDF v5.5.5（FreeRTOS）
        epdiy ≥2.0 · GT911 · BMI270 · BM8563 · SDMMC-1bit
```

依赖方向只允许向下：app → {srs, ui} → hal。srs 不允许依赖 ui/hal；
hal 不允许依赖 app/ui。ui 只操作 hal 提供的帧缓冲指针与刷新 API。

## 2. 任务模型（两个任务 + 一个 10ms 输入轮询）

- `main_fsm_task`（core 1，16KB 栈）：顶层状态机。所有状态转移、队列调度、
  持久化都在这里串行发生，避免加锁地狱。
- `disp_push_task`（core 0，8KB 栈）：显示推送。app/ui 只往消息队列塞"刷新请求"
  （脏矩形 + 模式），由本任务统一执行局刷/全刷、刷新计数、电源开关。
  —— 这是 M5ReadPaper 验证过的形态：刷屏绝不能和业务混在同一任务里。
- 输入轮询：10ms 硬件定时器 notify 唤醒 `DeviceInterruptTask` 内嵌逻辑
  （触摸事件、双击判定 200ms、IMU 自动旋转去抖：连续 4 次一致 + 600ms 保持）。
  本任务只做"采集→投递消息"，不做渲染。

## 3. 目录树

```
moka/
├── CMakeLists.txt              # IDF 工程根（最小 stub）
├── partitions.csv              # 自定义 16MB 分区表（见下）
├── sdkconfig.defaults          # esp32s3 + Octal PSRAM + 分区表 等
├── main/
│   ├── CMakeLists.txt          # GLOB 收集 src/**，注册组件
│   └── src/
│       ├── main.cpp            # 仅初始化 + 状态机调度
│       ├── app/                # 应用层（C++）
│       ├── srs/                # 纯 C99：SM-2 / 队列 / 持久化
│       ├── hal/                # 硬件抽象（C API，内部可用 C++）
│       └── ui/                 # 页面渲染（C++）
├── tests/
│   └── srs_pc/                 # PC 端 gcc 单测（阶段 2 交付）
│       ├── test_srs_engine.c
│       └── Makefile            # gcc 直编 srs/*.c，无 IDF 依赖
├── tools/
│   └── wbx_gen/                # PC 端 .wbx 词书生成脚本（阶段 2 交付）
│       ├── wbx_gen.py
│       └── README.md           # 输入/输出格式说明
└── docs/
    ├── stage1_architecture.md  # 本文件
    ├── wbx_format.md           # .wbx 二进制格式定义
    ├── srs_persistence.md      # .srs / .stats 文件布局与断电安全
    └── sm2_state_machine.md    # SM-2 状态转移表
```

## 4. 分区表（partitions.csv，16MB 对齐 EDC Book 的 font 分区思路）

```
# name,     type, subType,  offset,    size
nvs,        data, nvs,      0x9000,    0x5000    # 20KB  WiFi 校准等（基本闲置）
otadata,    data, ota,      0xE000,    0x2000    # 8KB   保留 OTA 能力
app0,       app,  factory,  0x10000,   0x400000  # 4MB   固件（无内嵌字体，4MB 充裕）
font,       data, 0x40,     0x410000,  0xA00000  # 10MB  字体分区（可直接灌入
                                                  #       EDC 格式的 43511 字 30px 中文字库，
                                                  #       兼容 IPA 音标字体的同格式扩展）
spiffs,     data, spiffs,   0xE10000,  0x1F0000  # 1984KB 内置资源（帮助图/默认词书）
# 结束于 0x1000000 = 16MB 边界
```

设计理由：字体走独立分区而不是内嵌 PROGMEM——M5ReadPaper 把 7MB 字体编进
app 导致 app 分区 13.5MB；独立分区后 app 4MB 绰绰有余，且字体可单独烧录/升级。

## 5. 关键决策记录（Decision Log）

- **渲染后端：epdiy 原生绘制（纯 C）**，不引入 M5GFX/Arduino。
  理由：需求 NFR-5 说"M5GFX 内置 efont 不含 IPA 字符，须从 SD 卡加载 .bin 字体"——
  既然字体引擎必须自研，M5GFX 的绘制优势就不再成立；epdiy 就是为 ED047TC1
  直驱屏写的纯 C 驱动（16 灰度、局部刷新、LUT 可调），与"纯 C/C++"目标吻合。
  hal/display.c 内部用 epdiy 的 epd_draw / epd_write_string（字体后接自研 bin
  渲染器），若后期确需 M5GFX 画布特性，只在 hal 内部换实现，上层无感。
- **字体方案：沿用 EDC Book / M5ReadPaper 同源的 1-bit 打包位图 bin 格式**
  （134 字节头 + 每字形索引），中文字库直接灌入 font 分区；IPA 音标用同格式
  生成一份小字体（覆盖 U+0250–U+02AF 等），从 SD 卡 /font/ 加载，失败则
  降级隐藏音标行（NFR-5 要求"降级为不显示音标而非乱码"）。
- **时间基准：BM8563 RTC**（首次手动设置/联网写入，之后离线走时）。
- **电源模型（2026-09 修订：深睡→轻睡）**：闲置 120s → 保存 → 轻睡眠。
  原设计"深睡眠 + GPIO48 ext1 触摸唤醒"在真机上被证伪（GPIO48 不在
  ESP32-S3 RTC GPIO 域 0~21，ext1 物理不可用，深睡后唤不醒）；
  改用轻睡 gpio_wakeup（任意数字 GPIO 可用）+ "1100ms 内 3 触"唤醒确认
  （M5ReadPaper 同方案），RAM/帧缓冲全程保持，唤醒零重绘。
  代价：功耗 µA 级 → mA 级以下，长时间不用应走 [关机] 彻底断电。
  电源键（PMS150G 硬复位）作为兜底唤醒：RTC 快照在 EN 复位后仍保持，
  冷启动凭快照断点续读。
- **残影纪律（NFR-2）**：刷新计数器在 disp_push_task 内集中管理，
  每 10 次局刷强制 1 次全刷；休眠前必做全刷。此处直接采用 EDC/M5ReadPaper
  验证过的策略，但阈值按规范固定为 10。
- **磁盘纪律（断电安全）**：SD 写入后 delay(500ms) 再断电（100ms 不够，
  M5ReadPaper 实测教训）；.srs 合并采用 tmp+rename 原子替换。

## 6. 阶段-文件对照

| 阶段 | 交付 | 涉及路径 |
|---|---|---|
| 1（本次）| 架构/格式/转移表 | docs/*.md + 工程骨架 |
| 2 | SRS 引擎 + PC 单测 | main/src/srs/*.c, tests/srs_pc/ |
| 3 | hal 驱动 + 点亮 Demo | main/src/hal/*, main/src/main.cpp |
| 4 | 模式 A 闭环 | app/, ui/card.cpp, srs_store.c |
| 5 | 模式 B / 统计 / 生词本 / 电源 | app/, ui/stats.cpp, app/power.cpp |
