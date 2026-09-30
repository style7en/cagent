/*
 * core/base.h - 平台头 / 角色常量 / 宿主钩子 / 输出封装
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ===== 宿主钩子 (由 GUI 前端实现) ===== */
#define CAGENT_ROLE_SYS   0   /* 系统/过程提示 (thinking/回滚/工具结果/载入提示) */
#define CAGENT_ROLE_USER  1   /* 用户输入 */
#define CAGENT_ROLE_AI    2   /* AI 输出 */
static void (*cagent_emit)(const char *utf8, int role) = NULL;  /* 输出一段文本 (role 区分角色) */
static void (*cagent_on_done)(void) = NULL;                   /* 一轮 Agent 结束 */
static void (*cagent_read_config_ui)(void) = NULL;            /* 从 UI 同步配置到全局 */

/* 安全的输出封装: 前端只需提供 cagent_emit。默认按 SYS 角色输出。 */
static void append_text(const char *utf8) {
    if (cagent_emit) cagent_emit(utf8, CAGENT_ROLE_SYS);
}
