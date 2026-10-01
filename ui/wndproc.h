/*
 * ui/wndproc.h - 布局 / 输入框子类 / 主窗口过程 / 历史框子类
 *
 * cagent 界面层的一部分, 由 cagent_ui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

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

        static const WCHAR *labels[3] = { L"Base Url:", L"Key:", L"Model:" };
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
        system_prompt_init();        /* 读 exe 同目录外置 SYSTEM_PROMPT, 缺失/无效静默用默认 */
        migrate_legacy_sessions();   /* 旧版散落在 exe 目录的会话搬进 sessions\ */
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

        /* 收紧历史框行距: 用 PARAFORMAT2 设"精确"行距为字体字身高度(physical twips),
           去除 RichEdit 默认额外行距; 按 LOGPIXELSY 换算以兼顾高 DPI。glyph 不会裁切。 */
        {
            PARAFORMAT2 pf;
            memset(&pf, 0, sizeof(pf));
            pf.cbSize = sizeof(pf);
            pf.dwMask = PFM_LINESPACING;
            pf.bLineSpacingRule = 4;          /* 精确行距 */
            HDC hdc = GetDC(g_hHistory);
            HFONT oldf = (HFONT)SelectObject(hdc, g_hFontHist);
            TEXTMETRICW tm;
            if (GetTextMetricsW(hdc, &tm)) {
                int lpy = GetDeviceCaps(hdc, LOGPIXELSY);
                pf.dyLineSpacing = (LONG)((LONGLONG)tm.tmHeight * 1440 / (lpy ? lpy : 96));
            } else {
                pf.dyLineSpacing = 18 * 15;   /* 兜底: 18px @96DPI */
            }
            if (oldf) SelectObject(hdc, oldf);
            ReleaseDC(g_hHistory, hdc);
            SendMessageW(g_hHistory, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
        }

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

        /* 不要在这里 SetFocus(g_hInput): 窗口尚未显示, 输入框会在"光标创建但
         * 不可见"的半初始化状态下拿到焦点, 且之后点击不再触发 WM_SETFOCUS
         * (已有焦点), 光标永不出现。保持不设焦点, 由用户点击时自然建立。 */
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
        /* Agent 线程若仍在运行: 先请求取消并等它收尾 (history_save 在线程里执行,
         * 直接退出可能把会话文件写一半)。有界等待 —— 服务端僵死时最多多等 5s,
         * 不让关闭动作无限卡住; 超时则照常退出 (.bak 兜底)。 */
        if (g_hThread) {
            InterlockedExchange(&g_cancel, 1);
            WaitForSingleObject(g_hThread, 5000);
            CloseHandle(g_hThread);
            g_hThread = NULL;
        }
        InterlockedExchange(&g_running, 0);
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
    if (msg == WM_APP_STREAM) {
        if (wp == 1) {
            g_stream_chars = 0;
        } else if (g_stream_chars > 0) {
            /* -1 表示选到真正的末尾, 避免 GetWindowTextLengthW 的估算误差留下残尾 */
            long start = GetWindowTextLengthW(h) - g_stream_chars;
            if (start < 0) start = 0;
            SendMessageW(h, EM_SETSEL, start, -1);
            SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)L"");
            g_stream_chars = 0;
        }
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
