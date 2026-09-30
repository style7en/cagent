/*
 * ui/main.h - 程序入口: 注册窗口类 / 图标 / 消息循环
 *
 * cagent 界面层的一部分, 由 cagent_ui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

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
