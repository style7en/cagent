/*
 * ui/main.h - 程序入口: 注册窗口类 / 图标 / 消息循环
 *
 * cagent 界面层的一部分, 由 cagent_ui.c 按依赖顺序聚合 (单 TU, 全 static)。
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
    if (cmd && strstr(cmd, "--selftest")) {
        freopen("selftest.txt", "w", stdout);
        return run_all_tests();   /* 覆盖 JSON/编码/工具/HTTP 的全套回归 */
    }

    /* 安装宿主钩子 (必须在 agent 运行前设置) */
    cagent_emit = gui_emit;
    cagent_on_done = gui_on_done;
    cagent_read_config_ui = gui_read_config_ui;
    cagent_stream_begin = gui_stream_begin;
    cagent_stream_undo = gui_stream_undo;

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

    HWND hwnd = CreateWindowW(L"CagentGuiWnd", L"cagent " CAGENT_VERSION_WSTR,
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
    /* 不在此 SetFocus(输入框): 启动时窗口未必是前台, 焦点会先于光标进入半初始化
     * 状态且后续点击不触发 WM_SETFOCUS。详见 WndProc 的 WM_CREATE 注释。 */

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
