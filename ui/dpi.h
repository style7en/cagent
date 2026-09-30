/*
 * ui/dpi.h - 高 DPI 适配 (三级降级)
 *
 * cagent 界面层的一部分, 由 cagent.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== DPI 适配 ===== */
static void enable_dpi_awareness(void) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) return;

    typedef DPI_AWARENESS_CONTEXT (WINAPI *PFN_SetCtx)(DPI_AWARENESS_CONTEXT);
    PFN_SetCtx pSetCtx = (PFN_SetCtx)(void*)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
    if (pSetCtx) {
        if (pSetCtx((DPI_AWARENESS_CONTEXT)-5)) return;
    }

    typedef HRESULT (WINAPI *PFN_SetAwareness)(int);
    PFN_SetAwareness pSetAwareness = (PFN_SetAwareness)(void*)GetProcAddress(hUser32, "SetProcessDpiAwareness");
    if (pSetAwareness) {
        pSetAwareness(1);
        return;
    }

    typedef BOOL (WINAPI *PFN_SetAware)(void);
    PFN_SetAware pSetAware = (PFN_SetAware)(void*)GetProcAddress(hUser32, "SetProcessDPIAware");
    if (pSetAware) pSetAware();
}
