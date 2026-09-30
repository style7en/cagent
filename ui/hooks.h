/*
 * ui/hooks.h - 宿主钩子实现: 输出投递 / 一轮结束 / 配置同步
 *
 * cagent 界面层的一部分, 由 cagent_ui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== 宿主钩子实现 ===== */

/* 输出: 复制到堆, 通过 PostMessage 投递到 UI 线程 (线程安全)。
 * wParam 携带角色 (CAGENT_ROLE_*), 供历史框按角色着色。 */
static void gui_emit(const char *utf8, int role) {
    char *copy = strdup(utf8);
    if (copy) PostMessageW(g_hHistory, WM_APP_APPEND, (WPARAM)role, (LPARAM)copy);
}

/* 一轮 Agent 结束: 恢复 UI。 */
static void gui_on_done(void) {
    InterlockedExchange(&g_running, 0);
    InterlockedExchange(&g_cancel, 0);
    SetWindowTextW(g_hSend, L"发送");
    EnableWindow(g_hSend, TRUE);
    EnableWindow(g_hInput, TRUE);
    SetFocus(g_hInput);
}

/* 退出时从 Edit 同步配置到全局。 */
static void gui_read_config_ui(void) {
    if (g_hCfg[CFG_URL]) read_edit_utf8(g_hCfg[CFG_URL], g_api_url, sizeof(g_api_url));
    if (g_hCfg[CFG_KEY]) read_edit_utf8(g_hCfg[CFG_KEY], g_api_key, sizeof(g_api_key));
    if (g_hCfg[CFG_MDL]) read_edit_utf8(g_hCfg[CFG_MDL], g_model,   sizeof(g_model));
    if (g_hWorkspace)     read_edit_utf8(g_hWorkspace, g_workspace, sizeof(g_workspace));
}
