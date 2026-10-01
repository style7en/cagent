/*
 * ui/task.h - 启动一轮 Agent: 配置校验 / 工作目录 / 线程派发
 *
 * cagent 界面层的一部分, 由 cagent_ui.c 按依赖顺序聚合 (单 TU, 全 static)。
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
    if (g_active_ws[0] && strcmp(g_workspace, g_active_ws) != 0) {
        /* 工作目录已变更: 当前对话落盘保留, 另起新会话 (不自动加载历史) */
        if (strlen(messages) > strlen(SYSTEM_PROMPT)) history_save();
        reset_conversation();
        g_history_file[0] = '\0';
        append_text("(工作目录已变更, 已另起新会话; 旧会话可通过 [加载会话] 恢复)\r\n");
    }
    snprintf(g_active_ws, sizeof(g_active_ws), "%s", g_workspace);

    if (!g_api_url[0] || !g_api_key[0] || !g_model[0]) {
        MessageBoxW(hwnd, L"请填写 Base Url、Key、Model 三项配置。",
                    L"配置不完整", MB_OK | MB_ICONWARNING);
        return;
    }

    wchar_t wws[MAX_PATH];
    if (utf8_to_wide(g_workspace, wws, MAX_PATH)) SetCurrentDirectoryW(wws);

    WCHAR *wbuf = (WCHAR*)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    GetWindowTextW(g_hInput, wbuf, wlen + 1);

    AgentTask *task = (AgentTask*)calloc(1, sizeof(AgentTask));
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, task->user_msg, BUFSZ, NULL, NULL);
    free(wbuf);

    SetWindowTextW(g_hInput, L"");

    /* 回显用户输入到历史框 (USER 角色: 蓝色加粗) */
    {
        size_t ul = strlen(task->user_msg);
        char *uecho = (char*)malloc(ul + 16);
        if (uecho) {
            snprintf(uecho, ul + 16, "\r\n你：%s\r\n", task->user_msg);
            gui_emit(uecho, CAGENT_ROLE_USER);
            free(uecho);
        }
    }

    InterlockedExchange(&g_running, 1);
    SetWindowTextW(g_hSend, L"停止");
    EnableWindow(g_hSend, TRUE);
    g_hThread = CreateThread(NULL, 0, agent_thread, task, 0, NULL);
}
