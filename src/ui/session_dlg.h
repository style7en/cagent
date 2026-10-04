/*
 * ui/session_dlg.h - 会话选择对话框: 枚举 / 自绘列表 / 载入
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== 历史会话选择对话框 (非模态, 由主消息循环驱动) ===== */

static HWND g_sess_dlg   = NULL;
static HWND g_sess_owner = NULL;
static char g_sess_paths[64][PATHSZ];   /* 会话目录 + 文件名, 见 state.h 的 PATHSZ 说明 */
static char g_sess_ws[64][200];      /* 第一行: 工作目录 */
static char g_sess_sub[64][300];     /* 第二行: N 条消息 · 时间 · 预览 */
static int  g_sess_n = 0;

/* 对话框自己的 DPI 与字体 —— 同 about.h: 它可能被拖到与主窗口不同缩放比例的显示器上,
 * 且**绝不能写全局 g_dpi**(那会让主窗口下次 layout() 用错比例)。 */
static UINT  g_sess_dpi = 96;
static UINT  g_sess_font_dpi = 0;
static HFONT g_sessFont     = NULL;
static HFONT g_sessFontBold = NULL;

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

/* 枚举会话目录 (sessions\) 下所有 history_*.json, 记录路径与元信息 (目录/条数/时间/预览)。
 * dir 需以 \ 结尾。 */
static int sess_enumerate(const char *dir) {
    g_sess_n = 0;
    char pat[PATHSZ];
    snprintf(pat, sizeof(pat), "%shistory_*.json", dir);
    wchar_t wpat[PATHSZ];
    if (!utf8_to_wide(pat, wpat, (int)sizeof(wpat))) return 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (g_sess_n >= 64) break;
        char name[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL);
        path_copy(g_sess_paths[g_sess_n], sizeof(g_sess_paths[g_sess_n]), dir);
        path_append(g_sess_paths[g_sess_n], sizeof(g_sess_paths[g_sess_n]), name);

        char ws[160], prev[200];
        int cnt = 0;
        session_read_meta(g_sess_paths[g_sess_n], ws, sizeof(ws), prev, sizeof(prev), &cnt);

        /* 修改时间 -> 本地时间。ftLastWriteTime 是 UTC, FileTimeToLocalFileTime 已转本地;
         * 原先又调 SystemTimeToTzSpecificLocalTime 把本地时间当 UTC 再偏移一次 (快 8 小时), 已删除 */
        FILETIME lft;
        SYSTEMTIME st;
        FileTimeToLocalFileTime(&fd.ftLastWriteTime, &lft);
        FileTimeToSystemTime(&lft, &st);
        char tbuf[32];
        snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);

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
    if (g_active_ws[0] && strlen(messages) > strlen(g_system_prompt))
        history_save();          /* 空对话不落盘, 避免覆盖旧会话文件 */
    if (!history_load_from_file(path)) return;
    set_edit_utf8(g_hWorkspace, g_workspace);
    wchar_t wws[MAX_PATH];
    if (utf8_to_wide(g_workspace, wws, MAX_PATH))
        SetCurrentDirectoryW(wws);
    SetWindowTextW(g_hHistory, L"");
    /* 清空会把段落格式一并复位(实测行距会跳回 RichEdit 的自然行距, 比我们设的松),
     * 缩放比也可能被重置 —— 所以紧接着把这两项重新下发一次。 */
    ui_history_parafmt(g_hHistory);
    ui_history_zoom_apply(g_hHistory);
    append_text("(已载入历史会话, 工作目录: ");
    append_text(g_workspace);
    append_text(")\r\n");
    history_replay();
}

/* ===== 布局与字体 (首次创建与 WM_DPICHANGED 共用) ===== */

/* 按 g_sess_dpi 重建列表用的两种字体(常规 / 加粗)。直接按字体族建, 不克隆主窗口的 g_hFont ——
 * 那个可能是别的显示器上的尺寸。 */
static void sess_fonts_sync(void) {
    if (g_sess_font_dpi == g_sess_dpi && g_sessFont && g_sessFontBold) return;
    if (g_sessFont)     { DeleteObject(g_sessFont);     g_sessFont     = NULL; }
    if (g_sessFontBold) { DeleteObject(g_sessFontBold); g_sessFontBold = NULL; }
    int h = dp_at(g_sess_dpi, 16);
    g_sessFont     = CreateFontW(h, 0,0,0, FW_NORMAL, FALSE,FALSE,FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 FF_DONTCARE, CAGENT_UI_FACE);
    g_sessFontBold = CreateFontW(h, 0,0,0, FW_BOLD,   FALSE,FALSE,FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                                 FF_DONTCARE, CAGENT_UI_FACE);
    g_sess_font_dpi = g_sess_dpi;
}

/* 重建会话列表控件。行高由 WM_MEASUREITEM 一次性决定, 而 ownerdraw 列表**不接受**
 * LB_SETITEMHEIGHT 改行高 —— 所以 DPI 变化时只能重建, 否则行高会停在旧字号的尺度上。
 * 首次创建时 old 为 NULL, 走同一条路径。 */
static void sess_create_list(HWND hwnd) {
    HWND old = GetDlgItem(hwnd, ID_SESS_LB);
    LRESULT cur = old ? SendMessageW(old, LB_GETCURSEL, 0, 0) : (LRESULT)-1;
    if (old) DestroyWindow(old);

    HWND lb = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY |
        LBS_OWNERDRAWFIXED | LBS_NOINTEGRALHEIGHT,
        0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_SESS_LB, NULL, NULL);
    for (int i = 0; i < g_sess_n; i++)
        SendMessageW(lb, LB_ADDSTRING, 0, 0);
    if (cur < 0) {   /* 首次: 预选最近使用的会话(ini 里记录的 last_session), 否则第一条 */
        cur = 0;
        for (int i = 0; i < g_sess_n; i++)
            if (g_last_session[0] && strcmp(g_sess_paths[i], g_last_session) == 0) { cur = i; break; }
    }
    if (g_sess_n > 0) SendMessageW(lb, LB_SETCURSEL, cur, 0);
}

/* 排布控件并下发字体。尺寸全部按当前 g_sess_dpi 现算, 所以重跑一次就适配新 DPI。
 * 客户区按 540x450 逻辑像素设计 —— 与 show_session_dialog 里的外框计算是一套。 */
static void sess_layout(HWND hwnd) {
    const UINT d = g_sess_dpi;
    MoveWindow(GetDlgItem(hwnd, ID_SESS_LB),  dp_at(d, 12),  dp_at(d, 12),
               dp_at(d, 496), dp_at(d, 330), TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_SESS_OK), dp_at(d, 300), dp_at(d, 356),
               dp_at(d, 100), dp_at(d, 32), TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_SESS_CAN), dp_at(d, 412), dp_at(d, 356),
               dp_at(d, 100), dp_at(d, 32), TRUE);
    SendMessageW(GetDlgItem(hwnd, ID_SESS_LB),  WM_SETFONT, (WPARAM)g_sessFont, TRUE);
    SendMessageW(GetDlgItem(hwnd, ID_SESS_OK),  WM_SETFONT, (WPARAM)g_sessFont, TRUE);
    SendMessageW(GetDlgItem(hwnd, ID_SESS_CAN), WM_SETFONT, (WPARAM)g_sessFont, TRUE);
}

static LRESULT CALLBACK SessDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_sess_dpi = dpi_of_window(hwnd);      /* 按对话框所在屏定 DPI */
        sess_fonts_sync();

        sess_create_list(hwnd);
        CreateWindowW(L"BUTTON", L"载入", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                      0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_SESS_OK, NULL, NULL);
        CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_SESS_CAN, NULL, NULL);
        sess_layout(hwnd);
        return 0;
    }

    case WM_DPICHANGED: {
        /* 被拖到缩放比例不同的显示器。**顺序**: 建字体 -> 重建列表(行高要按新 DPI 重算)
         * -> 调整外框 -> 排布并下发字体。字体只在 sess_layout 里下发(那时控件已在新位置),
         * 避免"新字号配旧几何"的中间帧留下残迹。**不碰全局 g_dpi**。 */
        UINT nd = (UINT)HIWORD(wp);
        g_sess_dpi = nd ? nd : dpi_of_window(hwnd);
        sess_fonts_sync();
        sess_create_list(hwnd);
        {
            const RECT *pr = (const RECT*)lp;
            RECT r = { 0, 0, dp_at(g_sess_dpi, 540), dp_at(g_sess_dpi, 450) };
            dpi_adjust_rect(&r, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX,
                            FALSE, WS_EX_DLGMODALFRAME, g_sess_dpi);
            MoveWindow(hwnd, pr->left, pr->top, r.right - r.left, r.bottom - r.top, TRUE);
        }
        sess_layout(hwnd);
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        return 0;
    }

    case WM_MEASUREITEM:
        ((MEASUREITEMSTRUCT*)lp)->itemHeight = dp_at(g_sess_dpi, 56);
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
        SelectObject(d->hDC, g_sessFontBold);
        wchar_t w1[200];
        utf8_to_wide(g_sess_ws[idx], w1, 200);
        RECT r1 = d->rcItem;
        r1.left  += dp_at(g_sess_dpi, 10);
        r1.top   += dp_at(g_sess_dpi, 6);
        r1.bottom = r1.top + dp_at(g_sess_dpi, 20);
        DrawTextW(d->hDC, w1, -1, &r1, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        /* 第二行: 条数 · 时间 · 预览 (灰) */
        SetTextColor(d->hDC, sel ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                 : RGB(120, 120, 120));
        SelectObject(d->hDC, g_sessFont);
        wchar_t w2[300];
        utf8_to_wide(g_sess_sub[idx], w2, 300);
        RECT r2 = d->rcItem;
        r2.left += dp_at(g_sess_dpi, 10);
        r2.top  += dp_at(g_sess_dpi, 28);
        DrawTextW(d->hDC, w2, -1, &r2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (d->itemState & ODS_FOCUS) DrawFocusRect(d->hDC, &d->rcItem);
        return TRUE;
    }
    case WM_COMMAND: {
        if (LOWORD(wp) == ID_SESS_LB && HIWORD(wp) == LBN_DBLCLK) wp = MAKEWPARAM(ID_SESS_OK, BN_CLICKED);
        if (LOWORD(wp) == ID_SESS_OK && HIWORD(wp) == BN_CLICKED) {
            HWND lb = GetDlgItem(hwnd, ID_SESS_LB);
            LRESULT idx = SendMessageW(lb, LB_GETCURSEL, 0, 0);
            if (idx != LB_ERR && idx < g_sess_n)
                sess_apply_selected(g_sess_paths[idx]);
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
    char sessdir[MAX_PATH];
    sessions_dir(sessdir, sizeof(sessdir));    /* sessions\ (含结尾 \), 不存在时已自动创建 */
    if (sess_enumerate(sessdir) == 0) {
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

    /* 按主窗口所在屏先定尺寸(窗口还没建, 只能问 owner); 建完后 WM_CREATE 会用对话框
     * 自己的显示器再核一次。540x450 是**客户区**逻辑尺寸, 与 sess_layout 是一套。 */
    g_sess_dpi = dpi_of_window(owner);
    RECT wr = { 0, 0, dp_at(g_sess_dpi, 540), dp_at(g_sess_dpi, 450) };
    dpi_adjust_rect(&wr, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX,
                    FALSE, WS_EX_DLGMODALFRAME, g_sess_dpi);

    g_sess_owner = owner;
    g_sess_dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"CAGENT_SESSDLG", L"历史会话",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, owner, NULL,
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
