/*
 * ui/dpi.h - 高 DPI 适配: 统一缩放 (dp) 与 DPI 感知设定
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 *
 * 约定: **所有布局常量与字号都按 96 DPI 的逻辑像素书写, 一律经 dp() 换算成物理像素。**
 * 想调尺寸就改逻辑值, 不要再往布局里塞裸像素数字。
 *
 * 当前采用 **系统级** DPI 感知 (DPI_AWARENESS_SYSTEM_AWARE):
 *   - 缩放比例与主屏一致时(绝大多数场景), 界面按原生分辨率绘制 => 清晰;
 *   - 拖到缩放比例不同的显示器时由系统位图缩放: 尺寸正确、略糊 —— 与"完全不感知"相比
 *     只会更好, 不会更差。
 * 若日后要升级到 PER_MONITOR_AWARE_V2, 除了改这里的常量, **必须**同时在 wndproc.h 里
 * 处理 WM_DPICHANGED(更新 g_dpi -> 重建字体 -> 重新布局), 否则窗口在副屏上尺寸会算错。
 */

/* 取值来自 winuser.h 的 DPI_AWARENESS_CONTEXT:
 *   UNAWARE=-1  SYSTEM_AWARE=-2  PER_MONITOR_AWARE=-3  PER_MONITOR_AWARE_V2=-4 */
#define CAGENT_DPI_AWARENESS_SYSTEM ((DPI_AWARENESS_CONTEXT)-2)

static UINT g_dpi = 96;                        /* 系统 DPI; 由 dpi_init() 设定 */

/* 96 DPI 逻辑像素 -> 物理像素 */
static int dp(int v) { return MulDiv(v, (int)g_dpi, 96); }

/* 系统 DPI: 优先 GetDpiForSystem (Win10 1607+), 回退 GetDeviceCaps。
 * 注意返回值随进程的 DPI 感知模式而变, 所以必须在设定感知**之后**再调。 */
static UINT dpi_system(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef UINT (WINAPI *PFN_GetDpiForSystem)(void);
        PFN_GetDpiForSystem f = (PFN_GetDpiForSystem)(void*)GetProcAddress(u, "GetDpiForSystem");
        if (f) { UINT d = f(); if (d) return d; }
    }
    HDC hdc = GetDC(NULL);
    UINT d = (UINT)(hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96);
    if (hdc) ReleaseDC(NULL, hdc);
    return d ? d : 96;
}

/* 设定 DPI 感知并记录 g_dpi。**必须在创建任何窗口/字体/DC 之前调用**。 */
static void dpi_init(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        typedef BOOL (WINAPI *PFN_SetCtx)(DPI_AWARENESS_CONTEXT);
        PFN_SetCtx setCtx = (PFN_SetCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        if (setCtx && setCtx(CAGENT_DPI_AWARENESS_SYSTEM)) { g_dpi = dpi_system(); return; }

        /* Win7/Vista 回退: SetProcessDPIAware 给的就是系统级感知, 与上面等价 */
        typedef BOOL (WINAPI *PFN_SetAware)(void);
        PFN_SetAware setAware = (PFN_SetAware)(void*)GetProcAddress(u, "SetProcessDPIAware");
        if (setAware) setAware();
    }
    g_dpi = dpi_system();
}

/* 历史(踩过的坑, 别再犯): 这里原先传的是 -5 = UNAWARE_GDISCALED, 名字看着像"感知",
 * 实际是 **不感知** —— 整个界面被系统位图放大。后果有两个:
 *   ① 高缩放显示器上文字/边框发虚(被拉伸);
 *   ② 窗口尺寸随所在显示器变化: 同一份 720x560 的请求, 在 200% 屏上是 1440x1120、
 *      250% 屏上是 1800x1400 —— 这也是早期两次测量"本机缩放到底是 200% 还是 250%"
 *      结果对不上的真正原因(不是测量误差, 是窗口落在了不同显示器上)。
 */
