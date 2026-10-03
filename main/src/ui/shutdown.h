/**
 * shutdown.h —— 关机画面（电源键关机后墨水屏断电留图，常显品牌页）
 */
#pragma once

/* 只写帧缓冲不刷屏：关机流程要求全刷，由 power_shutdown() 统一 disp_push_full */
void ui_shutdown_render(void);
