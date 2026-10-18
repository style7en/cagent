/*
 * ui/main.h - 程序入口: 注册窗口类 / 图标 / 消息循环
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev;
    if (cmd && strstr(cmd, "--version")) {
        /* GUI 子系统默认无控制台: 若有父控制台则接管并打印 (脚本可捕获),
           否则用对话框显示 (双击启动也能看到) */
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            printf("cagent %s\n", CAGENT_VERSION_STR);
            fflush(stdout);
        } else {
            MessageBoxW(NULL, CAGENT_VERSION_WSTR, L"cagent", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }

    /* 安装宿主钩子 (必须在 agent 运行前设置) */
    cagent_emit = gui_emit;
    cagent_on_done = gui_on_done;
    cagent_read_config_ui = gui_read_config_ui;
    cagent_stream_begin = gui_stream_begin;
    cagent_stream_undo = gui_stream_undo;
    cagent_status = gui_status;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    dpi_init();          /* 只给 g_dpi 一个初值; DPI 感知本身在 res/app.manifest 里声明 */
    InitCommonControls();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));   /* app.rc: 1 ICON app.ico */
    wc.lpszClassName = L"CagentGuiWnd";
    /* CS_HREDRAW | CS_VREDRAW: 窗口宽/高变化时**整个客户区**都作废重画。
     * 不加这两位的后果就是 issue #2 —— 水平拖动缩放时只有"新露出来"的那条区域被作废,
     * 而三个右侧对齐的按钮(浏览/新建会话/加载会话)是**整体左移**的: 它们让出的那几像素
     * 宽的老像素没人擦, 于是和按钮新位置拼在一起, 看着就像三个按钮叠在一起。松手后
     * (或任何一次整窗重绘)老像素被盖掉, 又"恢复正常", 正是用户描述的"重绘窗口后正常"。
     *
     * 注意 CS_* 是**窗口类**样式, 只能写进 wc.style; 而 WS_CLIPCHILDREN 是**窗口**样式,
     * 必须写在 CreateWindowW 的 dwStyle 里(见下) —— 两者字段不同, 不能混放。 */
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassW(&wc);

    /* 标题栏只写名字, 不带版本号: 版本在「关于」(系统菜单) 与 exe 文件属性里, 标题栏里
     * 那串数字每次发版都要跟着改, 且对用户没有信息量。 */
    /* 初次创建用系统 DPI 估个尺寸即可 —— WM_CREATE 里会按窗口真正落在的那块屏重算。
     * ex 样式 = CAGENT_WND_EXSTYLE (WS_EX_COMPOSITED): 整窗离屏合成, 单帧原子上屏
     * —— 这是"输出区闪"的根治手段 (依据见 state.h 注释)。 */
    HWND hwnd = CreateWindowExW(CAGENT_WND_EXSTYLE, L"CagentGuiWnd", L"cagent",
        CAGENT_WND_STYLE,
        CW_USEDEFAULT, CW_USEDEFAULT, dp(720), dp(560),
        NULL, NULL, hInst, NULL);

    /* 标题栏/任务栏图标 (大 + 小) */
    SendMessageW(hwnd, WM_SETICON, ICON_BIG,
        (LPARAM)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                           GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
        (LPARAM)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                           GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));

    g_oldHistoryProc = (WNDPROC)SetWindowLongPtrW(g_hHistory, GWLP_WNDPROC, (LONG_PTR)HistoryProc);

    /* 测试钩子: CAGENT_SIMULATE_DPI=<dpi> —— 启动时给自己发一次 WM_DPICHANGED。
     * 真实"拖到另一块缩放率不同的屏"无法脚本化, 而跨进程注入此消息会被 user32
     * 静默拦截 (Send/Post 均拒, E2E 实测) —— 只能由进程自发送驱动同一条处理链路。 */
    {
        const char *sd = getenv("CAGENT_SIMULATE_DPI");
        if (sd && sd[0]) {
            int nd = atoi(sd);
            if (nd > 0 && nd != (int)g_dpi) {
                RECT sug;
                GetWindowRect(hwnd, &sug);
                sug.right  = sug.left + (sug.right - sug.left) * nd / (int)g_dpi;
                sug.bottom = sug.top  + (sug.bottom - sug.top) * nd / (int)g_dpi;
                SendMessageW(hwnd, WM_DPICHANGED, MAKEWPARAM(0, nd), (LPARAM)&sug);
            }
        }
    }

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    /* 不在此 SetFocus(输入框): 启动时窗口未必是前台, 焦点会先于光标进入半初始化
     * 状态且后续点击不触发 WM_SETFOCUS。详见 WndProc 的 WM_CREATE 注释。 */

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
