/**
 * card.h —— 卡片页渲染（模式 A 正/背面、模式 B 选择题、通用提示页）
 * 所有函数只接收数据、画帧缓冲，末尾恰好一次 ui_render_commit。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 模式 A 正面：单词 + 音标 + 词性 + 顶部进度行 + 左上角退出按钮 */
typedef struct {
    const char *word;      /* 必有 */
    const char *phon;      /* NULL = 隐藏音标行（phon_len==0 或 IPA 字体缺失，NFR-5） */
    const char *pos_text;  /* 词性中文（如 "形容词"），NULL 不显示 */
    const char *header;    /* 顶部进度行（"今日待学 x/x · 今日待复习 x/x"），NULL 不显示 */
    uint8_t     word_scale2; /* 单词字号 ×2（2=1.0x … 8=4.0x），0/越界按 2 处理 */
} ui_card_front_t;

/* 模式 A 背面：释义 + 例句（SOH..STX 反色高亮）+ 底部三自评按钮 */
typedef struct {
    const char *gloss;     /* 必有 */
    const char *example;   /* NULL 或空串 = 无例句 */
    const char *btn_labels[3]; /* [忘记][模糊][认识] */
} ui_card_back_t;

/* 模式 B 选择题面：释义 + N 个选项（2~4，干扰项不足时降级少选项） */
typedef struct {
    const char *gloss;
    const char *options[4];
    int         count;         /* 2..4 */
    int         highlight;     /* -1 无；否则该选项反色（选对=选中项，选错=正确项） */
    const char *feedback;      /* NULL 无；否则在选项区上方显示（如 "回答正确"） */
    const char *header;        /* 顶部进度行，NULL 不显示 */
} ui_card_quiz_t;

void ui_card_front(const ui_card_front_t *d);
void ui_card_back(const ui_card_back_t *d);
void ui_card_quiz(const ui_card_quiz_t *d);

/* 退出确认弹窗：覆盖绘制在当前页面上（不清屏），恰好一次 commit。
 * learned_today = 今日已学词数（弹窗文案 "今天已学习 X 个单词，要退出吗"）。
 * 命中区域：HIT_POPUP_*（render.h），左 [继续] 右 [退出]。 */
void ui_card_exit_confirm(uint32_t learned_today);

/* 通用提示页（今日完成/空队列/致命错误）：两行居中文字 */
void ui_card_message(const char *line1, const char *line2);
