/**
 * render.h —— 帧缓冲绘图助手 + 全屏触摸命中区域表
 *
 * 触摸命中区域表（逻辑屏 540x960）是 app 层命中判定与 ui 层绘制的
 * 唯一权威来源：两侧都只引用这里的宏，禁止散落裸数字（规范硬性要求）。
 *
 * 刷屏纪律：每个页面渲染函数（ui/card|menu|stats）在末尾恰好调用一次
 * ui_render_commit()（内部即一次 disp_push_partial 整屏），
 * 满足"每次画面更新只调用一次 disp_push_partial"的硬性交互约束。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "display.h"
#include "font.h"

/* ===== 灰阶常量（4bpp，display.h 约定 0x00 黑 … 0x0F 白） ===== */
#define UI_GRAY_BLACK   0x00
#define UI_GRAY_DARK    0x03
#define UI_GRAY_MID     0x08
#define UI_GRAY_LIGHT   0x0C
#define UI_GRAY_WHITE   0x0F

/* ===== 通用：标题栏与右上返回区（菜单/设置/统计共用布局） ===== */
#define HIT_HEADER_H        96
#define HIT_BACK_X0         420   /* 右上 [返回] 按钮区 */
#define HIT_BACK_X1         DISP_WIDTH
#define HIT_BACK_Y0         0
#define HIT_BACK_Y1         HIT_HEADER_H

/* ===== 卡片页（模式 A） ===== */
/* 背面底部三自评按钮区：y>=800，横向三等分 [忘记][模糊][认识] */
#define HIT_CARD_BTN_Y0     800
#define HIT_CARD_BTN_H      (DISP_HEIGHT - HIT_CARD_BTN_Y0)   /* 160 */
#define HIT_CARD_BTN_COUNT  3
#define HIT_CARD_BTN_W      (DISP_WIDTH / HIT_CARD_BTN_COUNT) /* 180 */
#define HIT_CARD_IDX_FORGOT 0
#define HIT_CARD_IDX_BLURRY 1
#define HIT_CARD_IDX_KNOWN  2
/* 正面：整屏（含按钮区位置）任意按下 = 翻面，无独立控件 */

/* ===== 模式 B 选项区：y=560 起，每行高 88，行距 12 ===== */
#define HIT_QUIZ_OPT_Y0     560
#define HIT_QUIZ_OPT_H      88
#define HIT_QUIZ_OPT_GAP    12
#define HIT_QUIZ_OPT_STEP   (HIT_QUIZ_OPT_H + HIT_QUIZ_OPT_GAP)
#define HIT_QUIZ_OPT_MAX    4
#define HIT_QUIZ_OPT_X0     40
#define HIT_QUIZ_OPT_X1     (DISP_WIDTH - 40)
/* 反馈文字行（正确/正确答案提示） */
#define HIT_QUIZ_FEEDBACK_Y 480

/* ===== 菜单页 ===== */
#define HIT_MENU_ROW_Y0     HIT_HEADER_H
#define HIT_MENU_ROW_H      96
#define HIT_MENU_PAGE_ROWS  8     /* (864-96)/96：单页最多 8 行词书 */
#define HIT_MENU_LIST_Y1    (HIT_MENU_ROW_Y0 + HIT_MENU_PAGE_ROWS * HIT_MENU_ROW_H) /* 864 */
#define HIT_MENU_PWR_X0     170   /* 标题栏 [关机]（屏幕关机键，物理键为硬件复位不可拦截） */
#define HIT_MENU_PWR_X1     290
#define HIT_MENU_STATS_X0   300   /* 标题栏 [统计] */
#define HIT_MENU_STATS_X1   420
#define HIT_MENU_SET_X0     420   /* 标题栏 [设置] */
#define HIT_MENU_SET_X1     DISP_WIDTH
#define HIT_MENU_BOTTOM_Y0  HIT_MENU_LIST_Y1   /* 底部翻页栏 */
#define HIT_MENU_PREV_X0    0                    /* 底部左半 [上一页] */
#define HIT_MENU_PREV_X1    (DISP_WIDTH / 2)
#define HIT_MENU_NEXT_X0    (DISP_WIDTH / 2)     /* 底部右半 [下一页] */
#define HIT_MENU_NEXT_X1    DISP_WIDTH

/* ===== 卡片页（模式 A 正面 / 模式 B）：左上角退出 + 顶部进度行 ===== */
#define HIT_CARD_EXIT_X0    8
#define HIT_CARD_EXIT_Y0    12
#define HIT_CARD_EXIT_W     104
#define HIT_CARD_EXIT_H     64
#define HIT_CARD_EXIT_X1    (HIT_CARD_EXIT_X0 + HIT_CARD_EXIT_W)
#define HIT_CARD_EXIT_Y1    (HIT_CARD_EXIT_Y0 + HIT_CARD_EXIT_H)
#define CARD_HEADER_Y       92    /* "今日待学 x/x · 今日待复习 x/x" 行顶 */

/* ===== 退出确认弹窗（覆盖在当前页面上，不清屏） ===== */
#define HIT_POPUP_X0        70
#define HIT_POPUP_Y0        330
#define HIT_POPUP_W         400
#define HIT_POPUP_H         260
#define HIT_POPUP_BTN_H     64
#define HIT_POPUP_BTN_Y0    (HIT_POPUP_Y0 + HIT_POPUP_H - HIT_POPUP_BTN_H - 24)
#define HIT_POPUP_BTN_GAP   24
#define HIT_POPUP_BTN_W     ((HIT_POPUP_W - 2 * 32 - HIT_POPUP_BTN_GAP) / 2)
#define HIT_POPUP_CONTINUE_X0 (HIT_POPUP_X0 + 32)                       /* 左：[继续] */
#define HIT_POPUP_CONTINUE_X1 (HIT_POPUP_CONTINUE_X0 + HIT_POPUP_BTN_W)
#define HIT_POPUP_EXIT_X0   (HIT_POPUP_CONTINUE_X1 + HIT_POPUP_BTN_GAP) /* 右：[退出] */
#define HIT_POPUP_EXIT_X1   (HIT_POPUP_EXIT_X0 + HIT_POPUP_BTN_W)

/* ===== 设置页（行高 96，从 y=120 起；行号见下行） ===== */
#define HIT_SET_ROW_Y0      120
#define HIT_SET_ROW_H       96
#define HIT_SET_ROW_DAILY   0     /* 每日新词上限：[-] 值 [+] */
#define HIT_SET_ROW_MODE    1     /* 默认模式：[模式A] [模式B] */
#define HIT_SET_ROW_TIME    2     /* 时间设置：[-1h] [+1h] [-1m] [+1m] */
#define HIT_SET_ROW_WORD    3     /* 单词字号：[-] 值 [+]（1.0x~4.0x 步进 0.5x） */
#define HIT_SET_ROW_COUNT   4
#define HIT_SET_BACK_X0     HIT_BACK_X0
#define HIT_SET_BACK_Y1     HIT_BACK_Y1
/* 新词行按钮 */
#define HIT_SET_MINUS_X0    280
#define HIT_SET_MINUS_X1    390
#define HIT_SET_PLUS_X0     450
#define HIT_SET_PLUS_X1     DISP_WIDTH
/* 模式行按钮 */
#define HIT_SET_MODE_A_X0   280
#define HIT_SET_MODE_A_X1   420
#define HIT_SET_MODE_B_X0   420
#define HIT_SET_MODE_B_X1   DISP_WIDTH
/* 时间行按钮（行内下半区：相对行顶偏移 40，高 48） */
#define HIT_SET_TIME_BTN_DY 40
#define HIT_SET_TIME_BTN_H  48
#define HIT_SET_TIME_HM_X0  40
#define HIT_SET_TIME_HP_X0  165
#define HIT_SET_TIME_MM_X0  290
#define HIT_SET_TIME_MP_X0  415
#define HIT_SET_TIME_BTN_W  110

/* ===== 统计页 ===== */
#define HIT_STATS_BACK_X0   HIT_BACK_X0
#define HIT_STATS_BACK_Y1   HIT_BACK_Y1
#define HIT_STATS_DIFF_X0   40    /* 生词本入口按钮 */
#define HIT_STATS_DIFF_X1   (DISP_WIDTH - 40)
#define HIT_STATS_DIFF_Y0   780
#define HIT_STATS_DIFF_Y1   880

/* ===== 绘图助手（只写帧缓冲；commit 才触发刷屏） ===== */

/* 初始化：登记帧缓冲与字体；boot 时调用一次 */
void ui_render_init(moka_font_t *font_cn, moka_font_t *font_ipa);

/* 字体句柄访问（IPA 可为 NULL = 音标行降级隐藏，NFR-5） */
moka_font_t *ui_render_font_cn(void);
moka_font_t *ui_render_font_ipa(void);

void ui_render_clear(void);                                  /* 清屏为白 */
void ui_render_fill_rect(int x, int y, int w, int h, uint8_t gray);
void ui_render_invert_rect(int x, int y, int w, int h);      /* 反色：gray ^= 0x0F */
void ui_render_hline(int x, int y, int w, uint8_t gray);
void ui_render_vline(int x, int y, int h, uint8_t gray);
void ui_render_rect_border(int x, int y, int w, int h, uint8_t gray);

/* 文本：font 为 NULL 或 text 为空时安全跳过并返回 0（NFR-5 降级） */
int  ui_render_text(moka_font_t *f, int x, int y, const char *utf8);
int  ui_render_text_cn(int x, int y, const char *utf8);       /* 内置中文字体 */
int  ui_render_text_cn_center(int cx, int y, const char *utf8);

/* 居中文本（超宽自动换行，为 38px 大字库下的溢出兜底）：
 * 整行宽度超过 max_w 时，优先在 sep 处断成两行（sep 可多字节，如 "·"，
 * 传 NULL 则按码点折半），每行独立居中，行距=字高+8；返回末行底 y。 */
int  ui_render_text_cn_center_wrap(int cx, int y, int max_w, const char *utf8,
                                   const char *sep);

/* 中文字体双线性平滑放大绘制（单词大字/关机页等）；
 * scale_x2 = 2×倍率（2=1.0x、3=1.5x、4=2.0x … 8=4.0x），返回放大后推进宽度 */
int  ui_render_text_cn_smooth(int x, int y, const char *utf8, int scale_x2);
/* 中文字体精确测宽（原生 1x 推进宽度；缺字形计 0） */
int  ui_render_text_cn_width(const char *utf8);

/* 按估算宽度自动换行绘制；返回结束行底 y（不含 SOH/STX 处理，见 ui/card.cpp） */
int  ui_render_wrapped(moka_font_t *f, int x, int y, int max_w, const char *utf8);

/* 按钮：边框 + 居中文字；invert=true 时整框反色（黑底白字） */
void ui_render_button(int x, int y, int w, int h, const char *label, bool invert);

/* 画面提交：恰好一次 disp_push_partial 整屏（所有页面渲染末尾调用） */
void ui_render_commit(void);
