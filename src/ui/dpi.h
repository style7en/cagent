/*
 * ui/dpi.h - 高 DPI 适配: 统一缩放 (dp) + 每显示器 DPI 查询
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 *
 * 约定: **所有布局常量与字号都按 96 DPI 的逻辑像素书写, 一律经 dp() / dp_at() 换算成
 * 物理像素。** 想调尺寸就改逻辑值, 不要再往布局里塞裸像素数字。
 *
 * 感知模式 (PerMonitorV2) **不在这里设** —— 它声明在 res/app.manifest:
 *     <dpiAwareness>PerMonitorV2,PerMonitor</dpiAwareness>   (2016 命名空间)
 *     <dpiAware>true/pm</dpiAware>                            (2005 命名空间, Win8.1 起)
 * 由系统在**建进程之前**应用。为什么放清单而不是运行时调用:
 *   ① 更可靠 —— 没有"调用失败被静默忽略, 进程其实还是不感知"这种坑。历史上正是这么栽的:
 *      代码里传的是 -5 = UNAWARE_GDISCALED, 名字像"感知"实际是**不感知**, 界面被整体位图
 *      放大, 且窗口物理尺寸随所在显示器变化(720x560 在 200% 屏是 1440x1120、250% 屏是
 *      1800x1400)—— 那正是早期"本机缩放到底 200% 还是 250%"两次测量矛盾的根因。
 *   ② 早于任何代码/窗口/字体, 没有调用顺序的讲究;
 *   ③ 本文件因此省掉约 25 行动态 GetProcAddress 的降级链。
 * 代价: 运行时不能再改(这是好事); Win7 及更早不认识这两项, 会退回"不感知"(尺寸对、略糊)。
 *
 * per-monitor 的代价是跨屏必须自己重算 —— 系统发 WM_DPICHANGED, 收到后四步缺一不可:
 *   1) 取该窗口的新 DPI
 *   2) 重建字体并下发给全部控件
 *   3) 重设 RichEdit 的精确行距(twips 是绝对值, 不跟着字体走就会裁字)
 *   4) 按系统给的建议位置 SetWindowPos, 再重新排布
 * 主窗口在 wndproc.h 里做齐了这四步; 两个对话框(about / session)也各有一份。
 * **漏掉任何一步**: 界面继续按旧 DPI 的尺寸画在新屏上 —— 比"不感知"更糟(尺寸直接错)。
 *
 * 为什么 g_dpi 之外还有 dp_at():
 *   g_dpi 是**主窗口**所在显示器的 DPI。对话框可能被拖到别的显示器上(它们有自己的
 *   WM_DPICHANGED), 所以各自维护一份局部 DPI, 用 dp_at() 换算 —— 绝不能让对话框去写
 *   g_dpi, 否则主窗口下次 layout() 会用错比例。
 */

/* 界面统一字体族。放这里是因为对话框要按**自己所在显示器**的 DPI 独立建字体,
 * 不能直接克隆主窗口的 g_hFont(那可能是别的 DPI 的尺寸)。 */
#define CAGENT_UI_FACE L"Microsoft YaHei UI"

static UINT g_dpi = 96;      /* 主窗口所在显示器的 DPI; dpi_init() 给初值, WM_CREATE 里定准 */

static int dp_at(UINT dpi, int v) { return MulDiv(v, (int)dpi, 96); }
static int dp(int v) { return dp_at(g_dpi, v); }

/* 系统 DPI(主屏)。只在 dpi_init() 里取一次当初值, 也充当 dpi_of_window() 的最后兜底。
 * 注意: 不感知的进程问出来恒为 96 —— 所以它依赖清单里的 dpiAwareness 已经生效。 */
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

/* 窗口所在显示器的 DPI。per-monitor 下每块屏不同, 所以必须问**窗口**而不是问系统。 */
static UINT dpi_of_window(HWND h) {
    if (h) {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) {
            typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
            PFN_GetDpiForWindow f = (PFN_GetDpiForWindow)(void*)GetProcAddress(u, "GetDpiForWindow");
            if (f) { UINT d = f(h); if (d) return d; }
        }
        HDC hdc = GetDC(h);   /* 老系统回退: 窗口 DC 的 LOGPIXELSY 同样是每显示器的值 */
        if (hdc) { UINT d = (UINT)GetDeviceCaps(hdc, LOGPIXELSY); ReleaseDC(h, hdc); if (d) return d; }
    }
    return dpi_system();
}

/* 客户区 -> 外框, 按 dpi 反推。必须用 ForDpi 版本:
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

/* 给 g_dpi 一个初值, 让第一帧的窗口大小大致正确 —— 真正的每显示器 DPI 由 WM_CREATE /
 * WM_DPICHANGED 用 dpi_of_window() 定。**DPI 感知本身不在这里设**, 见文件头。 */
static void dpi_init(void) { g_dpi = dpi_system(); }
