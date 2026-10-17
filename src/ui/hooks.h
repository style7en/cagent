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

/* ===== 插话排队 (inbox) =====
 * 发送按钮在运行中收到文字就调 inbox_push (UI 线程); gui_on_done 在工作线程 take。
 * 两个线程会碰同一张表, 所以进出都走 SRWLOCK (见 state.h 的说明)。
 * 这些函数要用的 gui_emit / gui_turn_end 在上面, 顺序不能反。 */

/* 入队一条用户消息。返回 0 = 队列满或超长 (调用方应把文字留在输入框并提示)。 */
static int inbox_push(const char *msg) {
    if (!msg || !msg[0]) return 0;
    size_t len = strlen(msg);
    int ok = 0;
    AcquireSRWLockExclusive(&g_inbox_lock);
    if (g_inbox_n < INBOX_MAX && len < INBOX_SLOT) {
        memcpy(g_inbox[g_inbox_n++], msg, len + 1);
        ok = 1;
    }
    ReleaseSRWLockExclusive(&g_inbox_lock);
    return ok;
}

/* 取队首 (FIFO)。整槽搬移比逐条 strlen 简单, 且槽是定长数组, 读满槽也安全。
 * take 不到 (空或 cap 太小) 时返回 0, 消息留在队里 —— 绝不静默丢弃。 */
static int inbox_take(char *out, size_t cap) {
    int ok = 0;
    AcquireSRWLockExclusive(&g_inbox_lock);
    if (g_inbox_n > 0 && strlen(g_inbox[0]) < cap) {
        memcpy(out, g_inbox[0], strlen(g_inbox[0]) + 1);
        ok = 1;
        for (int i = 1; i < g_inbox_n; i++)
            memcpy(g_inbox[i - 1], g_inbox[i], INBOX_SLOT);
        g_inbox_n--;
    }
    ReleaseSRWLockExclusive(&g_inbox_lock);
    return ok;
}

/* 用户消息回显 (USER 角色: 蓝底加粗, 见 ui/state.h 的样式表)。
 * 两处共用: task.h 的首发、gui_on_done 的插话消费。不加"你："前缀 (样式已说明是谁),
 * 不带前导换行 (上一块若没收尾, do_append 的 g_last_no_lf 会补; 带了每条用户消息前
 * 会多出一个吃 USER 段前距的空段)。 */
static void ui_echo_user(const char *utf8) {
    if (!utf8 || !utf8[0]) return;
    size_t ul = strlen(utf8);
    char *uecho = (char*)malloc(ul + 16);
    if (uecho) {
        snprintf(uecho, ul + 16, "%s\r\n", utf8);
        gui_emit(uecho, CAGENT_ROLE_USER);
        free(uecho);
    }
}

/* ===== 双按钮状态 (发送 / 停止 彻底分开) =====
 * 「发送」永远可用: 空闲=发新轮, 运行中=排队插话; 排队中在标签上计数 ("发送 (2)")。
 * 「停止」空闲时禁用 (点了也没意义), 运行中可用, 点击后进"停止中..."直至本轮收尾。 */
static void ui_buttons_idle(void) {
    SetWindowTextW(g_hSend, L"发送");
    EnableWindow(g_hSend, TRUE);
    SetWindowTextW(g_hStop, L"停止");
    EnableWindow(g_hStop, FALSE);
}

static void ui_buttons_running(void) {
    /* g_inbox_n 是**无锁快照**: 调用点都在 push/take 之后紧接着, 极端交错下计数可能
     * 短暂旧一格, 但下一次 push 或收尾会刷过来 —— 队列的进出本身都在锁内, 不会不一致。 */
    wchar_t lbl[40];
    if (g_inbox_n > 0) _snwprintf(lbl, 40, L"发送 (%d)", g_inbox_n);
    else               _snwprintf(lbl, 40, L"发送");
    SetWindowTextW(g_hSend, lbl);
    EnableWindow(g_hSend, TRUE);
    SetWindowTextW(g_hStop, L"停止");
    EnableWindow(g_hStop, TRUE);
}

static void ui_buttons_stopping(void) {
    SetWindowTextW(g_hStop, L"停止中...");
    EnableWindow(g_hStop, FALSE);
}

/* 取队首并完成两步行投递: 先 gui_turn_end (把上一轮答案的最后一个块折起来),
 * 再回显这条插话。两条都走 PostMessage, 同一条窗口队列 FIFO 保证顺序 ——
 * 用户看到的是 [上一轮答案][折起的块][自己的插话][下一轮输出]。
 * 返回 1 = 取到 (消息已复制进 out, 调用方负责建线程接着跑); 0 = 队列空。 */
static int ui_drain_inbox(char *out, size_t cap) {
    if (!inbox_take(out, cap)) return 0;
    gui_turn_end();
    ui_echo_user(out);
    return 1;
}

/* 一轮 Agent 结束: 恢复 UI。
 *
 * **排队插话的消费点** (core 每轮 done 都会调这里): 队列非空时这一轮不停机 ——
 * 折上轮尾块、回显插话、原地再开一个后台线程跑下一轮, g_running 保持 1。
 * 好处是没有"收工又重启"的中间态: 按钮不闪烁、不会露出 g_running=0 的空窗
 * (那个空窗里用户的一次发送会滑进 start_task 的旧路径, 与排队语义打架)。
 *
 * 注意 drain 里 **CreateThread 会覆盖 g_hThread**: 旧句柄必须先 CloseHandle
 * (WM_DESTROY 的 join 与 task.h 的等待都指着它, 覆盖=泄漏)。 */
static void gui_on_done(void) {
    char next[INBOX_SLOT];
    if (ui_drain_inbox(next, sizeof(next))) {
        AgentTask *task = (AgentTask*)calloc(1, sizeof(AgentTask));
        if (task) {
            snprintf(task->user_msg, sizeof(task->user_msg), "%s", next);  /* INBOX_SLOT < BUFSZ */
            HANDLE old = g_hThread;
            g_hThread = CreateThread(NULL, 0, agent_thread, task, 0, NULL);
            if (g_hThread) {
                if (old) CloseHandle(old);
                ui_buttons_running();     /* 停止恢复可用 (可能是从"停止中..."进来的); 发送按新队计数 */
                return;                   /* g_running 保持 1, 不收尾 */
            }
            free(task);
            emit_err("(排队消息启动后台线程失败, 该消息未发出)\r\n");
            /* 建不出线程也只能收尾; 消息已回显但没进 messages, 下一轮不会带着它 */
        }
    }
    InterlockedExchange(&g_running, 0);
    InterlockedExchange(&g_cancel, 0);
    ui_buttons_idle();
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
