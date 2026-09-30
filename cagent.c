/*
 * cagent_gui.c - Win32 GUI 前端 (界面 + 宿主钩子)
 *
 * 共用 cagent_core.h 中的 Agent 循环 / 工具 / 网络 / JSON 解析。
 * 通过钩子把核心与界面解耦:
 *   cagent_emit            -> PostMessage 追加到历史框
 *   cagent_on_done         -> 一轮结束, 恢复 UI
 *   cagent_read_config_ui  -> 退出时从 Edit 同步配置到全局
 *
 * 编译: gcc -mwindows -o cagent_gui.exe cagent_gui.c -lcomctl32 -lwinhttp
 */

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include "cagent_core.h"

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

/* ===== 宿主钩子实现 ===== */

/* 输出: 复制到堆, 通过 PostMessage 投递到 UI 线程 (线程安全)。
 * wParam 携带角色 (CAGENT_ROLE_*), 供历史框按角色着色。 */
static void gui_emit(const char *utf8, int role) {
    char *copy = strdup(utf8);
    if (copy) PostMessageW(g_hHistory, WM_APP_APPEND, (WPARAM)role, (LPARAM)copy);
}

/* 一轮 Agent 结束: 恢复 UI。 */
static void gui_on_done(void) {
    InterlockedExchange(&g_running, 0);
    InterlockedExchange(&g_cancel, 0);
    SetWindowTextW(g_hSend, L"发送");
    EnableWindow(g_hSend, TRUE);
    EnableWindow(g_hInput, TRUE);
    SetFocus(g_hInput);
}

/* 退出时从 Edit 同步配置到全局。 */
static void gui_read_config_ui(void) {
    if (g_hCfg[CFG_URL]) read_edit_utf8(g_hCfg[CFG_URL], g_api_url, sizeof(g_api_url));
    if (g_hCfg[CFG_KEY]) read_edit_utf8(g_hCfg[CFG_KEY], g_api_key, sizeof(g_api_key));
    if (g_hCfg[CFG_MDL]) read_edit_utf8(g_hCfg[CFG_MDL], g_model,   sizeof(g_model));
    if (g_hWorkspace)     read_edit_utf8(g_hWorkspace, g_workspace, sizeof(g_workspace));
}

/* ===== UI 辅助 ===== */

static void read_edit_utf8(HWND h, char *out, size_t cap) {
    int wlen = GetWindowTextLengthW(h);
    if (wlen <= 0) return;
    WCHAR *wbuf = (WCHAR*)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    GetWindowTextW(h, wbuf, wlen + 1);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)cap, NULL, NULL);
    free(wbuf);
}

static void set_edit_utf8(HWND h, const char *utf8) {
    if (!utf8 || !*utf8) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return;
    WCHAR *wbuf = (WCHAR*)malloc((size_t)wlen * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wlen);
    SetWindowTextW(h, wbuf);
    free(wbuf);
}

/* 主线程处理: UTF-8 -> UTF-16, LF 补 CRLF, 按角色着色后追加到历史框末尾。
 * emoji 代理对区段显式用 Segoe UI Emoji 字体渲染 (YaHei 无字形, 字体绑定回退会渲染成 '?')。 */
static void do_append(const char *utf8, int role) {
    size_t in_len = strlen(utf8);
    char *norm = (char*)malloc(in_len * 2 + 1);
    if (!norm) return;
    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = utf8[i];
        if (c == '\n' && (i == 0 || utf8[i-1] != '\r')) {
            norm[j++] = '\r';
            norm[j++] = '\n';
        } else {
            norm[j++] = c;
        }
    }
    norm[j] = '\0';

    int wlen = MultiByteToWideChar(CP_UTF8, 0, norm, -1, NULL, 0);
    if (wlen <= 0) { free(norm); return; }
    WCHAR *wbuf = (WCHAR*)malloc((size_t)wlen * sizeof(WCHAR));
    if (!wbuf) { free(norm); return; }
    MultiByteToWideChar(CP_UTF8, 0, norm, -1, wbuf, wlen);
    free(norm);

    /* 当前插入点的字符格式 (字体/字号) 作为普通文本基底 */
    CHARFORMAT2W base;
    memset(&base, 0, sizeof(base));
    base.cbSize = sizeof(base);
    SendMessageW(g_hHistory, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&base);

    /* 按角色叠加颜色: 用户=蓝加粗, 系统提示=灰斜体, AI=默认黑 */
    CHARFORMAT2W role_cf = base;
    role_cf.dwMask |= CFM_COLOR;
    role_cf.crTextColor = RGB(0, 0, 0);            /* AI: 默认黑 */
    if (role == CAGENT_ROLE_USER) {
        role_cf.crTextColor = RGB(0, 90, 200);     /* 用户: 蓝 */
        role_cf.dwMask   |= CFM_BOLD;
        role_cf.dwEffects |= CFE_BOLD;
    } else if (role == CAGENT_ROLE_SYS) {
        role_cf.crTextColor = RGB(110, 110, 110);  /* 系统: 灰 */
        role_cf.dwMask   |= CFM_ITALIC;
        role_cf.dwEffects |= CFE_ITALIC;
    }

    /* 按 run 插入: 连续 emoji 代理对为一段(用 Segoe UI Emoji), 其余为普通段 */
    int total = lstrlenW(wbuf);
    int i = 0;
    while (i < total) {
        BOOL emo = (wbuf[i] >= 0xD800 && wbuf[i] <= 0xDBFF &&
                    wbuf[i+1] >= 0xDC00 && wbuf[i+1] <= 0xDFFF);
        int j = i;
        if (emo) {
            while (j + 1 < total && wbuf[j] >= 0xD800 && wbuf[j] <= 0xDBFF &&
                   wbuf[j+1] >= 0xDC00 && wbuf[j+1] <= 0xDFFF) j += 2;
        } else {
            while (j < total && !(wbuf[j] >= 0xD800 && wbuf[j] <= 0xDBFF &&
                                  j + 1 < total && wbuf[j+1] >= 0xDC00 && wbuf[j+1] <= 0xDFFF)) j++;
        }

        CHARFORMAT2W cf = role_cf;
        if (emo) {
            cf.dwMask |= CFM_FACE;
            wcscpy(cf.szFaceName, L"Segoe UI Emoji");
        }
        int len = GetWindowTextLengthW(g_hHistory);
        SendMessageW(g_hHistory, EM_SETSEL, len, len);
        SendMessageW(g_hHistory, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        { WCHAR save = wbuf[j]; wbuf[j] = L'\0';
          SendMessageW(g_hHistory, EM_REPLACESEL, FALSE, (LPARAM)(wbuf + i));
          wbuf[j] = save; }
        i = j;
    }
    SendMessageW(g_hHistory, EM_SCROLLCARET, 0, 0);
    SendMessageW(g_hHistory, WM_VSCROLL, SB_BOTTOM, 0);   /* 强制滚到末尾 */
    free(wbuf);
}

/* ===== 启动一轮 Agent (GUI) ===== */

static void start_task(HWND hwnd) {
    if (g_hThread) {
        WaitForSingleObject(g_hThread, INFINITE);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }

    int wlen = GetWindowTextLengthW(g_hInput);
    if (wlen <= 0) return;

    /* 同步配置 */
    read_edit_utf8(g_hCfg[CFG_URL], g_api_url, sizeof(g_api_url));
    read_edit_utf8(g_hCfg[CFG_KEY], g_api_key, sizeof(g_api_key));
    read_edit_utf8(g_hCfg[CFG_MDL], g_model,   sizeof(g_model));
    read_edit_utf8(g_hWorkspace, g_workspace, sizeof(g_workspace));
    if (g_active_ws[0] && strcmp(g_workspace, g_active_ws) != 0) {
        /* 工作目录已变更: 当前对话落盘保留, 另起新会话 (不自动加载历史) */
        if (strlen(messages) > strlen(SYSTEM_PROMPT)) history_save();
        reset_conversation();
        g_history_file[0] = '\0';
        append_text("(工作目录已变更, 已另起新会话; 旧会话可通过 [加载会话] 恢复)\r\n");
    }
    snprintf(g_active_ws, sizeof(g_active_ws), "%s", g_workspace);

    if (!g_api_url[0] || !g_api_key[0] || !g_model[0]) {
        MessageBoxW(hwnd, L"请填写 Url-Base、Key、Model 三项配置。",
                    L"配置不完整", MB_OK | MB_ICONWARNING);
        return;
    }

    wchar_t wws[MAX_PATH];
    if (utf8_to_wide(g_workspace, wws, MAX_PATH)) SetCurrentDirectoryW(wws);

    WCHAR *wbuf = (WCHAR*)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    GetWindowTextW(g_hInput, wbuf, wlen + 1);

    AgentTask *task = (AgentTask*)calloc(1, sizeof(AgentTask));
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, task->user_msg, BUFSZ, NULL, NULL);
    free(wbuf);

    SetWindowTextW(g_hInput, L"");

    /* 回显用户输入到历史框 (USER 角色: 蓝色加粗) */
    {
        size_t ul = strlen(task->user_msg);
        char *uecho = (char*)malloc(ul + 16);
        if (uecho) {
            snprintf(uecho, ul + 16, "\r\n你：%s\r\n", task->user_msg);
            gui_emit(uecho, CAGENT_ROLE_USER);
            free(uecho);
        }
    }

    InterlockedExchange(&g_running, 1);
    SetWindowTextW(g_hSend, L"停止");
    EnableWindow(g_hSend, TRUE);
    g_hThread = CreateThread(NULL, 0, agent_thread, task, 0, NULL);
}

/* ===== 布局 / 子类化 / 窗口过程 ===== */

static void layout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int gap = 8;
    int row_h = 26;
    int lbl_w = 80;
    int top_h = row_h * 4 + gap * 5;
    int btn_w = 80;
    int ws_btn_w = 72;
    int new_w = 80;
    int sess_w = 80;
    int input_h = 72;
    int input_y = H - input_h - gap;
    int input_w = W - btn_w - gap * 3;

    for (int i = 0; i < 3; i++) {
        int y = gap + i * (row_h + gap);
        HWND lbl = GetDlgItem(hwnd, ID_LBL_BASE + i);
        MoveWindow(lbl,        gap,         y + 4, lbl_w,               row_h, TRUE);
        MoveWindow(g_hCfg[i],  gap + lbl_w, y,     W - gap*2 - lbl_w,   row_h, TRUE);
    }
    {
        int y = gap + 3 * (row_h + gap);
        HWND lbl = GetDlgItem(hwnd, ID_LBL_WS);
        MoveWindow(lbl,           gap,         y + 4, lbl_w,             row_h, TRUE);
        MoveWindow(g_hWorkspace,  gap + lbl_w, y,     W - lbl_w - ws_btn_w - new_w - sess_w - gap*5, row_h, TRUE);
        MoveWindow(g_hWsBrowse,   W - gap - ws_btn_w - new_w - sess_w - gap*2, y, ws_btn_w, row_h, TRUE);
        MoveWindow(g_hNew,        W - gap - new_w - sess_w - gap, y, new_w,           row_h, TRUE);
        MoveWindow(g_hSess,       W - gap - sess_w, y, sess_w,             row_h, TRUE);
    }

    MoveWindow(g_hHistory, gap, top_h, W - gap * 2,
               H - top_h - input_h - gap * 2, TRUE);
    MoveWindow(g_hInput, gap, input_y, input_w, input_h, TRUE);
    MoveWindow(g_hSend,  gap * 2 + input_w, input_y, btn_w, input_h, TRUE);
}

static LRESULT CALLBACK InputProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            return CallWindowProcW(g_oldInputProc, h, msg, wp, lp);
        }
        SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(ID_SEND, BN_CLICKED), 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) {
        if (!(GetKeyState(VK_SHIFT) & 0x8000)) return 0;
    }
    return CallWindowProcW(g_oldInputProc, h, msg, wp, lp);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        LoadLibraryW(L"Msftedit.dll");   /* 注册 RICHEDIT50W 控件类 */

        g_hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, FF_DONTCARE, L"Microsoft YaHei UI");
        /* 输出区独立字体: 比控件/标签大一档, 阅读更醒目 */
        g_hFontHist = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, FF_DONTCARE, L"Microsoft YaHei UI");

        static const WCHAR *labels[3] = { L"Url-Base:", L"Key:", L"Model:" };
        static const DWORD ed_styles[3] = { 0, ES_PASSWORD, 0 };

        for (int i = 0; i < 3; i++) {
            HWND lbl = CreateWindowW(L"STATIC", labels[i],
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)(ID_LBL_BASE + i), NULL, NULL);
            SendMessageW(lbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            g_hCfg[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ed_styles[i],
                0, 0, 0, 0, hwnd,
                (HMENU)(LONG_PTR)(ID_CFG_BASE + i), NULL, NULL);
            SendMessageW(g_hCfg[i], WM_SETFONT, (WPARAM)g_hFont, TRUE);
        }
        SendMessageW(g_hCfg[CFG_KEY], EM_SETPASSWORDCHAR, (WPARAM)'*', 0);

        config_load();
        set_edit_utf8(g_hCfg[CFG_URL], g_api_url);
        set_edit_utf8(g_hCfg[CFG_KEY], g_api_key);
        set_edit_utf8(g_hCfg[CFG_MDL], g_model);

        {
            HWND lbl = CreateWindowW(L"STATIC", L"工作目录:",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                0,0,0,0, hwnd, (HMENU)(LONG_PTR)ID_LBL_WS, NULL, NULL);
            SendMessageW(lbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            g_hWorkspace = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                0,0,0,0, hwnd, (HMENU)(LONG_PTR)ID_WS_EDIT, NULL, NULL);
            SendMessageW(g_hWorkspace, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            g_hWsBrowse = CreateWindowW(L"BUTTON", L"浏览...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0,0,0,0, hwnd, (HMENU)(LONG_PTR)ID_WS_BTN, NULL, NULL);
            SendMessageW(g_hWsBrowse, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            ensure_workspace();
            set_edit_utf8(g_hWorkspace, g_workspace);
        }

        g_hHistory = CreateWindowExW(WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_HISTORY, NULL, NULL);
        SendMessageW(g_hHistory, WM_SETFONT, (WPARAM)g_hFontHist, TRUE);

        g_hInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_INPUT, NULL, NULL);
        SendMessageW(g_hInput, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_oldInputProc = (WNDPROC)SetWindowLongPtrW(g_hInput, GWLP_WNDPROC, (LONG_PTR)InputProc);

        g_hSend = CreateWindowW(L"BUTTON", L"发送",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_SEND, NULL, NULL);
        SendMessageW(g_hSend, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hNew = CreateWindowW(L"BUTTON", L"新建会话",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_NEW_BTN, NULL, NULL);
        SendMessageW(g_hNew, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hSess = CreateWindowW(L"BUTTON", L"加载会话",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_SESS_BTN, NULL, NULL);
        SendMessageW(g_hSess, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        {
            /* 启动不自动加载历史: 全新对话, 旧会话通过"加载会话"按钮按需载入 */
            reset_conversation();
            snprintf(g_active_ws, sizeof(g_active_ws), "%s", g_workspace);
        }

        if (g_key_decrypt_failed) {
            append_text("API Key 解密失败 (可能换了用户/机器), 请重新填写 Key。\r\n");
            g_key_decrypt_failed = 0;
        }

        SetFocus(g_hInput);
        return 0;
    }

    case WM_SIZE:
        layout(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_SEND && HIWORD(wp) == BN_CLICKED) {
            if (g_running) {
                InterlockedExchange(&g_cancel, 1);
                SetWindowTextW(g_hSend, L"停止中...");
                EnableWindow(g_hSend, FALSE);
                append_text("(正在停止, 需等当前工具执行完毕...)\r\n");
            } else {
                start_task(hwnd);
            }
            return 0;
        }
        if (LOWORD(wp) == ID_WS_BTN && HIWORD(wp) == BN_CLICKED) {
            IFileOpenDialog *pfd = NULL;
            if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC,
                                            &IID_IFileOpenDialog, (void**)&pfd))) {
                FILEOPENDIALOGOPTIONS opts = 0;
                pfd->lpVtbl->GetOptions(pfd, &opts);
                pfd->lpVtbl->SetOptions(pfd, opts | FOS_PICKFOLDERS);
                pfd->lpVtbl->SetTitle(pfd, L"选择工作目录");
                if (SUCCEEDED(pfd->lpVtbl->Show(pfd, hwnd))) {
                    IShellItem *psi = NULL;
                    if (SUCCEEDED(pfd->lpVtbl->GetResult(pfd, &psi)) && psi) {
                        PWSTR ppath = NULL;
                        if (SUCCEEDED(psi->lpVtbl->GetDisplayName(psi, SIGDN_FILESYSPATH, &ppath)) && ppath) {
                            char utf8[MAX_PATH];
                            WideCharToMultiByte(CP_UTF8, 0, ppath, -1, utf8, sizeof(utf8), NULL, NULL);
                            snprintf(g_workspace, sizeof(g_workspace), "%s", utf8);
                            set_edit_utf8(g_hWorkspace, g_workspace);
                            CoTaskMemFree(ppath);
                        }
                        psi->lpVtbl->Release(psi);
                    }
                }
                pfd->lpVtbl->Release(pfd);
            }
            return 0;
        }
        if (LOWORD(wp) == ID_NEW_BTN && HIWORD(wp) == BN_CLICKED) {
            if (!g_running) {
                /* 新建会话: 当前对话先落盘保留, 重开空对话 (不删除任何文件) */
                read_edit_utf8(g_hWorkspace, g_workspace, sizeof(g_workspace));
                snprintf(g_active_ws, sizeof(g_active_ws), "%s", g_workspace);
                history_start_new();
                SetWindowTextW(g_hHistory, L"");
                append_text("(新会话已创建。旧会话文件保留, 可通过 [加载会话] 恢复)\r\n");
                SetFocus(g_hInput);
            }
            return 0;
        }
        if (LOWORD(wp) == ID_SESS_BTN && HIWORD(wp) == BN_CLICKED) {
            if (!g_running) show_session_dialog(hwnd);
            return 0;
        }
        break;

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_DESTROY:
        config_save();
        if (g_hFont) DeleteObject(g_hFont);
        if (g_hFontHist) DeleteObject(g_hFontHist);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK HistoryProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP_APPEND) {
        char *txt = (char*)lp;
        do_append(txt, (int)wp);
        free(txt);
        return 0;
    }
    if (msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL ||
        msg == WM_KEYDOWN || msg == WM_KEYUP) {
        LRESULT r = CallWindowProcW(g_oldHistoryProc, h, msg, wp, lp);
        InvalidateRect(h, NULL, TRUE);
        UpdateWindow(h);
        return r;
    }
    return CallWindowProcW(g_oldHistoryProc, h, msg, wp, lp);
}

/* ===== DPI 适配 ===== */
static void enable_dpi_awareness(void) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) return;

    typedef DPI_AWARENESS_CONTEXT (WINAPI *PFN_SetCtx)(DPI_AWARENESS_CONTEXT);
    PFN_SetCtx pSetCtx = (PFN_SetCtx)(void*)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
    if (pSetCtx) {
        if (pSetCtx((DPI_AWARENESS_CONTEXT)-5)) return;
    }

    typedef HRESULT (WINAPI *PFN_SetAwareness)(int);
    PFN_SetAwareness pSetAwareness = (PFN_SetAwareness)(void*)GetProcAddress(hUser32, "SetProcessDpiAwareness");
    if (pSetAwareness) {
        pSetAwareness(1);
        return;
    }

    typedef BOOL (WINAPI *PFN_SetAware)(void);
    PFN_SetAware pSetAware = (PFN_SetAware)(void*)GetProcAddress(hUser32, "SetProcessDPIAware");
    if (pSetAware) pSetAware();
}

/* ===== 历史会话选择对话框 (非模态, 由主消息循环驱动) ===== */

static HWND g_sess_dlg   = NULL;
static HWND g_sess_owner = NULL;
static char g_sess_sel[MAX_PATH];
static char g_sess_paths[64][MAX_PATH];
static char g_sess_ws[64][200];      /* 第一行: 工作目录 */
static char g_sess_sub[64][300];     /* 第二行: N 条消息 · 时间 · 预览 */
static int  g_sess_n = 0;
static HFONT g_hFontBold = NULL;

/* 旧格式文件从文件名反推工作目录显示 (sanitized: '_' 大多为分隔符)。 */
static void ws_from_filename(const char *path, char *out, size_t cap) {
    const char *base = strrchr(path, '\\');
    if (!base) base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char fn[MAX_PATH];
    snprintf(fn, sizeof(fn), "%s", base);
    size_t len = strlen(fn);
    if (len > 5 && _stricmp(fn + len - 5, ".json") == 0) fn[len -= 5] = '\0';
    const char *p = fn;
    if (strncmp(p, "history_", 8) == 0) p += 8;
    len = strlen(p);
    const char *last = NULL;                 /* 去掉末尾 _HASH */
    for (size_t i = 0; i < len; i++) if (p[i] == '_') last = p + i;
    if (last) len = (size_t)(last - p);
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < cap; i++) {
        char ch = p[i];
        if (ch == '_') ch = (i == 1) ? ':' : '\\';
        out[o++] = ch;
    }
    out[o] = '\0';
}

/* 枚举 exe 目录下所有 history_*.json, 记录路径与元信息 (目录/条数/时间/预览)。 */
static int sess_enumerate(const char *exedir) {
    g_sess_n = 0;
    char pat[MAX_PATH];
    snprintf(pat, sizeof(pat), "%shistory_*.json", exedir);
    wchar_t wpat[MAX_PATH];
    if (!utf8_to_wide(pat, wpat, MAX_PATH)) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (g_sess_n >= 64) break;
        char name[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL);
        snprintf(g_sess_paths[g_sess_n], MAX_PATH, "%s%s", exedir, name);

        char ws[160], prev[200];
        int cnt = 0;
        session_read_meta(g_sess_paths[g_sess_n], ws, sizeof(ws), prev, sizeof(prev), &cnt);

        /* 修改时间 -> 本地 "MM-DD HH:MM" */
        FILETIME lft;
        SYSTEMTIME st, lst;
        FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lft);
        FileTimeToSystemTime(&lft, &st);
        SystemTimeToTzSpecificLocalTime(NULL, &st, &lst);
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d",
                 lst.wYear, lst.wMonth, lst.wDay, lst.wHour, lst.wMinute);

        /* 旧格式: 从文件名反推目录显示 */
        char wsdisp[180];
        if (ws[0]) snprintf(wsdisp, sizeof(wsdisp), "%s", ws);
        else {
            char derived[160];
            ws_from_filename(name, derived, sizeof(derived));
            snprintf(wsdisp, sizeof(wsdisp), "(旧格式) %s", derived);
        }

        snprintf(g_sess_ws[g_sess_n], 200, "%s", wsdisp);
        if (prev[0])
            snprintf(g_sess_sub[g_sess_n], 300, "%d 条消息 · %s · %s", cnt, tbuf, prev);
        else
            snprintf(g_sess_sub[g_sess_n], 300, "%d 条消息 · %s", cnt, tbuf);
        g_sess_n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return g_sess_n;
}

/* 应用选中的会话: 有实际对话才先保存 -> 载入选中文件 -> 同步 UI -> 回放。 */
static void sess_apply_selected(const char *path) {
    if (g_active_ws[0] && strlen(messages) > strlen(SYSTEM_PROMPT))
        history_save();          /* 空对话不落盘, 避免覆盖旧会话文件 */
    if (!history_load_from_file(path)) return;
    set_edit_utf8(g_hWorkspace, g_workspace);
    wchar_t wws[MAX_PATH];
    if (utf8_to_wide(g_workspace, wws, MAX_PATH))
        SetCurrentDirectoryW(wws);
    SetWindowTextW(g_hHistory, L"");
    append_text("(已载入历史会话, 工作目录: ");
    append_text(g_workspace);
    append_text(")\r\n");
    history_replay();
}

static LRESULT CALLBACK SessDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HWND lb = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
            12, 12, 496, 330, hwnd, (HMENU)(LONG_PTR)ID_SESS_LB, NULL, NULL);
        SendMessageW(lb, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        if (!g_hFontBold) {
            LOGFONTW lf;
            GetObjectW(g_hFont, sizeof(lf), &lf);
            lf.lfWeight = FW_BOLD;
            g_hFontBold = CreateFontIndirectW(&lf);
        }
        for (int i = 0; i < g_sess_n; i++)
            SendMessageW(lb, LB_ADDSTRING, 0, 0);
        if (g_sess_n > 0) SendMessageW(lb, LB_SETCURSEL, 0, 0);
        CreateWindowW(L"BUTTON", L"载入", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            300, 356, 100, 32, hwnd, (HMENU)(LONG_PTR)ID_SESS_OK, NULL, NULL);
        CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            412, 356, 100, 32, hwnd, (HMENU)(LONG_PTR)ID_SESS_CAN, NULL, NULL);
        SendMessageW(GetDlgItem(hwnd, ID_SESS_OK), WM_SETFONT, (WPARAM)g_hFont, TRUE);
        SendMessageW(GetDlgItem(hwnd, ID_SESS_CAN), WM_SETFONT, (WPARAM)g_hFont, TRUE);
        return 0;
    }
    case WM_MEASUREITEM:
        ((MEASUREITEMSTRUCT*)lp)->itemHeight = 56;
        return TRUE;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT*)lp;
        if (d->CtlID != ID_SESS_LB || d->itemID >= (DWORD)g_sess_n) return TRUE;
        int idx = (int)d->itemID;
        int sel = (d->itemState & ODS_SELECTED) != 0;
        FillRect(d->hDC, &d->rcItem,
                 GetSysColorBrush(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        SetBkMode(d->hDC, TRANSPARENT);
        /* 第一行: 工作目录 (加粗) */
        SetTextColor(d->hDC, sel ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                 : GetSysColor(COLOR_WINDOWTEXT));
        SelectObject(d->hDC, g_hFontBold);
        wchar_t w1[200];
        utf8_to_wide(g_sess_ws[idx], w1, 200);
        RECT r1 = d->rcItem; r1.left += 10; r1.top += 6; r1.bottom = r1.top + 20;
        DrawTextW(d->hDC, w1, -1, &r1, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        /* 第二行: 条数 · 时间 · 预览 (灰) */
        SetTextColor(d->hDC, sel ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                 : RGB(120, 120, 120));
        SelectObject(d->hDC, g_hFont);
        wchar_t w2[300];
        utf8_to_wide(g_sess_sub[idx], w2, 300);
        RECT r2 = d->rcItem; r2.left += 10; r2.top += 28;
        DrawTextW(d->hDC, w2, -1, &r2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (d->itemState & ODS_FOCUS) DrawFocusRect(d->hDC, &d->rcItem);
        return TRUE;
    }
    case WM_COMMAND: {
        if (LOWORD(wp) == ID_SESS_LB && HIWORD(wp) == LBN_DBLCLK) wp = MAKEWPARAM(ID_SESS_OK, BN_CLICKED);
        if (LOWORD(wp) == ID_SESS_OK && HIWORD(wp) == BN_CLICKED) {
            HWND lb = GetDlgItem(hwnd, ID_SESS_LB);
            LRESULT idx = SendMessageW(lb, LB_GETCURSEL, 0, 0);
            if (idx != LB_ERR && idx < g_sess_n) {
                snprintf(g_sess_sel, sizeof(g_sess_sel), "%s", g_sess_paths[idx]);
                sess_apply_selected(g_sess_sel);
            }
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == ID_SESS_CAN && HIWORD(wp) == BN_CLICKED) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        /* 非模态: 恢复主窗口可用, 清理句柄。主消息循环无需任何特殊处理。 */
        if (g_sess_owner) {
            EnableWindow(g_sess_owner, TRUE);
            SetForegroundWindow(g_sess_owner);
        }
        g_sess_dlg = NULL;
        g_sess_owner = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 弹出历史会话选择窗口 (非模态), 立即返回; 载入动作由对话框自身完成。 */
static void show_session_dialog(HWND owner) {
    if (g_sess_dlg) { SetForegroundWindow(g_sess_dlg); return; }  /* 已打开 */
    char exedir[MAX_PATH];
    get_app_path(exedir, sizeof(exedir), "");   /* exe 目录 (含结尾 \) */
    if (sess_enumerate(exedir) == 0) {
        MessageBoxW(owner, L"没有已保存的历史会话。", L"历史会话", MB_OK | MB_ICONINFORMATION);
        return;
    }

    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = SessDlgProc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"CAGENT_SESSDLG";
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        if (!RegisterClassW(&wc)) return;
        reg = 1;
    }

    g_sess_owner = owner;
    g_sess_dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"CAGENT_SESSDLG", L"历史会话",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 540, 450, owner, NULL,
        GetModuleHandleW(NULL), NULL);
    if (!g_sess_dlg) { g_sess_owner = NULL; return; }

    /* 居中于主窗口 */
    RECT ro, rd;
    GetWindowRect(owner, &ro);
    GetWindowRect(g_sess_dlg, &rd);
    int x = ro.left + ((ro.right - ro.left) - (rd.right - rd.left)) / 2;
    int y = ro.top  + ((ro.bottom - ro.top) - (rd.bottom - rd.top)) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    MoveWindow(g_sess_dlg, x, y, rd.right - rd.left, rd.bottom - rd.top, TRUE);

    EnableWindow(owner, FALSE);
    ShowWindow(g_sess_dlg, SW_SHOW);
    SetForegroundWindow(g_sess_dlg);
    /* 无消息循环: 消息由 WinMain 的主循环统一泵送, 销毁时 WM_DESTROY 恢复主窗口。 */
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev;
    if (cmd && strstr(cmd, "--selftest")) {
        freopen("selftest.txt", "w", stdout);
        return json_selftest();
    }

    /* 安装宿主钩子 (必须在 agent 运行前设置) */
    cagent_emit = gui_emit;
    cagent_on_done = gui_on_done;
    cagent_read_config_ui = gui_read_config_ui;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    enable_dpi_awareness();
    InitCommonControls();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));   /* app.rc: 1 ICON app.ico */
    wc.lpszClassName = L"CagentGuiWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(L"CagentGuiWnd", L"cagent",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 560,
        NULL, NULL, hInst, NULL);

    /* 标题栏/任务栏图标 (大 + 小) */
    SendMessageW(hwnd, WM_SETICON, ICON_BIG,
        (LPARAM)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                           GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
        (LPARAM)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                           GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    g_oldHistoryProc = (WNDPROC)SetWindowLongPtrW(g_hHistory, GWLP_WNDPROC, (LONG_PTR)HistoryProc);

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
