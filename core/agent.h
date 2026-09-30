/*
 * core/agent.h - Agent 循环: LLM ↔ 工具调用、取消与回滚
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== Agent 循环 ===== */

typedef struct { char user_msg[BUFSZ]; } AgentTask;

#define SYSTEM_PROMPT \
    "{\"role\":\"system\",\"content\":\"你是 cagent,一个极简的编程 Agent。" \
    "你有三个工具: execute_bash(执行命令)、read_file(读文件)、write_file(写文件)。" \
    "写入或覆盖文件前,先 read_file 读取现有内容。" \
    "修改已有文件时,先读取再用 write_file 写入完整新内容(本 Agent 没有增量编辑工具)。" \
    "列目录用 execute_bash 跑 dir,递归搜索内容用 findstr /s /i 关键词 *.* 。" \
    "命令通过 cmd /c 执行,用 Windows 命令风格:不要 mkdir -p(直接 mkdir),运行当前程序不要 ./ 前缀。" \
    "文件工具仅限工作目录内,用相对路径。" \
    "高危命令必须先征得用户同意:调用 execute_bash 前,先用一句话说明要做什么和风险,然后停下来等用户明确确认;确认之前不要执行。" \
    "高危包括:删除或覆盖文件(del、rd、rmdir /s、format、diskpart)、改注册表或系统服务(reg、sc、net user、schtasks)、关机重启、批量移动或重命名文件、下载后直接执行外部脚本、改写 git 历史(git push -f、reset --hard)、以及任何写入工作目录之外位置的操作。" \
    "用户同意后执行一次即可,同类操作不必反复询问;用户拒绝则放弃该做法并换一个更安全的方案。" \
    "任务不明确时,先向用户澄清。" \
    "任务完成后,停止并简要总结你做了什么。" \
    "始终用中文回答。回答简洁。\"}"

/* messages 缓冲水位线: 接近上限时整轮对话重置, 防止越界. */
#define MESSAGES_WATERMARK  ((BUFSZ * 3) / 4)

static const char *ROLLBACK_HINT = "\r\n(本轮已回滚, 不影响后续对话)\r\n";

/* 初始化 messages 为只含 system prompt 的状态。 */
static void reset_conversation(void) {
    snprintf(messages, BUFSZ, "%s", SYSTEM_PROMPT);
}

/* 前向声明: agent_turn 调用历史函数, 其定义在 config 区之后 */
static void history_save(void);

/* 执行一轮对话 (user_msg -> 直至最终回复或回滚)。线程无关, 可在主线程直接调用。
 * 用户消息的界面回显由前端负责 (核心不管渲染)。 */
static void agent_turn(const char *user_msg) {
    InterlockedExchange(&g_cancel, 0);          /* 清除取消标志 */

    /* 1. 若历史接近溢出, 重开一个新会话 (旧会话文件保留, 不删除) */
    if (strlen(messages) > MESSAGES_WATERMARK) {
        append_text("(对话历史过长, 已自动另起新会话)\r\n");
        reset_conversation();
        g_history_file[0] = '\0';
    }

    /* 2. 若首次发言, messages 还是空(没经过 config_load 之外的初始化) */
    if (messages[0] == '\0') reset_conversation();

    /* 3. 记录快照点: 出错时回滚到这里, 丢弃本轮 user message */
    size_t savepoint = strlen(messages);
    int rolled_back = 0;   /* 在 done 之前标记是否需要回滚 */

    /* 4. 追加本轮 user message */
    char *escaped = json_escape_alloc(user_msg);
    if (!escaped) {
        append_text("(内存不足, 已忽略本轮)\r\n");
        goto done;
    }
    int n = snprintf(messages + savepoint, BUFSZ - savepoint,
                     ",{\"role\":\"user\",\"content\":\"%s\"}", escaped);
    free(escaped);
    if (n <= 0 || (size_t)n >= BUFSZ - savepoint) {
        append_text("(输入过长, 已忽略本轮)\r\n");
        messages[savepoint] = '\0';
        goto done;
    }

    for (;;) {
        /* 取消检查点 1: 每轮迭代顶部 (LLM 调用前) */
        if (InterlockedCompareExchange(&g_cancel, 0, 0)) {
            append_text("(已取消)\r\n");
            rolled_back = 1;
            goto done;
        }
        snprintf(body, BUFSZ,
            "{\"model\":\"%s\",\"messages\":[%s],\"tools\":%s,\"stream\":true}",
            g_model, messages, TOOLS_JSON);

        append_text("(thinking...)\r\n");

        /* 流式 SSE 请求 */
        StreamCtx ctx;
        memset(&ctx, 0, sizeof(ctx));

        char full_url[1280];
        size_t nurl = strlen(g_api_url);
        int has_slash = (nurl > 0 && g_api_url[nurl-1] == '/');
        snprintf(full_url, sizeof(full_url), "%s%schat/completions",
                 g_api_url, has_slash ? "" : "/");

        int status = http_post_stream(full_url, g_api_key, body, strlen(body),
                                      resp, BUFSZ, &ctx);

        /* 取消 */
        if (status == -2) {
            append_text("(已取消)\r\n");
            rolled_back = 1;
            goto done;
        }
        /* 失败 (网络错误或非 200) */
        if (status != 200) {
            append_text(resp);
            append_text(ROLLBACK_HINT);
            rolled_back = 1;
            goto done;
        }

        /* 结束原因: 除正常 stop/tool_calls 外, 其余情况明确提示用户 */
        if (strcmp(ctx.finish, "length") == 0)
            append_text("(达到长度上限被截断; 可让模型继续补全)\r\n");
        else if (strcmp(ctx.finish, "content_filter") == 0)
            append_text("(内容被服务端的过滤策略拦截)\r\n");

        if (ctx.n_calls > 0) {
            /* 有 tool_calls: 从 ctx.calls 转 ToolCall 并执行 */
            typedef struct {
                char id[256]; char name[64]; char args[ARGS_MAX]; char *output;
            } ToolCall;
            ToolCall calls[8];
            int n_calls = 0;
            for (int i = 0; i < ctx.n_calls && n_calls < 8; i++) {
                if (!ctx.calls[i].name[0]) continue;
                ToolCall *c = &calls[n_calls++];
                c->id[0] = c->name[0] = c->args[0] = '\0';
                c->output = NULL;
                snprintf(c->id, sizeof(c->id), "%s", ctx.calls[i].id);
                snprintf(c->name, sizeof(c->name), "%s", ctx.calls[i].name);
                snprintf(c->args, sizeof(c->args), "%s", ctx.calls[i].args);
            }
            if (n_calls == 0) {
                append_text("(tool_calls 解析失败)\r\n");
                append_text(ROLLBACK_HINT);
                rolled_back = 1;
                goto done;
            }

        /* 执行所有工具: arguments 是字符串化 JSON, 二次解析取 command */
        for (int i = 0; i < n_calls; i++) {
            /* 取消检查点 2: 每个工具执行前 */
            if (InterlockedCompareExchange(&g_cancel, 0, 0)) {
                append_text("(已取消)\r\n");
                rolled_back = 1;
                for (int j = 0; j < n_calls; j++) free(calls[j].output);
                goto done;
            }
            ToolCall *c = &calls[i];
            {
                char line[8192];
                snprintf(line, sizeof(line), "[Tool] %s(%s)\r\n", c->name, c->args);
                append_text(line);
            }
            /* 分发 (含参数校验): 错误原因会写进 tool_out 交回模型 */
            dispatch_tool(c->name, c->args, ctx.finish);
            c->output = strdup(tool_out);
            {
                size_t tl = strlen(tool_out);
                char line[BUFSZ + 32];
                if (tl > 0 && (tool_out[tl-1] == '\n' || tool_out[tl-1] == '\r'))
                    snprintf(line, sizeof(line), "[Output]\r\n%s", tool_out);
                else
                    snprintf(line, sizeof(line), "[Output]\r\n%s\r\n", tool_out);
                append_text(line);
            }
        }

        /* 第二遍: 写 assistant 消息 (含所有 tool_calls 数组)。
         * content 保留模型本轮的自然语言说明: 否则下一轮它看不到自己说过什么。
         * 说明文字过大放不下时退化为空串 (不影响工具调用本身)。 */
        size_t mstart = strlen(messages);
        char *esc_narr = (ctx.content_len > 0) ? json_escape_alloc(ctx.content_buf) : NULL;
        int an = snprintf(messages + mstart, BUFSZ - mstart,
            ",{\"role\":\"assistant\",\"content\":\"%s\",\"tool_calls\":[",
            esc_narr ? esc_narr : "");
        if (esc_narr) free(esc_narr);
        int ok = (an > 0 && (size_t)an < BUFSZ - mstart);
        if (!ok) {
            /* 叙述过长: 退化为不带 content 的写法, 保证工具调用能写入 */
            an = snprintf(messages + mstart, BUFSZ - mstart,
                ",{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[");
            ok = (an > 0 && (size_t)an < BUFSZ - mstart);
        }

        for (int i = 0; i < n_calls && ok; i++) {
            ToolCall *c = &calls[i];
            char *esc_id   = json_escape_alloc(c->id);
            char *esc_name = json_escape_alloc(c->name);
            char *esc_args = json_escape_alloc(c->args);
            if (!esc_id || !esc_name || !esc_args) { ok = 0; free(esc_id); free(esc_name); free(esc_args); break; }
            size_t len = strlen(messages);
            int n2 = snprintf(messages + len, BUFSZ - len,
                "%s{\"id\":\"%s\",\"type\":\"function\","
                "\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}",
                (i == 0 ? "" : ","), esc_id, esc_name, esc_args);
            free(esc_id); free(esc_name); free(esc_args);
            if (n2 <= 0 || (size_t)n2 >= BUFSZ - len) { ok = 0; }
        }

        if (ok) {
            size_t len = strlen(messages);
            int n2 = snprintf(messages + len, BUFSZ - len, "]}");
            if (n2 <= 0 || (size_t)n2 >= BUFSZ - len) ok = 0;
        }

        /* 第三遍: 为每个 tool_call 写 tool 消息 */
        for (int i = 0; i < n_calls && ok; i++) {
            ToolCall *c = &calls[i];
            char *esc_id  = json_escape_alloc(c->id);
            char *esc_out = json_escape_alloc(c->output ? c->output : "");
            if (!esc_id || !esc_out) { ok = 0; free(esc_id); free(esc_out); break; }
            size_t len = strlen(messages);
            int n2 = snprintf(messages + len, BUFSZ - len,
                ",{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"%s\"}",
                esc_id, esc_out);
            free(esc_id); free(esc_out);
            if (n2 <= 0 || (size_t)n2 >= BUFSZ - len) ok = 0;
        }

        /* 释放工具输出 */
        for (int i = 0; i < n_calls; i++) free(calls[i].output);

        if (!ok) {
            /* 任一步失败: 把本次拼接的 assistant+tool 段全部截掉, 回滚整轮 */
            messages[mstart] = '\0';
            append_text("(历史空间或内存不足, 本轮回滚)\r\n");
            rolled_back = 1;
            goto done;
        }
        } else {
            /* 无 tool_calls: content 收尾。流式已逐字显示, 这里只追加换行 + messages。 */
            if (ctx.content_len > 0) {
                /* 内容末尾已带换行则不再补, 避免双倍空行 */
                if (ctx.content_buf[ctx.content_len - 1] != '\n' &&
                    ctx.content_buf[ctx.content_len - 1] != '\r')
                    append_text("\r\n");
                char *esc = json_escape_alloc(ctx.content_buf);
                if (esc) {
                    size_t len = strlen(messages);
                    int an = snprintf(messages + len, BUFSZ - len,
                        ",{\"role\":\"assistant\",\"content\":\"%s\"}", esc);
                    free(esc);
                    if (an <= 0 || (size_t)an >= BUFSZ - len) {
                        append_text("(历史空间不足, 本轮回滚)\r\n");
                        rolled_back = 1;
                    }
                }
            } else {
                if (strcmp(ctx.finish, "length") == 0)
                    append_text("(响应为空且已达长度上限, 本轮回滚)\r\n");
                else
                    append_text("(响应解析失败)\r\n");
                append_text(ROLLBACK_HINT);
                rolled_back = 1;
            }
            goto done;
        }
    }
done:
    if (rolled_back) {
        messages[savepoint] = '\0';
    }
    history_save();   /* 每轮 done 后保存 (含回滚后状态) */
}

/* 后台线程入口: 跑一轮后通过钩子通知前端。 */
static DWORD WINAPI agent_thread(LPVOID arg) {
    AgentTask *task = (AgentTask*)arg;
    agent_turn(task->user_msg);
    free(task);
    if (cagent_on_done) cagent_on_done();
    return 0;
}

