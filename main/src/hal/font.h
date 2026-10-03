/**
 * font.h —— EDC Book / M5ReadPaper 系 bin 字体（NFR-5）
 * 同时支持两代磁盘格式：v2（134B 头 + 1-bit 位图）与旧 EDC（5B 头 +
 * Huffman 灰度位图），判别与字段依据见 font.c 文件头（已用真实
 * font 分区数据验证）。
 * 双来源：font 分区（内置中文 43511 字 30px）/ SD 卡 font 目录下 .bin（IPA 音标等）。
 * 加载失败返回 NULL——上层必须降级为不显示该文本行，禁止乱码（NFR-5）。
 */
#ifndef MOKA_HAL_FONT_H
#define MOKA_HAL_FONT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct moka_font moka_font_t;   /* 不透明句柄 */

moka_font_t *font_open_partition(const char *label);  /* 如 "font" */
moka_font_t *font_open_sd(const char *path);          /* 如 "/sdcard/font/ipa.bin" */
void         font_close(moka_font_t *f);

int font_height(const moka_font_t *f);

/* 在 4bpp 帧缓冲上绘制 UTF-8 文本；返回推进像素宽度；缺字形自动跳过（不算失败） */
int font_draw_text(moka_font_t *f, uint8_t *fb4bpp, int fb_width, int fb_height,
                   int x, int y, const char *utf8_text);

/* 锐化双线性放大绘制（单词大字/关机页等）：插值消除阶梯，S 形曲线锐化
 * 防发虚（强度见 font.c FONT_SMOOTH_GAIN_X100）。scale_x2 = 2×放大倍率
 * （2=1.0x，3=1.5x … 8=4.0x，上限 16=8.0x），返回放大后的推进宽度。
 * scale_x2==2 时走 1x 直绘快路径。 */
int font_draw_text_smooth(moka_font_t *f, uint8_t *fb4bpp, int fb_width, int fb_height,
                          int x, int y, const char *utf8_text, int scale_x2);

/* 只测宽不绘制：返回原生 1x 下的推进像素宽度（缺字形按 0 计） */
int font_text_width(moka_font_t *f, const char *utf8_text);

#ifdef __cplusplus
}
#endif
#endif
