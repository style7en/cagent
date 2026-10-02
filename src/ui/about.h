/*
 * ui/about.h - "关于" 对话框: 一句话功能 + 版本 + 两个项目地址 (地址可点击打开)
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 *
 * 入口: 主窗口**系统菜单**里的「关于(&A)...」(点标题栏图标 / Alt+Space / 右键标题栏),
 * 见 ui/wndproc.h 的 WM_CREATE 挂菜单 + WM_SYSCOMMAND 分发 (命令 ID = IDM_ABOUT)。
 * 之所以不用标题栏问号按钮: WS_EX_CONTEXTHELP 与 WS_MINIMIZEBOX/WS_MAXIMIZEBOX 互斥,
 * 实测在带最小化/最大化的窗口上问号根本不会被绘制。
 *
 * 文案全部取自 core/version.h, 这里不出现字面量的版本号或地址 —— 改版本/地址只改那一处。
 *
 * 为什么不用 MessageBox: 它的正文是纯文本, 里面的链接点不开。而 SysLink 控件 / TaskDialog
 * 的超链接都要求 comctl32 v6 (必须给 EXE 嵌 manifest), 本程序没有 manifest。
 * 于是用最朴素也最可靠的做法: 带 SS_NOTIFY 的 STATIC 充当链接 —— 点击会向父窗口发
 * WM_COMMAND(STN_CLICKED), 我们接住后交给 ShellExecute 打开默认浏览器; 视觉上套系统
 * 链接色 + 下划线 + 手型光标, 用户能认出它是链接。
 *
 * 非模态的生命周期与 session_dlg.h 一致: 窗口消息由主循环统一泵送, 关闭时自己恢复主窗口。
 */

#define ID_ABOUT_URL       2004   /* GitHub 链接 (充当链接的 STATIC) */
#define ID_ABOUT_URL_GITEE 2006   /* Gitee 链接 */
#define ID_ABOUT_OK        2005   /* 关闭按钮 */

static HWND g_about_wnd   = NULL;
static HWND g_about_owner = NULL;
static HFONT g_aboutFontTitle = NULL;
static HFONT g_aboutFontLink  = NULL;

/* 两个链接共用同一套「着色 / 光标 / 点击」逻辑, 只有 URL 不同 —— 集中成一个判据,
 * 免得 WM_CTLCOLORSTATIC / WM_SETCURSOR / WM_COMMAND 三处各写一遍 if。
 * 非链接控件返回 NULL。 */
static const char *about_link_url(int id) {
    if (id == ID_ABOUT_URL)       return CAGENT_PROJECT_URL;
    if (id == ID_ABOUT_URL_GITEE) return CAGENT_PROJECT_URL_GITEE;
    return NULL;
}

/* 打开链接 (交给系统默认浏览器)。ShellExecuteW 失败不打扰用户: 地址就印在窗口上,
 * 用户自己也能复制。 */
static void about_open_url(const char *url) {
    wchar_t wurl[512];
    if (url && utf8_to_wide(url, wurl, 512))
        ShellExecuteW(NULL, L"open", wurl, NULL, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        wchar_t wtag[256], wver[160], wgh[512], wgt[512];
        if (!utf8_to_wide(CAGENT_TAGLINE,           wtag, 256)) wtag[0] = L'\0';
        if (!utf8_to_wide(CAGENT_VERSION_STR,       wver, 160)) wver[0] = L'\0';
        if (!utf8_to_wide(CAGENT_PROJECT_URL,       wgh,  512)) wgh[0]  = L'\0';
        if (!utf8_to_wide(CAGENT_PROJECT_URL_GITEE, wgt,  512)) wgt[0]  = L'\0';

        /* 由控件字体派生出标题(加粗)与链接(下划线)两种变体, 保持字族一致。
         * 必须各从**原始** lf 派生 —— 曾图省事在同一个 lf 上连着改(先置 FW_BOLD, 再加下划线)
         * 去创建链接字体, 结果链接连标题的粗体一起继承, 显示成"加粗的下划线蓝字"。 */
        if (!g_aboutFontTitle || !g_aboutFontLink) {
            LOGFONTW base, lf;
            GetObjectW(g_hFont, sizeof(base), &base);
            if (!g_aboutFontTitle) { lf = base; lf.lfWeight    = FW_BOLD; g_aboutFontTitle = CreateFontIndirectW(&lf); }
            if (!g_aboutFontLink)  { lf = base; lf.lfUnderline = TRUE;    g_aboutFontLink  = CreateFontIndirectW(&lf); }
        }

        /* 紧凑排布: 左内边距 16, 标签列 56(放得下 "GitHub"), 值列自 x=76 起, 行距只给
         * 字体实际高度 + 4px。全部按 96 DPI 逻辑像素书写, 经 dp() 换算 (见 ui/dpi.h)。
         * 客户区 420x180 —— 与下面 show_about 里 AdjustWindowRectEx 的尺寸是一套,
         * 改一处必须改另一处, 否则底部留白或按钮被裁掉。 */
        const int pad = dp(16);
        const int lw  = dp(56);
        const int vx  = pad + lw + dp(4);       /* 值列起点 = 76 */
        const int vw  = dp(420) - pad - vx;     /* 值列宽   = 328 */
        const int rh  = dp(20);
        const int bw  = dp(84);

        HWND t;
        t = CreateWindowW(L"STATIC", L"cagent", WS_CHILD | WS_VISIBLE | SS_LEFT,
                          pad, dp(12), dp(388), dp(22), hwnd, NULL, NULL, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)(g_aboutFontTitle ? g_aboutFontTitle : g_hFont), TRUE);

        t = CreateWindowW(L"STATIC", wtag, WS_CHILD | WS_VISIBLE | SS_LEFT,
                          pad, dp(38), dp(388), rh, hwnd, NULL, NULL, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        /* 三行"标签列 + 值列": 光秃秃一个 1.1.2 没人看得出是什么 */
        t = CreateWindowW(L"STATIC", L"版本", WS_CHILD | WS_VISIBLE | SS_LEFT,
                          pad, dp(66), lw, rh, hwnd, NULL, NULL, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        t = CreateWindowW(L"STATIC", wver, WS_CHILD | WS_VISIBLE | SS_LEFT,
                          vx, dp(66), vw, rh, hwnd, NULL, NULL, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        /* 两个链接: SS_NOTIFY 才会把点击告诉父窗口 */
        t = CreateWindowW(L"STATIC", L"GitHub", WS_CHILD | WS_VISIBLE | SS_LEFT,
                          pad, dp(90), lw, rh, hwnd, NULL, NULL, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        t = CreateWindowW(L"STATIC", wgh, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
                          vx, dp(90), vw, rh, hwnd, (HMENU)(LONG_PTR)ID_ABOUT_URL, NULL, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)(g_aboutFontLink ? g_aboutFontLink : g_hFont), TRUE);

        t = CreateWindowW(L"STATIC", L"Gitee", WS_CHILD | WS_VISIBLE | SS_LEFT,
                          pad, dp(114), lw, rh, hwnd, NULL, NULL, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        t = CreateWindowW(L"STATIC", wgt, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOTIFY,
                          vx, dp(114), vw, rh, hwnd, (HMENU)(LONG_PTR)ID_ABOUT_URL_GITEE, NULL, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)(g_aboutFontLink ? g_aboutFontLink : g_hFont), TRUE);

        t = CreateWindowW(L"BUTTON", L"关闭",
                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                          dp(420) - pad - bw, dp(142), bw, dp(26), hwnd,
                          (HMENU)(LONG_PTR)ID_ABOUT_OK, NULL, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        /* 链接用系统热链色 (跟随主题, 深色模式也不瞎), 背景与对话框一致否则会留白块 */
        if (about_link_url(GetDlgCtrlID((HWND)lp))) {
            SetBkMode((HDC)wp, TRANSPARENT);
            SetTextColor((HDC)wp, GetSysColor(COLOR_HOTLIGHT));
            return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
        }
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_SETCURSOR:
        /* 指到链接上换手型光标 —— 没有这个, 用户不知道那行字能点 */
        {
            POINT pt;
            if (GetCursorPos(&pt)) {
                HWND c = WindowFromPoint(pt);
                if (c && about_link_url(GetDlgCtrlID(c))) {
                    /* 用通用 LoadCursor 而非 LoadCursorW: IDC_HAND 是 MAKEINTRESOURCE 的
                     * ANSI 变体(LPSTR), 直接喂 W 版会类型不符。预定义光标只看资源 ID,
                     * A/W 无差别 —— 项目里注册窗口类也是这么写的。 */
                    SetCursor(LoadCursor(NULL, IDC_HAND));
                    return TRUE;
                }
            }
        }
        break;

    case WM_COMMAND:
        {   /* STN_CLICKED: 两个链接共用一条路径, 只有 URL 不同 */
            const char *u = about_link_url(LOWORD(wp));
            if (u) { about_open_url(u); return 0; }
        }
        if (LOWORD(wp) == ID_ABOUT_OK) { DestroyWindow(hwnd); return 0; }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_about_owner) {
            EnableWindow(g_about_owner, TRUE);
            SetForegroundWindow(g_about_owner);
        }
        g_about_wnd = NULL;
        g_about_owner = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 弹出关于窗口 (模态感: 期间禁用主窗口), 已打开则前置。 */
static void show_about(HWND owner) {
    if (g_about_wnd) { SetForegroundWindow(g_about_wnd); return; }

    static int reg = 0;
    if (!reg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = AboutProc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"CAGENT_ABOUT";
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.hIcon         = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
        if (!RegisterClassW(&wc)) return;
        reg = 1;
    }

    /* 按客户区尺寸反推外框: 免得到手算边框/标题栏高度。
     * 420x180 是**逻辑尺寸**, 经 dp() 换算; 与上面控件排布是一套 —— 改一处必须改另一处。
     * 进程是系统级 DPI 感知, 所以 AdjustWindowRectEx 用的度量与 g_dpi 一致。 */
    RECT r = {0, 0, dp(420), dp(180)};
    AdjustWindowRectEx(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    int cw = r.right - r.left, chh = r.bottom - r.top;

    g_about_owner = owner;
    g_about_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"CAGENT_ABOUT", L"关于 cagent",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, cw, chh,
        owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_about_wnd) { g_about_owner = NULL; return; }

    /* 居中于主窗口 */
    RECT ro, rd;
    GetWindowRect(owner, &ro);
    GetWindowRect(g_about_wnd, &rd);
    int x = ro.left + ((ro.right - ro.left) - (rd.right - rd.left)) / 2;
    int y = ro.top  + ((ro.bottom - ro.top) - (rd.bottom - rd.top)) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    MoveWindow(g_about_wnd, x, y, rd.right - rd.left, rd.bottom - rd.top, TRUE);

    EnableWindow(owner, FALSE);
    ShowWindow(g_about_wnd, SW_SHOW);
    SetForegroundWindow(g_about_wnd);
}
