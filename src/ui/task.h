/*
 * ui/task.h - 启动一轮 Agent: 配置校验 / 工作目录 / 线程派发
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== 启动一轮 Agent (GUI) ===== */

static void start_task(HWND hwnd) {
    if (g_hThread) {
        WaitForSingleObject(g_hThread, INFINITE);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }

    int wlen = GetWindowTextLengthW(g_hInput);
    if (wlen <= 0) return;

    /* 同步配置 */
    read_edit_utf8(g_hCfg[CFG_URL], g_api_url, sizeof(g_api_url));
    read_edit_utf8(g_hCfg[CFG_KEY], g_api_key, sizeof(g_api_key));
    read_edit_utf8(g_hCfg[CFG_MDL], g_model,   sizeof(g_model));
    read_edit_utf8(g_hWorkspace, g_workspace, sizeof(g_workspace));
    /* 先归一化成绝对路径再校验: 用户填相对路径时, 下面的存在性检查与 SetCurrentDirectoryW
     * 都会随 CWD 漂移 (相对值按 exe 目录解析, 见 normalize_workspace)。变了就回写输入框,
     * 让用户看到实际生效的值。 */
    {
        char before[MAX_PATH];
        snprintf(before, sizeof(before), "%s", g_workspace);
        normalize_workspace();
        if (strcmp(before, g_workspace) != 0) set_edit_utf8(g_hWorkspace, g_workspace);
    }

    /* 工作目录必须存在且是目录: 否则 SetCurrentDirectoryW 静默失败, 命令会落在上一个
     * CWD 上, 而 path_in_workspace 又按 g_workspace 放行 —— 校验基准和执行位置分家。 */
    {
        wchar_t wchk[MAX_PATH];
        if (!utf8_to_wide(g_workspace, wchk, MAX_PATH)) wchk[0] = L'\0';
        DWORD attr = wchk[0] ? GetFileAttributesW(wchk) : INVALID_FILE_ATTRIBUTES;
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            MessageBoxW(hwnd, L"工作目录为空或不存在, 请重新选择。", L"工作目录无效",
                        MB_OK | MB_ICONWARNING);
            return;
        }
    }

    if (g_active_ws[0] && strcmp(g_workspace, g_active_ws) != 0) {
        /* 工作目录已变更: 当前对话落盘保留, 另起新会话 (不自动加载历史) */
        if (strlen(messages) > strlen(g_system_prompt)) history_save();
        reset_conversation();
        g_history_file[0] = '\0';
        emit_note("(工作目录已变更, 已另起新会话; 旧会话可通过 [加载会话] 恢复)\r\n");
    }
    snprintf(g_active_ws, sizeof(g_active_ws), "%s", g_workspace);

    if (!g_api_url[0] || !g_api_key[0] || !g_model[0]) {
        MessageBoxW(hwnd, L"请填写 Base Url、Key、Model 三项配置。",
                    L"配置不完整", MB_OK | MB_ICONWARNING);
        return;
    }

    /* 接入点/模型变了 = 半路换了后端: 分词器、缓存实现、上下文窗口都可能换了一套,
     * 上一模型返回的 prompt/cached 读数与累计命中率不再可比。**历史保留** (允许换模型
     * 接着聊), 只把用量统计清零, 状态栏立即刷新 —— 否则"累计命中率"会把两家服务端、
     * 两个 tokenizer 的数字混在一起。上下文分母仍按 ini 的 context_tokens:
     * 换到窗口不同的模型时请同步修改配置 (程序无法从模型名推断窗口)。 */
    if ((g_stat_url[0] && strcmp(g_api_url, g_stat_url) != 0) ||
        (g_stat_model[0] && strcmp(g_model, g_stat_model) != 0)) {
        usage_stats_reset();
        gui_status(NULL);
        log_line("[stats] 接入点/模型已变更, 用量统计清零 (对话历史保留)");
    }
    snprintf(g_stat_url, sizeof(g_stat_url), "%s", g_api_url);
    snprintf(g_stat_model, sizeof(g_stat_model), "%s", g_model);

    wchar_t wws[MAX_PATH];
    if (utf8_to_wide(g_workspace, wws, MAX_PATH) && !SetCurrentDirectoryW(wws)) {
        MessageBoxW(hwnd, L"无法切换到该工作目录, 请检查权限。", L"工作目录无效",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    WCHAR *wbuf = (WCHAR*)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    GetWindowTextW(g_hInput, wbuf, wlen + 1);

    AgentTask *task = (AgentTask*)calloc(1, sizeof(AgentTask));
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, task->user_msg, BUFSZ, NULL, NULL);
    free(wbuf);

    SetWindowTextW(g_hInput, L"");

    /* 回显用户输入到历史框 (USER 角色: 前端按"用户消息"样式渲染 —— 蓝底加粗)。
     * 措辞/前缀/换行的取舍见 hooks.h 的 ui_echo_user; 插话的消费路径共用它。 */
    ui_echo_user(task->user_msg);

    InterlockedExchange(&g_running, 1);
    ui_buttons_running();   /* 发送按钮可用 (此刻队为空, 标签即"发送"); 停止按钮解禁 */
    g_hThread = CreateThread(NULL, 0, agent_thread, task, 0, NULL);
    if (!g_hThread) {
        /* 线程创建失败: 必须把 UI 状态回滚, 否则 g_running 恒为 1、停止按钮常亮、
         * agent_thread 永不执行 -> gui_on_done 永不回调, 界面卡死在"运行中"。
         * task 由 agent_thread 释放, 这里失败只能自己释放, 否则泄漏。 */
        free(task);
        InterlockedExchange(&g_running, 0);
        ui_buttons_idle();
        SetFocus(g_hInput);
        emit_err("(启动后台线程失败, 请重试)\r\n");
    }
}
