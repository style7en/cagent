/*
 * ui/wndproc.h - 布局 / 输入框子类 / 主窗口过程 / 历史框子类
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== 布局 / 子类化 / 窗口过程 ===== */

static void layout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    /* 以下全部按 96 DPI 逻辑像素书写, 经 dp() 换算成物理像素 (见 ui/dpi.h)。
     * 调尺寸请改这些逻辑值, 不要再往布局里塞裸像素数字。 */
    int gap = dp(8);
    /* 行高 22 是**算出来的**, 不是随手定的:
     * 单行 EDIT 没有垂直对齐的开关(没有对应 ES_* 样式, 也没有 EM_* 消息), 文字由系统锚在
     * "距控件顶约 0.28×字号" 处, 余下的空间全留在下方 —— 行高给大了就看着偏上。
     * 实测(200%, 16 逻辑像素字号): 字形高 26px、锚点 9px, 行高 52 时是 9/17(偏上);
     * 要让上下相等需要 9+26+9 = 44px = dp(22)。锚点与字形高都随字号等比缩放,
     * 所以这个 22 在任何 DPI 下都成立。 */
    int row_h = dp(22);
    int lbl_w = dp(80);
    int top_h = row_h * 4 + gap * 5;
    int btn_w = dp(80);
    int ws_btn_w = dp(72);
    int new_w = dp(80);
    int sess_w = dp(80);
    int input_h = dp(72);
    int input_y = H - input_h - gap;
    int input_w = W - btn_w - gap * 3;

    for (int i = 0; i < 3; i++) {
        int y = gap + i * (row_h + gap);
        HWND lbl = GetDlgItem(hwnd, ID_LBL_BASE + i);
        MoveWindow(lbl,        gap,         y + dp(4), lbl_w,             row_h, TRUE);
        MoveWindow(g_hCfg[i],  gap + lbl_w, y,         W - gap*2 - lbl_w, row_h, TRUE);
    }
    {
        int y = gap + 3 * (row_h + gap);
        HWND lbl = GetDlgItem(hwnd, ID_LBL_WS);
        MoveWindow(lbl, gap, y + dp(4), lbl_w, row_h, TRUE);
        /* 按钮从右往左依次排: 加载会话 | 新建会话 | 浏览... —— 顺序写死在这里,
         * 加成对/删除都只动这几行, 不必再手算每个偏移。工作目录输入框吃掉剩余宽度。 */
        int x = W - gap;
        x -= sess_w;         MoveWindow(g_hSess,     x, y, sess_w,   row_h, TRUE);
        x -= gap + new_w;    MoveWindow(g_hNew,      x, y, new_w,    row_h, TRUE);
        x -= gap + ws_btn_w; MoveWindow(g_hWsBrowse, x, y, ws_btn_w, row_h, TRUE);
        x -= gap;
        MoveWindow(g_hWorkspace, gap + lbl_w, y, x - (gap + lbl_w), row_h, TRUE);
    }

    MoveWindow(g_hHistory, gap, top_h, W - gap * 2,
               H - top_h - input_h - gap * 2, TRUE);
    MoveWindow(g_hInput, gap, input_y, input_w, input_h, TRUE);
    MoveWindow(g_hSend,  gap * 2 + input_w, input_y, btn_w, input_h, TRUE);
}

/* ===== 字体与行距 (跨显示器 DPI 变化时要整体重做) ===== */

static BOOL CALLBACK ui_font_cb(HWND h, LPARAM lp) {
    (void)lp;
    /* 输出区用独立字体, 其余控件共用界面字体 */
    SendMessageW(h, WM_SETFONT,
                 (WPARAM)(GetDlgCtrlID(h) == ID_HISTORY ? g_hFontHist : g_hFont), TRUE);
    return TRUE;
}

/* 按 g_dpi 重建界面字体并下发到全部子控件。
 * **顺序必须是** 建新的 -> 下发 -> 再删旧的; 反过来控件会短暂引用已释放的句柄。
 * 窗口首次创建时也调它 —— 那时还没有子控件, 下发这一步自然是空操作。 */
static void ui_fonts_rebuild(HWND hwnd) {
    HFONT old[2] = { g_hFont, g_hFontHist };
    g_hFont     = CreateFontW(dp(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, FF_DONTCARE, CAGENT_UI_FACE);
    /* 输出区独立字体: 默认比控件/标签大一档, 阅读更醒目。
     * **基准字号恒为 HIST_LPX_DEF** —— Ctrl+滚轮的缩放走 EM_SETZOOM(整体显示缩放),
     * 不再靠改这个字体, 否则每次滚动都要重建字体+刷全部 run, 还会把 run 字号钉死。 */
    g_hFontHist = CreateFontW(dp(HIST_LPX_DEF), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, FF_DONTCARE, CAGENT_UI_FACE);
    EnumChildWindows(hwnd, ui_font_cb, 0);
    for (int i = 0; i < 2; i++) if (old[i]) DeleteObject(old[i]);
}

/* 收紧历史框行距: 用 PARAFORMAT2 设"精确"行距 = 字身格高 + HIST_LINE_EXTRA,
 * 去掉 RichEdit 默认的额外行距。三个坑:
 *   ① dyLineSpacing 是**绝对 twips**, 不随字体走 —— 字体换了(跨显示器 DPI 变化)就必须重设,
 *      否则行高按旧字号卡死, 字号变大时字会被裁切;
 *   ② 段落属性只作用于选中范围, 空选区时仅改光标所在那一段 —— 所以先全选、改完再还原选区;
 *   ③ 用**精确(exactly)**行距时行盒小于字形就会被削顶(见 state.h 的 HIST_LINE_EXTRA),
 *      别把值设成刚好等于字身格高。
 * 另外: 空字符串 WM_SETTEXT 会把段落格式复位(实测行距会跳回 RichEdit 的自然行距),
 * 所以清空历史框之后必须重新调一次本函数。 */
static void ui_history_parafmt(HWND h) {
    if (!h) return;
    LONG sel_s = 0, sel_e = 0;
    SendMessageW(h, EM_GETSEL, (WPARAM)&sel_s, (LPARAM)&sel_e);
    SendMessageW(h, EM_SETSEL, 0, -1);

    PARAFORMAT2 pf;
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_LINESPACING;
    pf.bLineSpacingRule = 4;          /* 精确行距 */
    /* 行高直接由"请求的字体像素高 + 余量"换算成 twips, **单一 DPI 来源 = g_dpi**。
     * 这里曾经用 GetTextMetricsW + GetDeviceCaps(hdc, LOGPIXELSY) 去量, 但那是错的:
     * 在 WM_DPICHANGED 里子控件的 DPI 上下文还没跟着更新, 那个 DC 报的仍是**旧屏**的 DPI,
     * 而 dp() 已经是新屏的 —— 两个口径混用会让行距整体偏一个 DPI 比例
     * (实测 200%->250% 时行距从应有的 47px 变成 60px)。 */
    pf.dyLineSpacing = (LONG)((LONGLONG)dp(HIST_LPX_DEF + HIST_LINE_EXTRA) * 1440
                              / (g_dpi ? g_dpi : 96));
    SendMessageW(h, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    SendMessageW(h, EM_SETSEL, sel_s, sel_e);
    SendMessageW(h, EM_SETMODIFY, FALSE, 0);
}

/* ===== 输出区字号缩放 (Ctrl + 滚轮) ===== */

/* 把 g_hist_lpx 折算成 EM_SETZOOM 的比值下发。基准字号(1:1)就是 HIST_LPX_DEF,
 * 其余尺寸全由控件自己缩放显示。
 *
 * 为什么用 EM_SETZOOM 而不是"重建字体 + SCF_ALL 刷 run 字号"(这两条路都实测过):
 *   - EM_SETZOOM 只缩放**显示**, 不碰字符格式 -> 行距(精确 twips)自动等比跟随。
 *     实测 1:1 行距 36px -> 5:4 得 45 -> 3:2 得 54, 回到 1:1 精确还原; 12:18 得 24、
 *     40:18 得 80, 与预期一致, 两端都被接受。
 *   - 写 run 字号会引入两个真 bug: ① yHeight 是 em 高、而默认字体是字身格高
 *     (YaHei 差 1.27 倍), 字比行距大 -> 削顶; ② WM_SETFONT 会作废 run 字号,
 *     跨显示器触发 ui_fonts_rebuild 后文字大小会跳变。
 * 若将来换到不认 EM_SETZOOM 的 RichEdit, 返回 0 —— 那时再考虑别的路, 但别再写 run 字号。 */
static void ui_history_zoom_apply(HWND hist) {
    if (!hist) return;
    SendMessageW(hist, EM_SETZOOM, (WPARAM)g_hist_lpx, (LPARAM)HIST_LPX_DEF);
}

/* 步进单位 1 逻辑像素。 */
static void ui_history_zoom(HWND hist, int steps) {
    if (!hist || !steps) return;
    int n = g_hist_lpx + steps;
    if (n < HIST_LPX_MIN) n = HIST_LPX_MIN;
    if (n > HIST_LPX_MAX) n = HIST_LPX_MAX;
    if (n == g_hist_lpx) return;                 /* 已到上下限: 不做无谓的缩放 */
    g_hist_lpx = n;
    ui_history_zoom_apply(hist);
    InvalidateRect(hist, NULL, TRUE);
}

/* Ctrl+滚轮 -> 缩放输出区字号; 返回非 0 表示该消息已被消费。
 *
 * 三处都要挂(主窗口 / 输出框子类 / 输入框子类), 因为 Windows 是把滚轮消息发给**焦点窗口**
 * 而不是光标下的窗口 —— 只挂输出框的话, 焦点在输入框里时就压根收不到。反过来说, 这个
 * 手势的语义就是"整窗有效、只作用于输出区", 与浏览器里 Ctrl+滚轮缩放一致。
 *
 * 修饰键用消息自带的 MK_CONTROL 而不是只靠 GetKeyState: 它随消息一起送达, 不依赖当前
 * 线程的消息队列状态(GetKeyState 取的是**调用线程**的键状态), 也让这条路径能被自动化验证。 */
static int ui_zoom_wheel(WPARAM wp) {
    if (!(LOWORD(wp) & MK_CONTROL) && !(GetKeyState(VK_CONTROL) & 0x8000)) return 0;

    int delta = (int)(short)HIWORD(wp);
    /* 一格 = WHEEL_DELTA(120)。高精度滚轮/触控板给出的是更小的增量, 累积满一格才走一步;
     * 方向一变就丢弃残量, 否则来回微滚会攒出"白送的"一步。 */
    if ((delta > 0) != (g_hist_zoom_acc > 0)) g_hist_zoom_acc = 0;
    g_hist_zoom_acc += delta;
    int steps = 0;
    while (g_hist_zoom_acc >= WHEEL_DELTA)  { g_hist_zoom_acc -= WHEEL_DELTA; steps++; }
    while (g_hist_zoom_acc <= -WHEEL_DELTA) { g_hist_zoom_acc += WHEEL_DELTA; steps--; }
    ui_history_zoom(g_hHistory, steps);
    return 1;   /* 攒不满一格也算消费: 否则缩放途中会同时触发控件自己的滚动 */
}

static LRESULT CALLBACK InputProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    /* Ctrl+滚轮: 缩放输出区字号(见 ui_zoom_wheel 的注释: 焦点在这里时消息只到这儿) */
    if (msg == WM_MOUSEWHEEL && ui_zoom_wheel(wp)) return 0;
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

        /* 此刻窗口已经落在某块显示器上 —— 按**这块屏**的 DPI 定 g_dpi。
         * per-monitor 下不能沿用 dpi_init() 里那个(那是主屏的)。 */
        g_dpi = dpi_of_window(hwnd);

        /* 按 DPI 修正外框, 让客户区正好是 720x560 逻辑像素 */
        {
            RECT r = { 0, 0, dp(720), dp(560) };
            dpi_adjust_rect(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, g_dpi);
            SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        ui_fonts_rebuild(hwnd);   /* 建字体; 此刻还没有子控件, 下发是空操作 */

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
        log_line("[启动] cagent %s, 日志已启用 (cagent.ini 里 log=0 可关)", CAGENT_VERSION_STR);
        system_prompt_init();        /* 读 exe 同目录外置 SYSTEM_PROMPT, 缺失/无效静默用默认 */
        migrate_legacy_sessions();   /* 旧版散落在 exe 目录的会话搬进 sessions\ */
        cleanup_orphan_session_backups();   /* 主文件已删的 .bak / .pre_compact 顺手清掉 */
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
        ui_history_parafmt(g_hHistory);
        /* 输出区底色略深于输入框(见 state.h 的 HIST_BG)。必须走 EM_SETBKGNDCOLOR ——
         * RichEdit 不是 STATIC, 不会来问 WM_CTLCOLORSTATIC, 那边返回的画刷对它无效。
         * 颜色与 DPI 无关, 所以只在创建时设一次; WM_DPICHANGED 里不必重设。 */
        SendMessageW(g_hHistory, EM_SETBKGNDCOLOR, 0, (LPARAM)(DWORD)HIST_BG);

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

        /* 「关于」挂到系统菜单 (标题栏图标左键 / Alt+Space / 右键标题栏)。
         * GetSystemMenu 第二参数必须传 FALSE: 传 TRUE 会把菜单重置回默认,
         * 我们刚加的两项会被抹掉。分隔线把自定义项与系统的 关闭 隔开。 */
        {
            HMENU sysm = GetSystemMenu(hwnd, FALSE);
            if (sysm) {
                AppendMenuW(sysm, MF_SEPARATOR, 0, NULL);
                AppendMenuW(sysm, MF_STRING, IDM_ABOUT, L"关于(&A)...");
            }
        }

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

    case WM_SYSCOMMAND:
        /* 系统菜单里我们加的自定义项。低 4 位被系统用作内部标志(如 SC_MOUSEMENU 高位),
         * 所以比较前必须先掩掉 —— 否则鼠标点出来的命令 (0xF0x0) 匹配不上。 */
        if ((wp & 0xFFF0) == IDM_ABOUT) {
            show_about(hwnd);
            return 0;
        }
        break;

    case WM_DPICHANGED: {
        /* 窗口被拖到了缩放比例不同的显示器。
         * **顺序很关键**: 先按新 DPI 调整外框并重新排布控件, **最后**才重建字体。
         * 反过来(先换字体)会让控件带着新字号、按**旧几何**先重画一次; 那一帧的像素若没被
         * 随后的位移擦掉就会留在原位 —— 实测: 主屏(200%)拖到副屏(250%)后, "工作目录:" 上方
         * 出现一排残留的笔画尖(屏幕上看得到, PrintWindow 重画时看不到, 因为它是残迹不是绘制错)。 */
        UINT nd = (UINT)HIWORD(wp);
        g_dpi = nd ? nd : dpi_of_window(hwnd);

        {
            const RECT *pr = (const RECT*)lp;
            RECT r = { 0, 0, dp(720), dp(560) };
            dpi_adjust_rect(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, g_dpi);
            SetWindowPos(hwnd, NULL, pr->left, pr->top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        layout(hwnd);
        ui_fonts_rebuild(hwnd);        /* 最后换字体: 控件按新几何重画一次即可 */
        ui_history_parafmt(g_hHistory);
        ui_history_zoom_apply(g_hHistory);   /* 缩放比是控件属性, 这里重下一遍保险 */

        /* 兜底: DPI 切换期间父窗口可能有大片区域没被擦到(子控件位移留下的空档)。
         * 整棵子树擦除 + 立即重画, 保证不留旧 DPI 的残迹。 */
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
        return 0;
    }

    case WM_MOUSEWHEEL:
        /* Ctrl+滚轮: 缩放输出区字号。没按 Ctrl 就交给控件自己滚动。 */
        if (ui_zoom_wheel(wp)) return 0;
        break;

    case WM_CTLCOLORSTATIC:
        /* 透明背景: 标签不擦自己的底, 底色由父窗口提供。
         * (试过改成"不透明 + 返回同色画刷"让标签自擦 —— 结果每个标签后面多出一块**白底**:
         *  OPAQUE 模式下 STATIC 填充用的是 DC 的 BkColor 而不是返回的画刷, 而 BkColor 默认是白。
         *  真要走自擦那条路, 必须同时 SetBkColor(COLOR_BTNFACE)。这里选择留在透明模式,
         *  由 WM_DPICHANGED 末尾那次整棵子树重画来保证不留旧像素。) */
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
    /* Ctrl+滚轮缩放输出区字号; 其余滚轮动作照旧交回原过程(滚动)并补一次重画 */
    if (msg == WM_MOUSEWHEEL && ui_zoom_wheel(wp)) return 0;
    if (msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL ||
        msg == WM_KEYDOWN || msg == WM_KEYUP) {
        LRESULT r = CallWindowProcW(g_oldHistoryProc, h, msg, wp, lp);
        InvalidateRect(h, NULL, TRUE);
        UpdateWindow(h);
        return r;
    }
    return CallWindowProcW(g_oldHistoryProc, h, msg, wp, lp);
}
