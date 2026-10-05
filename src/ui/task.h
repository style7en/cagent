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
        append_text("(工作目录已变更, 已另起新会话; 旧会话可通过 [加载会话] 恢复)\r\n");
    }
    snprintf(g_active_ws, sizeof(g_active_ws), "%s", g_workspace);

    if (!g_api_url[0] || !g_api_key[0] || !g_model[0]) {
        MessageBoxW(hwnd, L"请填写 Base Url、Key、Model 三项配置。",
                    L"配置不完整", MB_OK | MB_ICONWARNING);
        return;
    }

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
    if (!g_hThread) {
        /* 线程创建失败: 必须把 UI 状态回滚, 否则 g_running 恒为 1、按钮停在"停止",
         * agent_thread 永不执行 -> gui_on_done 永不回调, 界面卡死在"运行中"。
         * task 由 agent_thread 释放, 这里失败只能自己释放, 否则泄漏。 */
        free(task);
        InterlockedExchange(&g_running, 0);
        SetWindowTextW(g_hSend, L"发送");
        EnableWindow(g_hSend, TRUE);
        SetFocus(g_hInput);
        append_text("(启动后台线程失败, 请重试)\r\n");
    }
}
