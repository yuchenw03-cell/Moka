/**
 * log.h —— 统一日志宏（NFR-3：串口 115200，可一键关闭）
 * 为什么独立成头文件：规范要求"一键关闭"，集中定义后只需改
 * MOKA_LOG_ENABLE 一个宏即可全量静默。
 */
#pragma once

#include <stdio.h>

#ifndef MOKA_LOG_ENABLE
#define MOKA_LOG_ENABLE 1
#endif

#if MOKA_LOG_ENABLE
#include "esp_log.h"
#define MOKA_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define MOKA_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#else
#define MOKA_LOGI(tag, fmt, ...) ((void)0)
#define MOKA_LOGE(tag, fmt, ...) ((void)0)
#endif
