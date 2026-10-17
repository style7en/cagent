/*
 * test/ui_flicker_probe.c - 诊断: 点击折叠/缩放时输出区为什么闪
 *
 * 背景 (用户报告): 点击折叠块标题 (展开/收起) 与 Ctrl+滚轮缩放时, 输出区会闪。
 *
 * RichEdit 没有双缓冲: 文本/段落格式的每一次变更都可能同步触发一次重绘, 而界面
 * 的每一次 InvalidateRect(TRUE) 又会带来一次"擦背景 + 整屏重画"。这个探针给历史
 * 框套一层计数子类, 数一数一次展开/一次缩放到底触发多少次 WM_PAINT / WM_ERASEBKGND
 * —— 中间态被逐帧刷上屏, 用户看到的就是"闪"。
 *
 * 验证工具, 不进产品二进制。
 *
 * 用法: gcc -Wall -Isrc -o ui_flicker_probe.exe test/ui_flicker_probe.c \
 *          -lcomctl32 -lwinhttp -lcrypt32 -lshell32 -lole32 -luuid -lgdi32 -luser32
 *       ./ui_flicker_probe.exe <session.json>
 */

#define PROBE_MAIN
#include "../src/main_gui.c"

/* ---- 与产品一致的追加入口 (预览程序没有消息循环, 同步直调 do_append) ---- */
static void do_append_wrapper(const char *utf8, int role) { do_append(utf8, role); }

/* 打开 DPI 感知 (与 ui_preview.c 同一份): 否则 GDI 按 96 DPI 虚拟化, 量出来的坐标都失真 */
static void enable_dpi_awareness(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
    PFN_SetCtx f = (PFN_SetCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (f) { f((HANDLE)-4); return; }          /* -4 = PER_MONITOR_AWARE_V2 */
    typedef BOOL (WINAPI *PFN_SetAw)(void);
    PFN_SetAw g = (PFN_SetAw)(void*)GetProcAddress(u, "SetProcessDPIAware");
    if (g) g();
}

/* ---- 重绘计数子类: 套在产品 HistoryProc 外层, 只数消息不改行为 ---- */
static int g_paint_n, g_erase_n;
static WNDPROC g_paint_wrap_prev;

static LRESULT CALLBACK PaintCountProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        g_paint_n++;
        LRESULT r = CallWindowProcW(g_paint_wrap_prev, h, msg, wp, lp);
        /* 画完后取右下角留白区的像素: 不擦背景时它是不是还是 HIST_BG?
         * 是 -> RichEdit 在 WM_PAINT 里自己填背景, 修复时可以放心去掉 erase */
        RECT rc; GetClientRect(h, &rc);
        HDC hdc = GetDC(h);
        if (hdc) {
            COLORREF c = GetPixel(hdc, rc.right - 6, rc.bottom - 6);
            printf("    after-paint corner %06lX (HIST_BG=%06lX) %s\n",
                   (unsigned long)(c & 0xFFFFFF), (unsigned long)(HIST_BG & 0xFFFFFF),
                   (c & 0xFFFFFF) == (HIST_BG & 0xFFFFFF) ? "[bg ok]" : "[NO BG FILL!]");
            ReleaseDC(h, hdc);
        }
        return r;
    }
    else if (msg == WM_ERASEBKGND) {
        g_erase_n++;
        /* 擦出来的是什么色? 与 HIST_BG 不符的话, 那一帧用户看到的就是错色整屏 */
        LRESULT r = CallWindowProcW(g_paint_wrap_prev, h, msg, wp, lp);
        if (wp) {
            COLORREF c = GetPixel((HDC)wp, 4, 4);
            printf("    erase filled %06lX (HIST_BG=%06lX) %s\n",
                   (unsigned long)(c & 0xFFFFFF), (unsigned long)(HIST_BG & 0xFFFFFF),
                   (c & 0xFFFFFF) == (HIST_BG & 0xFFFFFF) ? "[match]" : "[MISMATCH!]");
        }
        return r;
    }
    return CallWindowProcW(g_paint_wrap_prev, h, msg, wp, lp);
}

static void measure(const char *tag, void (*fn)(void)) {
    g_paint_n = 0;
    g_erase_n = 0;
    fn();
    printf("%-34s WM_PAINT=%-4d WM_ERASEBKGND=%-4d\n", tag, g_paint_n, g_erase_n);
}

/* 每次开新段落前: 复刻 sess_apply_selected 对历史框的重置 (与 ui_gap_probe.c 同一份) */
static void hist_reset(void) {
    SetWindowTextW(g_hHistory, L"");
    g_last_role = -1;
    g_last_no_lf = 0;
    ui_fold_reset();
    ui_history_parafmt(g_hHistory);
    ui_history_zoom_apply(g_hHistory);
}

/* ---- 被测动作 ---- */
static int g_target_fold = -1;

/* 把消息队列里积压的 WM_PAINT 全部派发掉 (真实应用有消息循环, 延迟的重绘会在这里发生) */
static void pump_paints(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

/* 分阶段计数的展开: 复刻 ui_fold_toggle 的三步 (取滚动位 -> 改内容 -> 恢复滚动位),
 * 数一数重绘到底发生在哪一步 —— 落在"改内容"和"恢复滚动位"之间的那次, 用户看到的就是
 * 滚动位置没恢复的中间态 (视图先跳一下再回去), 这就是"闪"的一种。 */
static void act_fold_expand_phased(void) {
    g_paint_n = 0; g_erase_n = 0;
    POINT scroll;
    memset(&scroll, 0, sizeof(scroll));
    SendMessageW(g_hHistory, EM_GETSCROLLPOS, 0, (LPARAM)&scroll);
    ui_fold_set(g_target_fold, 0);
    int during_set = g_paint_n;
    SendMessageW(g_hHistory, EM_SETSCROLLPOS, 0, (LPARAM)&scroll);
    int during_restore = g_paint_n - during_set;
    pump_paints();
    printf("  expand: paint during set=%d, during scroll-restore=%d, deferred=%d (erase=%d)\n",
           during_set, during_restore, g_paint_n - during_set - during_restore, g_erase_n);
}

static void act_fold_collapse_phased(void) {
    g_paint_n = 0; g_erase_n = 0;
    POINT scroll;
    memset(&scroll, 0, sizeof(scroll));
    SendMessageW(g_hHistory, EM_GETSCROLLPOS, 0, (LPARAM)&scroll);
    ui_fold_set(g_target_fold, 1);
    int during_set = g_paint_n;
    SendMessageW(g_hHistory, EM_SETSCROLLPOS, 0, (LPARAM)&scroll);
    int during_restore = g_paint_n - during_set;
    pump_paints();
    printf("  collapse: paint during set=%d, during scroll-restore=%d, deferred=%d (erase=%d)\n",
           during_set, during_restore, g_paint_n - during_set - during_restore, g_erase_n);
}

static void act_fold_expand(void)  { ui_fold_toggle(g_target_fold); pump_paints(); }
static void act_fold_collapse(void) { ui_fold_toggle(g_target_fold); pump_paints(); }
static void act_zoom(void)         { ui_history_zoom(g_hHistory, 2); pump_paints(); }
static void act_zoom_back(void)    { ui_history_zoom(g_hHistory, -2); pump_paints(); }

/* 缩放分阶段: EM_SETZOOM 自己那次重绘 / InvalidateRect 的延迟重绘 各占多少 */
static void act_zoom_phased(void) {
    g_paint_n = 0; g_erase_n = 0;
    int before = g_hist_lpx;
    g_hist_lpx = before + 2;
    ui_history_zoom_apply(g_hHistory);          /* EM_SETZOOM */
    int during_setzoom = g_paint_n;
    pump_paints();
    printf("  zoom: paint during EM_SETZOOM=%d, deferred=%d (erase=%d)\n",
           during_setzoom, g_paint_n - during_setzoom, g_erase_n);
    g_hist_lpx = before;
    ui_history_zoom_apply(g_hHistory);
    pump_paints();
}

/* 不擦背景的直接重画 (修复方案的核心): 走 RDW_INVALIDATE|RDW_UPDATENOW, 不带 erase */
static void act_noerase_paint(void) {
    g_paint_n = 0; g_erase_n = 0;
    RedrawWindow(g_hHistory, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
    pump_paints();
    printf("  no-erase repaint: paint=%d erase=%d\n", g_paint_n, g_erase_n);
}

int main(int argc, char **argv) {
    const char *session = argc > 1 ? argv[1] : "sessions/history_test.json";
    enable_dpi_awareness();
    LoadLibraryW(L"Msftedit.dll");
    dpi_init();
    g_dpi = dpi_system();

    int W = dp(720), H = dp(500);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"cagent_flicker_wnd";
    if (!RegisterClassW(&wc)) { printf("RegisterClass failed\n"); return 1; }

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"cagent flicker probe", WS_POPUP,
                              40, 40, W, H, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("CreateWindow failed\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    int status_h = dp(22);
    g_hHistory = CreateWindowExW(0, L"RICHEDIT50W", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, W, H - status_h, hwnd, (HMENU)(LONG_PTR)ID_HISTORY, NULL, NULL);
    if (!g_hHistory) { printf("CreateWindow RichEdit failed\n"); return 1; }
    g_hStatus = CreateWindowW(L"STATIC", L"ready", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_STATUS, NULL, NULL);
    MoveWindow(g_hStatus, dp(8), H - status_h, W - dp(16), status_h, TRUE);
    g_oldHistoryProc = (WNDPROC)SetWindowLongPtrW(g_hHistory, GWLP_WNDPROC, (LONG_PTR)HistoryProc);
    /* 计数子类套在最外层 */
    g_paint_wrap_prev = (WNDPROC)SetWindowLongPtrW(g_hHistory, GWLP_WNDPROC, (LONG_PTR)PaintCountProc);

    ui_fonts_rebuild(hwnd);
    SendMessageW(g_hHistory, EM_SETBKGNDCOLOR, 0, (LPARAM)(DWORD)HIST_BG);
    ui_history_parafmt(g_hHistory);
    ui_history_pad(g_hHistory, W, H - status_h);

    if (!history_load_from_file(session)) { printf("history_load_from_file failed\n"); return 1; }
    cagent_emit = do_append_wrapper;

    /* 回放出带折叠块的会话 (与产品载入历史同一路径) */
    hist_reset();
    if (!history_load_from_file(session)) return 1;
    emit_note("(loaded history)\r\n");
    history_replay();

    printf("fold blocks: %d\n", g_fold_n);
    for (int i = 0; i < g_fold_n; i++) {
        printf("  block %d: segs=%d body_len=%ld collapsed=%d %s\n",
               i, g_folds[i].n_segs, g_folds[i].body_len,
               g_folds[i].collapsed,
               g_folds[i].collapsed ? "" : "(expanded)");
    }

    /* 挑正文最大的已收起块: 展开它要逐段插回全部内容, 是最能暴露重绘次数的操作 */
    {
        long best = -1;
        for (int i = 0; i < g_fold_n; i++)
            if (g_folds[i].n_segs > 0 && g_folds[i].collapsed &&
                g_folds[i].body_len > best) { best = g_folds[i].body_len; g_target_fold = i; }
    }
    if (g_target_fold < 0) { printf("no collapsible block with body\n"); return 1; }
    printf("target block: %d (segs=%d body_len=%ld)\n\n",
           g_target_fold, g_folds[g_target_fold].n_segs, g_folds[g_target_fold].body_len);

    /* 展开 -> 再收起, 各测一次 (整段点击路径 = 含消息泵后的总重绘次数) */
    measure("fold expand (1 click)", act_fold_expand);
    measure("fold collapse (1 click)", act_fold_collapse);
    /* 分阶段: 重绘发生在"改内容"还是"恢复滚动位"之间 */
    act_fold_expand_phased();
    act_fold_collapse_phased();
    /* 缩放, 放大一格再缩回 */
    measure("zoom +1 step (Ctrl+wheel)", act_zoom);
    measure("zoom -1 step (Ctrl+wheel)", act_zoom_back);
    act_zoom_phased();
    act_noerase_paint();

    return 0;
}
