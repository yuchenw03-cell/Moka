/**
 * wordbook.h —— 多词书管理：扫描词书目录下的 .wbx 文件，打开/关闭当前词书，
 * 惰性计算菜单概要（总词数/已学/待复习）。
 */
#pragma once

#include "app_internal.h"

/* 扫描词书目录，填充 g_app.books；返回词书数量，<0 为目录打开失败 */
int  wordbook_scan(void);
void wordbook_free(void);

/* 打开第 idx 本词书：wbx_open + srs 恢复/加载 + 补全新词默认记录 */
int  wordbook_open_current(int idx);

/* 计算并缓存第 idx 本的 已学/待复习 概要（读取其 .srs，结束后释放） */
int  wordbook_compute_summary(int idx);

/* 复习会话改变进度后调用：使概要缓存失效，下次进菜单重算 */
void wordbook_invalidate_summary(int idx);
