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
 * 的超链接都要求 comctl32 v6 (本项目已通过 res/app.manifest 声明), 但那样又得改控件结构;
 * 带 SS_NOTIFY 的 STATIC 更简单可靠 —— 点击会向父窗口发 WM_COMMAND(STN_CLICKED), 我们接住后
 * 交给 ShellExecute 打开默认浏览器; 视觉上套系统链接色 + 下划线 + 手型光标。
 *
 * DPI: 对话框维护**自己的** g_about_dpi, 用 dp_at() 换算 ——
 *   ① 它可能被拖到与主窗口不同缩放比例的显示器上;
 *   ② **绝不能去写全局 g_dpi**, 那会让主窗口下次 layout() 用错比例。
 *   布局全部集中在 about_layout(), 所以重新排布 = 重跑一次该函数。
 *
 * 非模态的生命周期与 session_dlg.h 一致: 窗口消息由主循环统一泵送, 关闭时自己恢复主窗口。
 */

#define ID_ABOUT_URL        2004   /* GitHub 链接 (充当链接的 STATIC) */
#define ID_ABOUT_OK         2005   /* 关闭按钮 */
#define ID_ABOUT_URL_GITEE  2006   /* Gitee 链接 */
#define ID_ABOUT_TITLE      2007   /* "cagent" */
#define ID_ABOUT_TAG        2008   /* 一句话定位 */
#define ID_ABOUT_VER_L      2009   /* "版本" 标签 */
#define ID_ABOUT_VER        2010   /* 版本号 */
#define ID_ABOUT_GIT_L      2011   /* "GitHub" 标签 */
#define ID_ABOUT_GITEE_L    2012   /* "Gitee" 标签 */

static HWND g_about_wnd   = NULL;
static HWND g_about_owner = NULL;

/* 对话框自己的 DPI 与字体。字体按 g_about_dpi 从字体族**直接建**(不是克隆主窗口的
 * g_hFont —— 那个可能是别的显示器上的尺寸)。 */
static UINT  g_about_dpi = 96;
static UINT  g_about_font_dpi = 0;      /* 下面三个字体是按哪个 DPI 建的 */
static HFONT g_aboutFontUI    = NULL;
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

/* 按 g_about_dpi 重建三种字体(界面体 / 标题加粗 / 链接下划线)。DPI 没变就什么都不做。
 * 早先这里是"克隆主窗口 g_hFont 的 LOGFONT 再改一两项", 有两个毛病: 依赖主窗口的 DPI,
 * 以及在同一个 LOGFONT 实例上连着改会让链接字体继承标题的 FW_BOLD(实测就是这样)。 */
static void about_fonts_sync(void) {
    if (g_about_font_dpi == g_about_dpi && g_aboutFontUI && g_aboutFontTitle && g_aboutFontLink)
        return;
    if (g_aboutFontUI)    { DeleteObject(g_aboutFontUI);    g_aboutFontUI    = NULL; }
    if (g_aboutFontTitle) { DeleteObject(g_aboutFontTitle); g_aboutFontTitle = NULL; }
    if (g_aboutFontLink)  { DeleteObject(g_aboutFontLink);  g_aboutFontLink  = NULL; }

    int h = dp_at(g_about_dpi, 16);
    g_aboutFontUI    = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, FF_DONTCARE, CAGENT_UI_FACE);
    g_aboutFontTitle = CreateFontW(h, 0, 0, 0, FW_BOLD,   FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, FF_DONTCARE, CAGENT_UI_FACE);
    g_aboutFontLink  = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, TRUE,  FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, FF_DONTCARE, CAGENT_UI_FACE);
    g_about_font_dpi = g_about_dpi;
}

/* 排布全部控件并下发字体。窗口首次创建与 WM_DPICHANGED 都调它 —— 尺寸全部现算,
 * 所以重跑一次就自动适配新 DPI。客户区固定 420x180 逻辑像素。
 * 左内边距 16, 标签列 56(放得下 "GitHub"), 值列自 x=76 起, 行距 = 字体高度 + 4px。
 * 这里的 420x180 与 show_about 里 dpi_adjust_rect 的尺寸是一套, 改一处必须改另一处。 */
static void about_layout(HWND hwnd) {
    const UINT d  = g_about_dpi;
    const int  pad = dp_at(d, 16);
    const int  lw  = dp_at(d, 56);
    const int  vx  = pad + lw + dp_at(d, 4);      /* 值列起点 = 76 */
    const int  vw  = dp_at(d, 420) - pad - vx;    /* 值列宽   = 328 */
    const int  rh  = dp_at(d, 20);
    const int  bw  = dp_at(d, 84);
    const int  right = dp_at(d, 420);

    MoveWindow(GetDlgItem(hwnd, ID_ABOUT_TITLE),   pad, dp_at(d, 12),  dp_at(d, 388), dp_at(d, 22), TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_ABOUT_TAG),     pad, dp_at(d, 38),  dp_at(d, 388), rh,           TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_ABOUT_VER_L),   pad, dp_at(d, 66),  lw,            rh,           TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_ABOUT_VER),     vx,  dp_at(d, 66),  vw,            rh,           TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_ABOUT_GIT_L),   pad, dp_at(d, 90),  lw,            rh,           TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_ABOUT_URL),     vx,  dp_at(d, 90),  vw,            rh,           TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_ABOUT_GITEE_L), pad, dp_at(d, 114), lw,            rh,           TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_ABOUT_URL_GITEE), vx, dp_at(d, 114), vw,           rh,           TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_ABOUT_OK),      right - pad - bw, dp_at(d, 142), bw, dp_at(d, 26), TRUE);

    SendMessageW(GetDlgItem(hwnd, ID_ABOUT_TITLE),     WM_SETFONT, (WPARAM)g_aboutFontTitle, TRUE);
    SendMessageW(GetDlgItem(hwnd, ID_ABOUT_URL),       WM_SETFONT, (WPARAM)g_aboutFontLink,  TRUE);
    SendMessageW(GetDlgItem(hwnd, ID_ABOUT_URL_GITEE), WM_SETFONT, (WPARAM)g_aboutFontLink,  TRUE);
    static const int plain[] = { ID_ABOUT_TAG, ID_ABOUT_VER_L, ID_ABOUT_VER,
                                ID_ABOUT_GIT_L, ID_ABOUT_GITEE_L, ID_ABOUT_OK };
    for (int i = 0; i < (int)(sizeof(plain)/sizeof(plain[0])); i++)
        SendMessageW(GetDlgItem(hwnd, plain[i]), WM_SETFONT, (WPARAM)g_aboutFontUI, TRUE);
}

static LRESULT CALLBACK AboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        wchar_t wtag[256], wver[160], wgh[512], wgt[512];
        if (!utf8_to_wide(CAGENT_TAGLINE,           wtag, 256)) wtag[0] = L'\0';
        if (!utf8_to_wide(CAGENT_VERSION_STR,       wver, 160)) wver[0] = L'\0';
        if (!utf8_to_wide(CAGENT_PROJECT_URL,       wgh,  512)) wgh[0]  = L'\0';
        if (!utf8_to_wide(CAGENT_PROJECT_URL_GITEE, wgt,  512)) wgt[0]  = L'\0';

        g_about_dpi = dpi_of_window(hwnd);   /* 按对话框所在屏定 DPI */
        about_fonts_sync();

        /* 先建控件(尺寸留给 about_layout 摆), 再统一下发字体与位置 */
        struct { int id; const WCHAR *cls; const WCHAR *txt; DWORD st; int link; } defs[] = {
            { ID_ABOUT_TITLE,   L"STATIC", L"cagent",  SS_LEFT, 0 },
            { ID_ABOUT_TAG,     L"STATIC", wtag,       SS_LEFT, 0 },
            { ID_ABOUT_VER_L,   L"STATIC", L"版本",    SS_LEFT, 0 },
            { ID_ABOUT_VER,     L"STATIC", wver,       SS_LEFT, 0 },
            { ID_ABOUT_GIT_L,   L"STATIC", L"GitHub",  SS_LEFT, 0 },
            { ID_ABOUT_URL,     L"STATIC", wgh,        SS_LEFT | SS_NOTIFY, 1 },
            { ID_ABOUT_GITEE_L, L"STATIC", L"Gitee",   SS_LEFT, 0 },
            { ID_ABOUT_URL_GITEE, L"STATIC", wgt,      SS_LEFT | SS_NOTIFY, 1 },
        };
        for (int i = 0; i < (int)(sizeof(defs)/sizeof(defs[0])); i++)
            CreateWindowW(defs[i].cls, defs[i].txt, WS_CHILD | WS_VISIBLE | defs[i].st,
                          0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)defs[i].id, NULL, NULL);

        CreateWindowW(L"BUTTON", L"关闭",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
                      0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_ABOUT_OK, NULL, NULL);

        about_layout(hwnd);
        return 0;
    }

    case WM_DPICHANGED: {
        /* 被拖到缩放比例不同的显示器。**顺序**: 建字体 -> 调整外框 -> 排布并下发字体。
         * 字体只在 about_layout 里下发(那时控件已经移到新位置), 所以不会出现"新字号配旧几何"
         * 的中间帧残迹。最后不碰全局 g_dpi —— 那是主窗口的。 */
        UINT nd = (UINT)HIWORD(wp);
        g_about_dpi = nd ? nd : dpi_of_window(hwnd);
        about_fonts_sync();
        {
            const RECT *pr = (const RECT*)lp;
            RECT r = { 0, 0, dp_at(g_about_dpi, 420), dp_at(g_about_dpi, 180) };
            dpi_adjust_rect(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
                            WS_EX_DLGMODALFRAME, g_about_dpi);
            MoveWindow(hwnd, pr->left, pr->top, r.right - r.left, r.bottom - r.top, TRUE);
        }
        about_layout(hwnd);
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        /* 链接用系统热链色(跟随主题); 背景保持**透明** —— 同 wndproc.h 里那段注释:
         * OPAQUE 模式下 STATIC 用的是 DC 的 BkColor(默认白)而不是返回的画刷, 会多出一块白底。 */
        if (about_link_url(GetDlgCtrlID((HWND)lp)))
            SetTextColor((HDC)wp, GetSysColor(COLOR_HOTLIGHT));
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

    /* 先按**主窗口所在屏**定尺寸(窗口还没建, 只能问 owner), 建完后 WM_CREATE 会用
     * 对话框自己的显示器再核一次。 */
    g_about_dpi = dpi_of_window(owner);
    RECT r = { 0, 0, dp_at(g_about_dpi, 420), dp_at(g_about_dpi, 180) };
    dpi_adjust_rect(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME, g_about_dpi);
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
