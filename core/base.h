/*
 * core/base.h - 平台头 / 角色常量 / 宿主钩子 / 输出封装
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* 核心只依赖: 基础窗口类型 / WinHTTP / WinCrypt / C 运行时 */
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ===== 编译辅助 ===== */
/* 单编译单元下, 各 static 函数会被所有前端(GUI / 控制台测试)一起编译,
   而 GUI 专属的入口(如 config_load、agent_thread)在测试二进制里无人调用,
   会触发 -Wunused-function。用本宏显式声明"允许在此前端不被使用",
   其余真正没人用的函数仍会正常告警。 */
#if defined(__GNUC__) || defined(__clang__)
#define CAGENT_MAYBE_UNUSED __attribute__((unused))
#else
#define CAGENT_MAYBE_UNUSED
#endif

/* ===== 宿主钩子 (由 GUI 前端实现) ===== */
#define CAGENT_ROLE_SYS   0   /* 系统/过程提示 (thinking/回滚/工具结果/载入提示) */
#define CAGENT_ROLE_USER  1   /* 用户输入 */
#define CAGENT_ROLE_AI    2   /* AI 输出 */
static void (*cagent_emit)(const char *utf8, int role) = NULL;  /* 输出一段文本 (role 区分角色) */
static void (*cagent_on_done)(void) = NULL;                   /* 一轮 Agent 结束 */
static void (*cagent_read_config_ui)(void) = NULL;            /* 从 UI 同步配置到全局 */

/* 流式分段控制: 每次发起流式请求前调 begin 标记新分段;
 * 请求失败重试前调 undo 回删本分段已上屏的内容 —— 否则重试会把半截回复和完整回复
 * 拼在一起, 界面显示的和实际写进 messages 的对不上。前端必须按调用顺序投递
 * (与 emit 一样走消息队列, 不能用跨线程 SendMessage, 否则会插队打乱顺序)。
 * 未设置钩子时安全降级: 只增不删。 */
static void (*cagent_stream_begin)(void) = NULL;
static void (*cagent_stream_undo)(void) = NULL;

/* 安全的输出封装: 前端只需提供 cagent_emit。默认按 SYS 角色输出。 */
static void append_text(const char *utf8) {
    if (cagent_emit) cagent_emit(utf8, CAGENT_ROLE_SYS);
}
