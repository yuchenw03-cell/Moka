# 墨卡 MoKa

**运行在 M5 PaperS3 墨水屏上的离线单词学习卡固件** —— 交互对标"墨墨背单词"，
全程离线、零网络依赖，面向考研 / 四六级词汇记忆场景。

![Version](https://img.shields.io/badge/version-1.0.0-blue)
![Platform](https://img.shields.io/badge/platform-M5%20PaperS3-green)
![Framework](https://img.shields.io/badge/ESP--IDF-v5.5-red)
![License](https://img.shields.io/badge/license-MIT-lightgrey)

---

## 特性

- **两种复习模式**
  - 模式 A「看词回忆」（主模式）：正面单词+音标+词性 → 点屏翻面 → 释义+例句
    （目标词反色高亮）→ [忘记]/[模糊]/[认识] 三档自评
  - 模式 B「看释义选 ABCD」：干扰项 100% 来自同词频桶（COCA 0-9 分桶），
    选错自动重插今日队尾并在收卡时强制补一轮模式 A
- **标准 SM-2 调度引擎**（纯 C99 实现，无任何硬件依赖，PC 端可单测）：
  EF 系数、间隔增长、答错降级，严格按经典算法实现
- **进度持久化**：每次自评即追加 64B 日志，满 20 条或休眠/关机前
  合并主记录；`.tmp + .bak` 三段轮换写入，**断电不丢进度**
- **统计页**：今日待复习/新学/已复习、连续打卡天数、近 30 天复习量柱状图、
  生词本（答错 ≥2 次自动收录，可单独成册复习）
- **电源管理**：闲置 120s 自动轻睡，**1100ms 内连触 3 次屏幕唤醒**
  （零重绘秒回睡前页面）；屏幕 [关机] 键显示品牌页后整机断电
  （墨水屏断电留图），再按电源键开机
- **可换字体**：SD 卡 `/sdcard/font/cn.bin` 优先于内置字库，最近邻放大
  渲染（1.0x~4.0x 可调），字库基线自动校正
- **多词书**：SD 卡 `/sdcard/wordbook/*.wbx` 即插即用，菜单显示
  总词数/已学/待复习

## 硬件平台

| 部件 | 型号 | 说明 |
|---|---|---|
| 主控 | ESP32-S3R8 | 240MHz 双核 + 8MB Octal PSRAM |
| 屏幕 | ED047TC1 | 540×960，16 灰阶电子墨水屏 |
| 触摸 | GT911 | I2C @ 0x14，540×960 竖屏坐标 |
| 时钟 | BM8563 | RTC，离线走时 |
| 电源 | PMS150G | 整机电源管理（GPIO44 脉冲断电） |
| 存储 | microSD | 词书 / 进度 / 字体 / 统计 |

## 快速开始

### 环境

- ESP-IDF **v5.5**（其余版本未验证）
- epdiy v2.1.3 已内置于 `components/epdiy`，无需额外下载

### 编译与烧录

```bash
source ~/esp/esp-idf/export.sh        # 按你的 IDF 安装路径调整
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/tty.usbmodem2101 flash monitor   # 端口按实际调整
```

分区表（`partitions.csv`）：`app0` 4MB 固件 + `font` 10MB 字库分区 + `spiffs`。

### SD 卡准备

```
/sdcard/
├── wordbook/
│   ├── CET4.wbx          # 词书（tools/wbx_gen 生成）
│   ├── moka.cfg          # 设置（自动生成）
│   ├── .stats            # 每日统计（自动生成）
│   └── CET4.wbx.srs      # 学习进度（自动生成）
└── font/
    ├── cn.bin            # 中文字体（可选，缺失时用内置字库）
    └── ipa.bin           # IPA 音标字体（可选，缺失时隐藏音标行）
```

### 制作词书

```bash
cd tools/wbx_gen
python3 wbx_gen.py --input your_words.jsonl --name "四级词汇" --output CET4.wbx
python3 wbx_gen.py --verify CET4.wbx     # 校验（含干扰项跨桶检查）
```

`.wbx` 二进制格式规范见 [docs/wbx_format.md](docs/wbx_format.md)。

## 项目结构

```
main/src/
├── main.cpp          # 入口：仅初始化与状态机调度
├── app/              # 应用层：复习流程 / 菜单 / 设置 / 统计 / 电源（业务逻辑）
├── srs/              # 纯 C99：SM-2 引擎 / 队列调度 / 持久化 / 词书解析
│                     # （不依赖任何 ESP 头文件，PC 端 gcc 可编译单测）
├── hal/              # 硬件抽象：display / touch / sdcard / rtc / i2c / font /
│                     #  button / powerctl / buzzer（对上层只暴露 C 风格 API）
└── ui/               # 页面渲染：card / menu / stats / shutdown / render
tests/srs_pc/         # PC 端单元测试（11 个用例：SM-2 状态转移 / 队列 /
                      #  持久化断电恢复 / 干扰项同桶校验 / 连续打卡）
tools/                # wbx_gen 词书生成器 / 字体工具
docs/                 # 架构、SM-2 状态机、.wbx 格式、持久化设计文档
```

### 运行测试

```bash
cd tests/srs_pc && make && ./srs_pc_tests
```

## 使用说明

- **主页面**：点词书行开始今日复习；标题栏 [关机] [统计] [设置]
- **复习页**：顶部显示「今日待学 x/x · 今日待复习 x/x」；左上角箭头退出
  （弹窗确认，退出自动落盘进度）
- **设置页**：每日新词上限（1-100）、默认复习模式（A/B）、时间校准、
  单词字号（1.0x-4.0x，0.5x 步进）
- **物理电源键**：短按 = 硬件复位（PMS150G 设计，固件不可拦截）；
  要关机请用屏幕上的 [关机] 按钮

## 已知限制

- 触摸唤醒走**轻睡眠**（GT911 INT 接 GPIO48，不在 ESP32-S3 RTC GPIO 域，
  深睡触摸唤醒物理不可行）：轻睡功耗高于深睡（µA 级 → mA 级以下），
  是触摸唤醒的必要代价；长时间不用请用 [关机] 彻底断电
- 内置 font 分区字库尾部约 80KB 被 10MB 分区边界截断（全角标点区），
  固件自动回退半角显示；使用 SD 卡字体（如 cn.bin）无此问题
- 音标显示需要自备支持 IPA 的 `.bin` 字体（格式见 docs/），缺失时音标行
  自动隐藏而不显示乱码

## 字体版权说明

本仓库**不包含**任何字体文件。`tools/cn_38px.bin`（汉仪空山楷）与
`tools/font_partition.bin`（设备提取字库）仅供本地开发调试，已加入
.gitignore，请勿二次分发。请使用你拥有合法授权的字体自行生成 `.bin`
字库（生成工具见 `tools/` 与 `docs/wbx_format.md` 同源规范）。

## 第三方组件与致谢

- [epdiy](https://github.com/vroland/epdiy) v2.1.3 —— 墨水屏驱动核心
  （**LGPL-3.0**，见 `components/epdiy/LICENSE`；本工程以 ESP-IDF 组件方式
  链接，构建产物含完整目标文件，满足 LGPL-3.0 重链接要求）
- [M5ReadPaper](https://github.com/) —— PaperS3 板级细节与字体格式的
  参考实现（本工程代码为独立实现）
- SM-2 算法 © SuperMemo World

## 开源协议

本工程自有代码（`main/`、`tests/`、`tools/`、`docs/`）以 **MIT License**
发布，见 [LICENSE](LICENSE)。

`components/epdiy/` 为第三方组件，遵循其自身 LGPL-3.0 协议。
