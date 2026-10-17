/*
 * ui/hooks.h - 宿主钩子实现: 输出投递 / 一轮结束 / 配置同步
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== 宿主钩子实现 ===== */

/* 输出: 复制到堆, 通过 PostMessage 投递到 UI 线程 (线程安全)。
 * wParam 携带角色 (CAGENT_ROLE_*), 供历史框按角色着色。 */
static void gui_emit(const char *utf8, int role) {
    char *copy = strdup(utf8);
    if (copy) PostMessageW(g_hHistory, WM_APP_APPEND, (WPARAM)role, (LPARAM)copy);
}

/* 状态栏: 报告"模型此刻在做什么"。core 在**工作线程**里调它, 所以只能投递到 UI 线程
 * (直接碰控件是不允许的); text 为 NULL 表示回到空闲。
 * 这些信息以前是往输出区打旁白的, 但它们属于状态而不是对话内容 —— 见 core/base.h 的说明。 */
static void gui_status(const char *text) {
    if (!g_hStatus) return;                        /* 控件还没建好 (理论上不会走到) */
    char *copy = text ? strdup(text) : NULL;
    PostMessageW(GetParent(g_hStatus), WM_APP_STATUS, 0, (LPARAM)copy);
}

/* "这一轮结束了" 的信号: 投递 WM_APP_FOLDEND (wndproc 收到后走 ui_turn_tail)。
 * 实时路径由 gui_on_done 在工作线程里投; **历史回放的终点也是轮次终点**,
 * 走同一条信号 (session_dlg.h 的 sess_apply_selected) —— 终态必须与实时一致。
 *
 * **必须走消息, 不能直接调 ui_turn_tail**: gui_emit 是 PostMessage 异步追加
 * (线程安全), 回放发出的一切此刻还排在队列里, 文档是空的、g_fold_n 为 0 ——
 * 直接调就是对着空气栈收尾 (d46e7f3 正是这么错的: 同步直调的测试桩测不出来,
 * 真机上最后一个工具块照旧展开)。同一条窗口队列 FIFO, 这条信号天然排在最后
 * 一条 WM_APP_APPEND 之后处理。 */
static void gui_turn_end(void) {
    PostMessageW(g_hHistory, WM_APP_FOLDEND, 0, 0);
}

/* 一轮 Agent 结束: 恢复 UI。 */
static void gui_on_done(void) {
    InterlockedExchange(&g_running, 0);
    InterlockedExchange(&g_cancel, 0);
    SetWindowTextW(g_hSend, L"发送");
    EnableWindow(g_hSend, TRUE);
    EnableWindow(g_hInput, TRUE);
    SetFocus(g_hInput);
    /* 本轮结束 = 最后一个折叠块也结束了, 按默认收起它 (见 gui_turn_end)。 */
    gui_turn_end();
    /* 状态栏回到"就绪" —— 同样只能投递 (传 0 表示空闲) */
    if (g_hStatus) PostMessageW(GetParent(g_hStatus), WM_APP_STATUS, 0, 0);
}

/* 流式分段: 发请求前开新分段, 重试前回删本分段已上屏的内容。
 * 走 PostMessage 而非 SendMessage —— 与 gui_emit 的追加同队列, 才能保证"先追加后回删"
 * 的顺序; 跨线程 SendMessage 会插到已投递的追加消息前面, 顺序就乱了。 */
static void gui_stream_begin(void) { PostMessageW(g_hHistory, WM_APP_STREAM, 1, 0); }
static void gui_stream_undo(void)  { PostMessageW(g_hHistory, WM_APP_STREAM, 0, 0); }

/* 退出时从 Edit 同步配置到全局。 */
static void gui_read_config_ui(void) {
    if (g_hCfg[CFG_URL]) read_edit_utf8(g_hCfg[CFG_URL], g_api_url, sizeof(g_api_url));
    if (g_hCfg[CFG_KEY]) read_edit_utf8(g_hCfg[CFG_KEY], g_api_key, sizeof(g_api_key));
    if (g_hCfg[CFG_MDL]) read_edit_utf8(g_hCfg[CFG_MDL], g_model,   sizeof(g_model));
    if (g_hWorkspace)     read_edit_utf8(g_hWorkspace, g_workspace, sizeof(g_workspace));
}
