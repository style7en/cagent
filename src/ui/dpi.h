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

/* 等宽字体族, 用于工具参数与工具输出。选 Consolas 是因为它从 Vista 起随系统分发
 * (不引入额外依赖), 且中英文混排时数字与 ASCII 宽度一致 —— 看目录树、看 diff 时对齐。
 * 中文没有等宽字形, 会由 RichEdit 的字体绑定回退到系统字体, 这没关系:
 * 这些块里主要是路径、命令、代码, 中文占比很低。 */
#define CAGENT_MONO_FACE L"Consolas"

/* ASCII 表格专用字体。表格由模型用空格数按"中文占 2 列、ASCII 占 1 列"排好,
 * 所以字体必须让中文宽度严格等于 2 个 ASCII 宽度。实测 RichEdit 里 Consolas
 * 没有中文字形, 回退后的中文约 1.8 个 ASCII 格宽, 表格会逐列向左漂移; NSimSun
 * 的中文恰好是 2 格, 且中文 Windows 自带。 */
#define CAGENT_TABLE_FACE L"NSimSun"

/* 界面字体渲染质量。**别再改回 ANTIALIASED_QUALITY** —— 那是"灰度抗锯齿", 在 100%
 * (96 DPI) 下笔画边缘只有灰阶过渡, 观感发虚发糊, 正是 issue #1 的成因。
 *
 * 选 DEFAULT_QUALITY 而不是 CLEARTYPE_QUALITY 的理由:
 *   - 实测(系统 ClearType 开启)两者渲染结果**逐像素完全相同** —— 都是 577 个墨点、
 *     其中 484 个次像素着色点(R!=G!=B); 而 ANTIALIASED_QUALITY 的次像素着色点是 0
 *     (纯灰)。也就是说 DEFAULT 已经能拿到 ClearType 的清晰度。
 *   - DEFAULT_QUALITY 交给系统按**用户自己的**字体平滑设置去选: 用户若在系统里关了
 *     ClearType, 界面会跟着关; 而 CLEARTYPE_QUALITY 会强行打开, 反而违背用户意愿。
 * 用统一常量而不是各建字体处各写一遍, 就是为了避免将来又有人改回去。 */
#define CAGENT_FONT_QUALITY DEFAULT_QUALITY

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
