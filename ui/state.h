/*
 * ui/state.h - 控件 ID / 自定义消息 / 全局句柄与字体
 *
 * cagent 界面层的一部分, 由 cagent.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

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
#define ID_SESS_BTN 1015   /* 加载会话按钮 */
#define ID_SESS_LB  2001   /* 会话列表 ListBox */
#define ID_SESS_OK  2002   /* 载入 */
#define ID_SESS_CAN 2003   /* 取消 */

#define WM_APP_APPEND  (WM_APP + 1)   /* wParam = role(int), lParam = UTF-8 char* (须 free) */

#define CFG_URL 0
#define CFG_KEY 1
#define CFG_MDL 2

/* 前向声明 (实现见 UI 辅助区) */
static void read_edit_utf8(HWND h, char *out, size_t cap);
static void set_edit_utf8(HWND h, const char *utf8);

/* ===== GUI 状态 ===== */
static HWND g_hHistory, g_hInput, g_hSend, g_hNew, g_hSess;
static void show_session_dialog(HWND owner);
static HWND g_hCfg[3];                 /* [url, key, model] */
static HWND g_hWorkspace, g_hWsBrowse; /* 工作目录 Edit + 浏览按钮 */
static HFONT g_hFont;
static HFONT g_hFontHist;
static HANDLE g_hThread = NULL;
static WNDPROC g_oldInputProc;
static WNDPROC g_oldHistoryProc;
