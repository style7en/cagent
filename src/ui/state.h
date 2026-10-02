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

/* ===== UI 配色 =====
 * 输出区(历史框)底色比输入框略深一档, 让"只读的输出"与"可编辑的输入"一眼能分层。
 * 输入框不设色, 保持系统默认的 COLOR_WINDOW(白)。
 *
 * 247 是刻意选的: 与白差 8 级(约 3%) —— 并排能看出分层, 单独看又不会觉得"那块发灰";
 * 再深就接近窗口自身的 COLOR_BTNFACE(240), 输出区会和背景糊在一起, 层次反而没了。
 * 想整体调深浅只改这一个数。 */
#define HIST_BG RGB(247, 247, 247)

/* ===== 输出区字号 (Ctrl+滚轮缩放) =====
 * 单位是 96 DPI 的**逻辑像素**, 表示"等效字号"。实现方式是 `EM_SETZOOM(当前值, 默认值)`,
 * 即整体缩放控件的显示 —— **不去改字符格式**, 因此行距会跟着等比缩放, 也不会出现
 * "run 字号被 WM_SETFONT 冲掉"那类跨屏跳变(详见 wndproc.h 的 ui_history_zoom)。
 * 上下限的理由: 12 约合 9pt, 再小汉字就难认了; 40 已是默认值的 2.2 倍, 一行放不下几个字。 */
#define HIST_LPX_DEF 18
#define HIST_LPX_MIN 12
#define HIST_LPX_MAX 40

/* 输出区行距在"字身格高"之外再留的余量(逻辑像素)。
 * 留 0 会让精确行距正好等于字身格高, 而实测 CJK 的字形外框比字身格还高约 2px(36px 字号时;
 * 带【】、重音等越界字形更明显) —— 字的顶边就会被行盒削掉(用户报过"字的顶都被削掉了")。
 * 留 1 逻辑像素即可消除削顶, 视觉上只比原来松约 5%。 */
#define HIST_LINE_EXTRA 1

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
static int g_hist_lpx = HIST_LPX_DEF;   /* 输出区字号(逻辑像素), Ctrl+滚轮可调 */
static int g_hist_zoom_acc;             /* Ctrl+滚轮的累积残量, 给高精度滚轮/触控板用 */
static long g_stream_chars;      /* 当前分段已上屏的 AI 文本字符数(UTF-16), 供重试时回删 */
static HANDLE g_hThread = NULL;
static WNDPROC g_oldInputProc;
static WNDPROC g_oldHistoryProc;
