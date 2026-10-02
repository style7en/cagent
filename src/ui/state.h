/*
 * ui/state.h - 控件 ID / 自定义消息 / 全局句柄与字体
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* 界面层额外依赖: Shell/COM (选目录对话框) */
#include <shlobj.h>
#include <shobjidl.h>

/* ===== 控件 ID ===== */
#define ID_HISTORY  1001
#define ID_INPUT    1002
#define ID_SEND     1003
#define ID_NEW_BTN  1004   /* 新建会话按钮 */
#define ID_CFG_BASE 1005   /* +0 url, +1 key, +2 model */
#define ID_LBL_BASE 1008   /* +0 url, +1 key, +2 model */
#define ID_LBL_WS   1011   /* 工作目录标签 */
#define ID_WS_EDIT  1012   /* 工作目录输入框 */
#define ID_WS_BTN   1013   /* 浏览按钮 */
#define ID_SESS_BTN 1014   /* 加载会话按钮 */
#define ID_SESS_LB  2001   /* 会话列表 ListBox */
#define ID_SESS_OK  2002   /* 载入 */
#define ID_SESS_CAN 2003   /* 取消 */

/* ===== 系统菜单命令 (WM_SYSCOMMAND) =====
 * 「关于」挂在窗口的系统菜单上 —— 主窗口没有菜单栏, 而标题栏问号按钮(WS_EX_CONTEXTHELP)
 * 与最小化/最大化互斥(实测加了它问号根本不画), 所以系统菜单是这里放"关于"的标准位置:
 * 点标题栏图标 / Alt+Space / 右键标题栏都能到。
 *
 * 取值有两处硬约束:
 *   ① 必须避开系统预定义命令 —— SC_* 全在 0xF000 以上
 *      (SC_SIZE=0xF000 … SC_CONTEXTHELP=0xF180, 另有 SC_SEPARATOR=0xF00F)。
 *   ② **必须是 16 的倍数。** WM_SYSCOMMAND 的低 4 位被系统占用, 分发时要 `wp & 0xFFF0`,
 *      若 ID 自身低 4 位非 0 (如 900 = 0x384), 掩码会把它削成别的值 —— 分支永远不命中。
 *      GCC 会以 -Wtautological-compare 报出来, 这正是本例踩过的坑。 */
#define IDM_ABOUT   0x0100

#define WM_APP_APPEND  (WM_APP + 1)   /* wParam = role(int), lParam = UTF-8 char* (须 free) */
#define WM_APP_STREAM  (WM_APP + 2)   /* wParam = 1: 新分段开始(清零计数); 0: 回删本分段 AI 文本
                                       * 必须与 APP_APPEND 一样按 FIFO 投递, 不能跨线程 SendMessage */

#define CFG_URL 0
#define CFG_KEY 1
#define CFG_MDL 2

/* 前向声明 (实现见 UI 辅助区) */
static void read_edit_utf8(HWND h, char *out, size_t cap);
static void set_edit_utf8(HWND h, const char *utf8);

/* ===== GUI 状态 ===== */
static HWND g_hHistory, g_hInput, g_hSend, g_hNew, g_hSess;
static void show_session_dialog(HWND owner);
static void show_about(HWND owner);
static HWND g_hCfg[3];                 /* [url, key, model] */
static HWND g_hWorkspace, g_hWsBrowse; /* 工作目录 Edit + 浏览按钮 */
static HFONT g_hFont;
static HFONT g_hFontHist;
static long g_stream_chars;      /* 当前分段已上屏的 AI 文本字符数(UTF-16), 供重试时回删 */
static HANDLE g_hThread = NULL;
static WNDPROC g_oldInputProc;
static WNDPROC g_oldHistoryProc;
