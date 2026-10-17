/*
 * ui_cue_probe.c - 验证接口地址输入框的 EM_SETCUEBANNER 占位提示 (诊断工具)
 *
 * 三种模式:
 *   edit   在独立 EDIT 控件上跑消息回环 (SET -> GET 逐字一致, **同进程**才是可信读回)
 *          + 把控件客户端渲染成 BMP 供人眼确认灰字确实画出来
 *   spawn  自己拉起一个 cagent 实例, 按 pid 认窗口, 截窗口位图 + 在 URL 框内数灰字像素
 *          判定。**跨进程 EM_GETCUEBANNER 不可信**: 实测画面已渲染而读回恒 0, 只作
 *          参考; 真机判定走像素。需 comctl v6 manifest + DPI 感知 (坐标要一致)。
 *   shot   对已在运行的 CagentGuiWnd 窗口做同样的读回 + 截图
 *
 * 编译: gcc -Wall -o ui_cue_probe.exe test/ui_cue_probe.c -luser32 -lgdi32 -mwindows
 *       (需带 comctl v6 manifest: windres 编 test/cue_probe.rc 一起链 —— EM_SETCUEBANNER
 *        在无 v6 清单的进程里会直接返回 FALSE 拒收, 与产品 res/app.rc 同理)
 * 运行: ./ui_cue_probe.exe edit                      独立控件消息回环
 *       ./ui_cue_probe.exe spawn <exe>               拉起实例, 按 pid 认窗口, 读回+截图+回收
 *       ./ui_cue_probe.exe shot                      对已在运行的窗口 (类名 CagentGuiWnd) 读回+截图
 */

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

/* 与 src/ui/wndproc.h 里 EM_SETCUEBANNER 传入的提示文字一致;
 * 这是断言的"期望值" —— drift 会被下面两条检查同时抓到 */
static const WCHAR *EXPECTED =
    L"\u4ec5\u652f\u6301 OpenAI \u517c\u5bb9\u534f\u8bae API, \u5982 https://api.example.com/v1";

static int save_bmp32(const char *path, int w, int h, const void *bits) {
    BITMAPFILEHEADER bf;
    BITMAPINFOHEADER bi;
    FILE *f;
    memset(&bf, 0, sizeof(bf));
    memset(&bi, 0, sizeof(bi));
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = -h;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    bf.bfType = 0x4D42;
    bf.bfOffBits = sizeof(bf) + sizeof(bi);
    f = fopen(path, "wb");
    if (!f) return 0;
    if (fwrite(&bf, 1, sizeof(bf), f) != sizeof(bf) || \
        fwrite(&bi, 1, sizeof(bi), f) != sizeof(bi) || \
        fwrite(bits, 1, (size_t)w * h * 4, f) != (size_t)w * h * 4) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

static void render_hwnd_to_bmp(HWND h, const char *path) {
    RECT rc;
    GetWindowRect(h, &rc);
    int w = rc.right - rc.left, hh = rc.bottom - rc.top;
    HDC hdc = GetDC(h);
    if (!hdc || w <= 0 || hh <= 0) { printf("  render: bad rect %dx%d\n", w, hh); if (hdc) ReleaseDC(h, hdc); return; }
    void *bits = NULL;
    HDC mem = CreateCompatibleDC(hdc);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -hh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) { printf("  render: DIB alloc failed\n"); goto done; }
    {
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, hh, hdc, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        if (save_bmp32(path, w, hh, bits)) printf("  render: saved %s (%dx%d)\n", path, w, hh);
        else printf("  render: write failed\n");
    }
    DeleteObject(bmp);
done:
    if (mem) DeleteDC(mem);
    ReleaseDC(h, hdc);
}

/* ---- edit 模式: 独立控件复刻配置区同款 EDIT ---- */
static LRESULT CALLBACK probe_wnd(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, wp, lp);
}

static int mode_edit(void) {
    const wchar_t CN[] = L"CagentCueProbeWnd";
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = probe_wnd;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = CN;
    if (!RegisterClassW(&wc)) { printf("RegisterClass failed\n"); return 1; }
    /* 192 DPI 下放得下: 300x80 逻辑像素 = 600x160 物理 */
    HWND wnd = CreateWindowExW(0, CN, L"cue probe", WS_OVERLAPPEDWINDOW,
                               100, 100, 300, 80, NULL, NULL, wc.hInstance, NULL);
    if (!wnd) { printf("CreateWindow failed\n"); return 1; }
    ShowWindow(wnd, SW_SHOW);
    UpdateWindow(wnd);

    /* 与配置区一致: WS_EX_CLIENTEDGE + ES_AUTOHSCROLL + 空串, 字体随手给一个非默认值 */
    HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                              10, 10, 260, 24, wnd, NULL, wc.hInstance, NULL);
    if (!ed) { printf("CreateWindow EDIT failed\n"); return 1; }

    /* 与产品里同一款调用: Edit_SetCueBannerText 宏 (wParam=0, 聚焦即隐藏) */
    LRESULT r = Edit_SetCueBannerText(ed, EXPECTED);
    printf("EM_SETCUEBANNER -> %ld (%s)\n", (long)r, r ? "accepted" : "REJECTED");

    WCHAR buf[256] = {0};
    LRESULT g = SendMessageW(ed, EM_GETCUEBANNER, (WPARAM)buf, (LPARAM)256);
    int same = (g && wcscmp(buf, EXPECTED) == 0);
    printf("EM_GETCUEBANNER -> %ld, text %s\n", (long)g, same ? "MATCH" : "MISMATCH!");
    if (!same) printf("  got: %ls\n  want: %ls\n", buf, EXPECTED);

    /* 把控件客户端画出来: 空串 + cue 应该渲染成灰字 */
    RedrawWindow(ed, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    render_hwnd_to_bmp(ed, "cue_probe_edit.bmp");

    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    DestroyWindow(wnd);
    return same && r ? 0 : 1;
}

/* ---- shot 模式: 真机窗口读回 + 截图 ---- */
static DWORD g_spawn_pid;

static BOOL CALLBACK find_by_pid(HWND h, LPARAM lp) {
    DWORD pid = 0;
    WCHAR cls[64] = {0};
    GetWindowThreadProcessId(h, &pid);
    GetClassNameW(h, cls, 64);
    if (pid == g_spawn_pid && wcscmp(cls, L"CagentGuiWnd") == 0 && IsWindowVisible(h)) {
        *(HWND *)lp = h;
        return FALSE;
    }
    return TRUE;
}

/* ---- spawn 模式: 自己拉起一个实例, 在**同一进程内**完成读回+截图+回收 ---- */
static int mode_spawn(const char *exe) {
    WCHAR wexe[512];
    MultiByteToWideChar(CP_UTF8, 0, exe, -1, wexe, 512);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessW(wexe, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("CreateProcess failed: %lu\n", GetLastError());
        return 1;
    }
    g_spawn_pid = pi.dwProcessId;
    WaitForInputIdle(pi.hProcess, 8000);
    Sleep(500);   /* 等 WM_CREATE 整个跑完 (cue 设置在其中) */

    HWND wnd = NULL;
    for (int i = 0; i < 25 && !wnd; i++) {
        EnumWindows(find_by_pid, (LPARAM)&wnd);
        if (!wnd) Sleep(200);
    }
    if (!wnd) {
        printf("spawned window (pid %lu) not found\n", g_spawn_pid);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return 1;
    }
    {
        DWORD st = WaitForSingleObject(pi.hProcess, 0);
        printf("app alive after find: %s\n", st == WAIT_TIMEOUT ? "yes" : "NO");
    }
    RECT rc;
    GetWindowRect(wnd, &rc);
    printf("window pid=%lu rect=(%ld,%ld,%ld,%ld)\n", g_spawn_pid,
           rc.left, rc.top, rc.right, rc.bottom);

    HWND ed = GetDlgItem(wnd, 1005);
    if (!ed) { printf("cfg url edit (1005) not found\n"); }
    else {
        /* 截图 + 在 URL 框内数"深灰像素" —— cue 灰字(实测 (87,87,87))是判定依据。
         * **不能用 EM_GETCUEBANNER 做跨进程断言**: 实测同进程读回正常, 跨进程恒返回 0
         * (窗口明明是渲染出来的), 拿它判定会误报。GET 只作信息打印。 */
        RECT rc, erc;
        GetWindowRect(wnd, &rc);
        GetWindowRect(ed, &erc);
        int W = rc.right - rc.left, H = rc.bottom - rc.top;
        int ex = erc.left - rc.left + 6, ey = erc.top - rc.top + 6;
        int ew = (erc.right - rc.left) - 6 - ex, eh = (erc.bottom - rc.top) - 6 - ey;
        HDC hdc = GetWindowDC(wnd);
        void *bits = NULL;
        HDC mem = NULL;
        HBITMAP bmp = NULL;
        long gray = 0;
        int cap_ok = 0;
        if (hdc && W > 0 && H > 0) {
            BITMAPINFO bi;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = W;
            bi.bmiHeader.biHeight = -H;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            mem = CreateCompatibleDC(hdc);
            bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (mem && bmp && bits) {
                HGDIOBJ old = SelectObject(mem, bmp);
                BitBlt(mem, 0, 0, W, H, hdc, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                cap_ok = 1;
                if (save_bmp32("cue_probe_spawn.bmp", W, H, bits))
                    printf("render: saved cue_probe_spawn.bmp (%dx%d)\n", W, H);
                for (int y = ey; y < ey + eh && y < H; y++) {
                    BYTE *row = (BYTE *)bits + (size_t)y * W * 4;
                    for (int x = ex; x < ex + ew && x < W; x++) {
                        BYTE b = row[x * 4], g2 = row[x * 4 + 1], r = row[x * 4 + 2];
                        int mx = r > g2 ? (r > b ? r : b) : (g2 > b ? g2 : b);
                        int mn = r < g2 ? (r < b ? r : b) : (g2 < b ? g2 : b);
                        if (mx >= 60 && mx <= 130 && mx - mn <= 8) gray++;
                    }
                }
            }
        }
        if (hdc) ReleaseDC(wnd, hdc);
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);

        WCHAR txt[256] = {0}, cue[256] = {0};
        GetWindowTextW(ed, txt, 256);
        LRESULT g = SendMessageW(ed, EM_GETCUEBANNER, (WPARAM)cue, (LPARAM)256);
        printf("url_empty=%d, cue gray px in url edit=%ld -> %s (cross-proc EM_GETCUEBANNER=%ld, 仅参考)\n",
               !txt[0], gray, (cap_ok && gray > 100) ? "RENDERED [OK]" : "NOT RENDERED [FAIL]", (long)g);
    }

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}

static int mode_shot(void) {
    HWND wnd = FindWindowW(L"CagentGuiWnd", NULL);
    if (!wnd) { printf("CagentGuiWnd not running\n"); return 1; }
    RECT rc; GetWindowRect(wnd, &rc);
    printf("window rect: (%ld,%ld)-(%ld,%ld)\n", rc.left, rc.top, rc.right, rc.bottom);

    /* 接口地址框 = 配置区第 0 个 EDIT (ID_CFG_BASE+0 = 1005) */
    HWND ed = GetDlgItem(wnd, 1005);
    if (!ed) { printf("cfg url edit (1005) not found\n"); return 1; }
    WCHAR buf[256] = {0};
    LRESULT g = SendMessageW(ed, EM_GETCUEBANNER, (WPARAM)buf, (LPARAM)256);
    int same = (g && wcscmp(buf, EXPECTED) == 0);
    printf("real window cue -> %ld, text %s\n", (long)g, same ? "MATCH" : "MISMATCH!");
    if (!same) printf("  got: %ls\n  want: %ls\n", buf, EXPECTED);

    /* 顺带确认框里当前是空的 (提示只在空串时显示) */
    WCHAR txt[256] = {0};
    GetWindowTextW(ed, txt, 256);
    printf("url edit text empty: %ls\n", txt[0] ? L"NO" : L"yes");

    render_hwnd_to_bmp(wnd, "cue_probe_app.bmp");
    return same ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "spawn") == 0 && argc > 2) return mode_spawn(argv[2]);
    if (argc > 1 && strcmp(argv[1], "shot") == 0) return mode_shot();
    return mode_edit();
}
