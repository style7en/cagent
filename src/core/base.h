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
#include <stdarg.h>
#include <ctype.h>
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
/* 角色决定前端用什么颜色/字体/段落格式渲染这段文字。
 * 前三个是"谁在说话", 后五个是"这是对话里的哪一类过程信息" —— 分开是为了让界面
 * 能把思考、调用、结果、错误各自成块, 而不是糊成同一种灰。
 * **新增角色要同步 ui/state.h 的配色表与 ui/helpers.h 的排版表**, 否则会落到默认样式。 */
#define CAGENT_ROLE_SYS    0  /* 未分类的系统提示 (兜底; 新代码请挑下面更具体的角色) */
#define CAGENT_ROLE_USER   1  /* 用户输入 */
#define CAGENT_ROLE_AI     2  /* AI 正文 (流式) */
#define CAGENT_ROLE_NOTE   3  /* 过程说明: 思考中/重试/压缩/取消 等旁白 */
#define CAGENT_ROLE_TOOL   4  /* 工具调用的标题行 (工具名) */
#define CAGENT_ROLE_ARGS   5  /* 工具调用的参数行 (等宽, 缩进在标题之下) */
#define CAGENT_ROLE_OUTPUT 6  /* 工具输出正文 (等宽, 带底色的块) */
#define CAGENT_ROLE_ERR    7  /* 失败/回滚/被拒绝 —— 需要用户注意 */
#define CAGENT_ROLE_THINK      8  /* 推理模型的思考过程正文 (可折叠块的正文) */
#define CAGENT_ROLE_THINK_HEAD 9  /* 思考块的标题行 —— 折叠块的抓手, 也是点击切换的目标 */
/* 角色总数。**新增角色时把它改成 (新角色 + 1)**, 前端的样式表按这个长度声明,
 * 漏改会在编译期报"数组初始化元素过多/过少", 比运行时越界好得多。 */
#define CAGENT_ROLE_COUNT  (CAGENT_ROLE_THINK_HEAD + 1)

/* 折叠块的标题行 —— core 与界面之间的**契约**: 前端靠它认出可折叠块 (见 ui/helpers.h 的
 * ui_fold_*), 改文案必须同步改前端的识别逻辑。
 *
 * 首字符是折叠状态符号, 界面切换时只替换这**一个字符**: ▾(U+25BE) 与 ▸(U+25B8) 都是
 * 单个 UTF-16 码元, 替换后标题行长度不变 —— 否则正文的字符位置会跟着漂移, 而折叠块的
 * 位置索引表全靠字符位置定位。界面侧的宽字符版本见 ui/helpers.h 的 UI_FOLD_GLYPH_*。 */
#define CAGENT_FOLD_GLYPH_OPEN  "▾"   /* 展开态 (内容可见) */
#define CAGENT_FOLD_GLYPH_SHUT  "▸"   /* 收起态 */

#define CAGENT_THINK_TITLE           CAGENT_FOLD_GLYPH_OPEN " 思考过程"
#define CAGENT_THINK_TITLE_COLLAPSED CAGENT_FOLD_GLYPH_SHUT " 思考过程"
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

/* 状态栏: 报告"模型此刻在干什么" (等待响应 / 生成中 / 执行工具 / 重试中)。
 * 这些信息以前是直接往输出区里打旁白的, 但它们属于**此刻的状态**而不是**对话内容** ——
 * 混在对话里既占地方, 又会跟着历史一起被保存和回放。text 传 NULL 表示回到空闲。 */
static void (*cagent_status)(const char *text) = NULL;
static void set_status(const char *text) { if (cagent_status) cagent_status(text); }

/* 安全的输出封装: 前端只需提供 cagent_emit。
 * 各语义化变体只是省去调用处写角色常量 —— 核心代码里一律用这些函数, 别直接调 cagent_emit,
 * 否则新增角色时容易漏掉某个调用点(它会悄悄退回灰色)。
 * 名字用 emit_ 前缀而不是 append_: core/exec.h 里已有一个 append_note(output, pos, cap, note),
 * 它是"往工具输出缓冲里追加说明"的另一个东西, 同名会直接编译不过。 */
static void emit_role(const char *utf8, int role) {
    if (cagent_emit) cagent_emit(utf8, role);
}
/* SYS 是"未分类"的兜底角色, 目前所有调用点都已归入下面的具体角色 —— 留着它是给
 * 将来新增的提示用, 免得又有人随手挑一个语义不符的角色把界面弄花。 */
CAGENT_MAYBE_UNUSED static void emit_text(const char *utf8) { emit_role(utf8, CAGENT_ROLE_SYS); }
static void emit_note(const char *utf8)   { emit_role(utf8, CAGENT_ROLE_NOTE); }
static void emit_think(const char *utf8)  { emit_role(utf8, CAGENT_ROLE_THINK); }
static void emit_think_head(const char *utf8) { emit_role(utf8, CAGENT_ROLE_THINK_HEAD); }
static void emit_tool(const char *utf8)   { emit_role(utf8, CAGENT_ROLE_TOOL); }
static void emit_args(const char *utf8)   { emit_role(utf8, CAGENT_ROLE_ARGS); }
static void emit_output(const char *utf8) { emit_role(utf8, CAGENT_ROLE_OUTPUT); }
static void emit_err(const char *utf8)    { emit_role(utf8, CAGENT_ROLE_ERR); }
