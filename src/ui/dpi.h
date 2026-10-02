/*
 * ui/dpi.h - 高 DPI 适配: per-monitor v2 感知 + 统一缩放 (dp)
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 *
 * 约定: **所有布局常量与字号都按 96 DPI 的逻辑像素书写, 一律经 dp() 换算成物理像素。**
 * 想调尺寸就改逻辑值, 不要再往布局里塞裸像素数字。
 *
 * 感知模式 = PER_MONITOR_AWARE_V2: 进程活在"窗口当前所在显示器"的 DPI 里, 每块屏都按原生
 * 分辨率绘制。代价是跨屏时必须自己重算 —— 系统发 WM_DPICHANGED, 收到后要做齐四步:
 *   1) 取该窗口的新 DPI
 *   2) 重建字体并下发给全部控件
 *   3) 重设 RichEdit 的精确行距(twips 是绝对值, 不跟着字体走就会裁字)
 *   4) 按系统给的建议位置 SetWindowPos, 再重新排布
 * 主窗口在 wndproc.h 里做齐了这四步; 两个对话框(about / session)也各有一份。
 * **漏掉任何一步的后果**: 界面继续按旧 DPI 的尺寸画在新屏上 —— 比"不感知"更糟(尺寸直接错)。
 *
 * 为什么 g_dpi 之外还有 dp_at():
 *   g_dpi 是**主窗口**所在显示器的 DPI。对话框可能被拖到别的显示器上(它们有自己的
 *   WM_DPICHANGED), 所以各自维护一份局部 DPI, 用 dp_at() 换算 —— 绝不能让对话框去写
 *   g_dpi, 否则主窗口下次 layout() 会用错比例。
 *
 * 降级: 拿不到 PMv2 就退到系统级感知(SetProcessDPIAware)。退化后 dpi_of_window() 读到的
 * 就是系统 DPI, 上述四步依然成立(只是 WM_DPICHANGED 不再触发), 两条路径共用同一套代码。
 *
 * **没有**把感知写进 res/app.manifest 的 <dpiAwareness>: 那样系统在建进程前就应用, 更可靠,
 * 但必须同时删掉这里的运行时调用(两边会打架), 且改动分散在两个文件。当前保持运行时设置。
 *
 * 历史 bug(别再犯): 这里原先传的是 -5 = UNAWARE_GDISCALED, 名字像"感知"实际**不感知** ——
 * 界面被整体位图放大, 且窗口物理尺寸随所在显示器变化(720x560 在 200% 屏是 1440x1120、
 * 250% 屏是 1800x1400)。那正是早期"本机缩放到底 200% 还是 250%"两次测量矛盾的根因。
 */

/* 取值来自 winuser.h 的 DPI_AWARENESS_CONTEXT:
 *   UNAWARE=-1  SYSTEM_AWARE=-2  PER_MONITOR_AWARE=-3  PER_MONITOR_AWARE_V2=-4
 * (另有 UNAWARE_GDISCALED=-5: 仍属"不感知", 只是系统会替 GDI 内容做缩放, 极易误判成"已感知") */
#define CAGENT_DPI_AWARENESS_PMV2 ((DPI_AWARENESS_CONTEXT)-4)

/* 界面统一字体族。放在这里是因为对话框要按**自己所在显示器**的 DPI 独立建字体,
 * 不能直接克隆主窗口的 g_hFont(那可能是别的 DPI 的尺寸)。 */
#define CAGENT_UI_FACE L"Microsoft YaHei UI"

static UINT g_dpi = 96;      /* 主窗口所在显示器的 DPI; dpi_init() 给初值, WM_CREATE 里定准 */

static int dp_at(UINT dpi, int v) { return MulDiv(v, (int)dpi, 96); }
static int dp(int v) { return dp_at(g_dpi, v); }

/* 窗口所在显示器的 DPI。per-monitor 下每块屏不同, 所以必须问**窗口**而不是问系统。 */
static UINT dpi_of_window(HWND h) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
        PFN_GetDpiForWindow f = (PFN_GetDpiForWindow)(void*)GetProcAddress(u, "GetDpiForWindow");
        if (f && h) { UINT d = f(h); if (d) return d; }
    }
    if (h) {   /* 回退: 用窗口自己的 DC 问 LOGPIXELSY, 同样是每显示器的值 */
        HDC hdc = GetDC(h);
        if (hdc) { UINT d = (UINT)GetDeviceCaps(hdc, LOGPIXELSY); ReleaseDC(h, hdc); if (d) return d; }
    }
    HDC hdc = GetDC(NULL);
    UINT d = (UINT)(hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96);
    if (hdc) ReleaseDC(NULL, hdc);
    return d ? d : 96;
}

/* 系统 DPI。只用作 dpi_init() 的初值 —— per-monitor 下它不等于窗口所在屏的 DPI。 */
static UINT dpi_system(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
        PFN_GetDpiForSystem f = (PFN_GetDpiForSystem)(void*)GetProcAddress(u, "GetDpiForSystem");
        if (f) { UINT d = f(); if (d) return d; }
    }
    HDC hdc = GetDC(NULL);
    UINT d = (UINT)(hdc ? GetDeviceCaps(hdc, LOGPIXELSY) : 96);
    if (hdc) ReleaseDC(NULL, hdc);
    return d ? d : 96;
}

/* 客户区 -> 外框, 按 dpi 反推。per-monitor 下必须用 ForDpi 版本:
 * 普通 AdjustWindowRectEx 用的是**系统** DPI 的边框度量, 在缩放比例不同的显示器上
 * 客户区会差几个像素。 */
static void dpi_adjust_rect(RECT *r, DWORD style, BOOL menu, DWORD exstyle, UINT dpi) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef BOOL (WINAPI *PFN_ForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
        PFN_ForDpi f = (PFN_ForDpi)(void*)GetProcAddress(u, "AdjustWindowRectExForDpi");
        if (f) { f(r, style, menu, exstyle, dpi); return; }
    }
    AdjustWindowRectEx(r, style, menu, exstyle);
}

/* 设定 DPI 感知并给 g_dpi 一个初值。**必须在创建任何窗口/字体/DC 之前调用**;
 * 真正的每显示器 DPI 由各窗口的 WM_CREATE / WM_DPICHANGED 用 dpi_of_window() 定。 */
static void dpi_init(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef BOOL (WINAPI *PFN_SetCtx)(DPI_AWARENESS_CONTEXT);
        PFN_SetCtx setCtx = (PFN_SetCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (setCtx) {
            /* 先试 V2; 失败再试 V1(-3, Win8.1 起的每显示器模式, 同样收 WM_DPICHANGED) */
            if (setCtx(CAGENT_DPI_AWARENESS_PMV2) ||
                setCtx((DPI_AWARENESS_CONTEXT)-3)) { g_dpi = dpi_system(); return; }
        }
        /* 最后兜底: 系统级感知(Vista+)。退化后 WM_DPICHANGED 不来, 其余逻辑不变 */
        typedef BOOL (WINAPI *PFN_SetAware)(void);
        PFN_SetAware setAware = (PFN_SetAware)(void*)GetProcAddress(u, "SetProcessDPIAware");
        if (setAware) setAware();
    }
    g_dpi = dpi_system();
}
