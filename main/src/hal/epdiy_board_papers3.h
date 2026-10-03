/**
 * epdiy_board_papers3.h —— M5 Paper S3 自定义 epdiy 板级定义（epdiy v2.1.3）
 * ED047TC1 直连 ESP32-S3（LCD_CAM 外设驱动），引脚按 M5 官方 PinMap。
 * 用法：epd_init(&epd_board_papers3, &ED047TC1, EPD_OPTIONS_DEFAULT);
 * （签名依据 components/epdiy/src/epdiy.h:206）
 */
#ifndef MOKA_EPDIY_BOARD_PAPERS3_H
#define MOKA_EPDIY_BOARD_PAPERS3_H

#include "epd_board.h"   /* EpdBoardDefinition（epdiy v2 板级抽象） */

#ifdef __cplusplus
extern "C" {
#endif

extern const EpdBoardDefinition epd_board_papers3;

#ifdef __cplusplus
}
#endif
#endif
