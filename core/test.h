/*
 * core/test.h - 自动化回归测试 (纯静态, 随 cagent_core.h 一起编译)
 *
 * run_all_tests() 覆盖: JSON 解析/转义、编码检测与字符边界、工具分发与参数校验、
 * edit_file 定点编辑的各类边界、HTTP 重试判定。无网络/GUI 依赖, 可离线运行。
 *
 * 复用方式:
 *   - GUI 二进制: cagent.exe --selftest 调用它, 结果写入 selftest.txt
 *   - 命令行: make test 编译 cagent_test.exe 并运行, 退出码即失败数
 *
 * 设计取向: 不依赖 LLM/网络, 直接调用各 static 函数并断言行为, 作为"改动即回归"的护栏。
 */

static int run_all_tests(void) {
    int fails = 0;
    #define CHK(cond) do { if(!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); fails++; } } while(0)

    /* ---- JSON 解析 / 转义 / DPAPI (复用既有 json_selftest) ---- */
    if (json_selftest() != 0) fails++;

    /* ---- 编码检测与字符边界 ---- */
    CHK(is_valid_utf8((const unsigned char*)"hello", 5) == 1);
    CHK(is_valid_utf8((const unsigned char*)"\xe4\xbd\xa0\xe5\xa5\xbd", 6) == 1); /* 你好 */
    CHK(is_valid_utf8((const unsigned char*)"\xc4\xe3", 2) == 0);                  /* GBK 字节 */
    {
        /* 末尾截断的多字节字符应停在完整字符边界前 */
        const char *s = "\xe4\xbd\xa0\xe5";   /* 你 + 半个字节 */
        CHK(utf8_trim_len(s, 4) == 3);
    }
    CHK(utf8_trim_len("\xc4\xe3", 2) == 2);   /* 非 UTF-8 原样返回, 绝不能判死 */

    /* ---- HTTP 重试判定 ---- */
    CHK(http_should_retry(-1) == 1);     /* 网络层错误 */
    CHK(http_should_retry(-2) == 0);     /* 取消 */
    CHK(http_should_retry(200) == 0);    /* 成功 */
    CHK(http_should_retry(400) == 0);    /* 客户端错误: 不重试 */
    CHK(http_should_retry(403) == 0);
    CHK(http_should_retry(408) == 1);    /* 请求超时 */
    CHK(http_should_retry(429) == 1);    /* 限流 */
    CHK(http_should_retry(500) == 1);    /* 服务端错误 */
    CHK(http_should_retry(503) == 1);

    /* ---- 工具: 分发与参数校验 ---- */
    {
        dispatch_tool("read_file", "{}", "");
        CHK(strstr(tool_out, "参数缺失") != NULL);

        dispatch_tool("read_file", "{not json", "");
        CHK(strstr(tool_out, "参数解析失败") != NULL);

        dispatch_tool("bogus", "{}", "");
        CHK(strstr(tool_out, "未知工具") != NULL);

        dispatch_tool("", "{}", "");
        CHK(strstr(tool_out, "未知工具") != NULL);
    }

    /* ---- 工具: edit_file 定点编辑边界 ---- */
    {
        /* 准备隔离的测试工作目录 (相对 CWD, GetFullPathName 会规范化) */
        CreateDirectoryA("cagent_test_ws", NULL);
        strcpy(g_workspace, "cagent_test_ws");
        g_touched_files[0] = '\0';

        /* 写一个测试文件 (ASCII, 合法 UTF-8) */
        tool_write_file("e.txt", "alpha\nbeta\ngamma\n");
        CHK(strstr(tool_out, "已写入") != NULL);
        CHK(strstr(g_touched_files, "e.txt") != NULL);   /* 改过的文件要记账 */

        /* 唯一匹配 -> 成功替换 */
        tool_edit_file("e.txt", "beta", "BETA");
        CHK(strstr(tool_out, "已完成定点替换") != NULL);

        /* 读回确认替换生效且其余内容完好 */
        tool_read_file("e.txt", 0);
        CHK(strstr(tool_out, "BETA") != NULL);
        CHK(strstr(tool_out, "alpha") != NULL);
        CHK(strstr(tool_out, "gamma") != NULL);

        /* 多处匹配 -> 拒绝并说明, 文件未改 */
        tool_edit_file("e.txt", "a", "X");
        CHK(strstr(tool_out, "匹配到") != NULL && strstr(tool_out, "文件未修改") != NULL);

        /* 零匹配 -> 提示先读原文 */
        tool_edit_file("e.txt", "nonexistent", "X");
        CHK(strstr(tool_out, "未找到匹配") != NULL);

        /* old_text 为空 -> 拒绝 */
        tool_edit_file("e.txt", "", "X");
        CHK(strstr(tool_out, "old_text 不能为空") != NULL);

        /* 工作目录外 -> 拒绝 */
        tool_edit_file("C:\\windows\\system32\\x.txt", "beta", "X");
        CHK(strstr(tool_out, "路径在工作目录外") != NULL);

        /* GBK 文件 -> 拒绝 (增量编辑无法定位非 UTF-8)。
         * 用 tool_write_file 写原始 GBK 字节, 确保文件落在工作目录内。 */
        tool_write_file("gbk.txt", "\xC4\xE3\n");   /* 原始 GBK 字节 (你 + 换行) */
        tool_edit_file("gbk.txt", "你", "您");
        CHK(strstr(tool_out, "不是 UTF-8") != NULL);
        remove("cagent_test_ws/gbk.txt");

        /* 清理测试目录 */
        remove("cagent_test_ws/e.txt");
        RemoveDirectoryA("cagent_test_ws");
        g_workspace[0] = '\0';
    }

    /* ---- 命令执行: 退出码 ---- */
    {
        /* 无输出的命令也要能看到退出码, 否则模型只能靠猜命令是否成功 */
        dispatch_tool("execute_bash", "{\"command\":\"exit 3\"}", "");
        CHK(strstr(tool_out, "[exit=3]") != NULL);
        dispatch_tool("execute_bash", "{\"command\":\"echo hi\"}", "");
        CHK(strstr(tool_out, "hi") != NULL);
        CHK(strstr(tool_out, "[exit=0]") != NULL);
    }

    /* ---- SSE: 超上限的 tool_calls 要记下 id/name ---- */
    {
        StreamCtx *c = (StreamCtx*)calloc(1, sizeof(StreamCtx));
        CHK(c != NULL);
        if (c) {
            JValue *tcs = json_parse(
                "[{\"index\":0,\"id\":\"call_0\",\"function\":{\"name\":\"execute_bash\","
                "\"arguments\":\"{\\\"command\\\":\\\"a\\\"}\"}},"
                "{\"index\":9,\"id\":\"call_9\",\"function\":{\"name\":\"write_file\","
                "\"arguments\":\"{\\\"path\\\":\\\"x\\\"}\"}}]");
            CHK(tcs != NULL);
            stream_apply_tool_calls(c, tcs);
            CHK(c->n_calls == 1);                     /* 正常的 0 号调用 */
            CHK(strcmp(c->calls[0].name, "execute_bash") == 0);
            CHK(c->n_dropped == 1);                   /* 9 号被丢弃, 但 id/name 必须留下 */
            CHK(strcmp(c->dropped[0].id, "call_9") == 0);
            CHK(strcmp(c->dropped[0].name, "write_file") == 0);
            json_free(tcs);
            free(c);
        }
    }

    /* ---- SSE: content 装不下要置截断标志并留出标记空间 ---- */
    {
        StreamCtx *c = (StreamCtx*)calloc(1, sizeof(StreamCtx));
        CHK(c != NULL);
        if (c) {
            char delta[4096];
            memset(delta, 'x', sizeof(delta) - 1);
            delta[sizeof(delta) - 1] = '\0';
            for (int i = 0; i < 400 && !c->truncated; i++)   /* 400*4KB > 1MB 必然触发 */
                on_content_delta(c, delta);
            CHK(c->truncated == 1);                                    /* 不再静默丢尾 */
            CHK(c->content_len + CONTENT_TAIL_RESERVE <= sizeof(c->content_buf));
            CHK(c->content_buf[c->content_len] == '\0');
            free(c);
        }
    }

    /* ---- JSON: 递归深度上限, 不可让不可信输入栈溢出 ---- */
    {
        char deep[512];
        int k = 0;
        for (int i = 0; i < 200; i++) deep[k++] = '[';
        for (int i = 0; i < 200; i++) deep[k++] = ']';
        deep[k] = '\0';
        CHK(json_parse(deep) == NULL);              /* 深度 200 > 64: 拒绝而非崩溃 */
        /* 深度计数在兄弟节点间不能累加: 100 个并列的内层数组最多只有 2 层 */
        k = 0; deep[k++] = '[';
        for (int i = 0; i < 100 && k < (int)sizeof(deep) - 8; i++) {
            if (i) deep[k++] = ',';
            deep[k++] = '['; deep[k++] = ']';
        }
        deep[k++] = ']'; deep[k] = '\0';
        {
            JValue *r = json_parse(deep);
            CHK(r != NULL);
            json_free(r);
        }
    }

    /* ---- 会话文件落点: 必须在 sessions\ 下, 不污染根目录 ---- */
    {
        char sdir[MAX_PATH];
        wchar_t wtmp[MAX_PATH];
        sessions_dir(sdir, sizeof(sdir));
        CHK(strlen(sdir) > 9 && _stricmp(sdir + strlen(sdir) - 9, "sessions\\") == 0);
        CHK(utf8_to_wide(sdir, wtmp, MAX_PATH));                    /* 目录已创建 */
        {
            DWORD wa = GetFileAttributesW(wtmp);
            CHK(wa != INVALID_FILE_ATTRIBUTES && (wa & FILE_ATTRIBUTE_DIRECTORY) != 0);
        }
        {
            char sp[MAX_PATH];
            build_new_session_path(sp, sizeof(sp));
            CHK(_strnicmp(sp, sdir, strlen(sdir)) == 0);            /* 新会话在该目录内 */
            CHK(strstr(sp, ".json") != NULL);
        }
    }

    /* ---- 沙箱: 路径穿越 / 越界必须被拒 ---- */
    {
        CreateDirectoryA("cagent_test_ws", NULL);
        strcpy(g_workspace, "cagent_test_ws");
        /* 相对路径穿越到工作目录外 */
        CHK(path_in_workspace("../escape.txt") == 0);
        CHK(path_in_workspace("..\\escape.txt") == 0);
        /* 绝对路径指向系统目录 */
        CHK(path_in_workspace("C:\\windows\\system32\\x.txt") == 0);
        /* 工作目录内的合法路径放行 */
        CHK(path_in_workspace("ok.txt") == 1);
        CHK(path_in_workspace("sub\\ok.txt") == 1);
        /* write_file / edit_file 越界被拒 */
        tool_write_file("C:\\windows\\system32\\x.txt", "pwn");
        CHK(strstr(tool_out, "路径在工作目录外") != NULL);
        tool_edit_file("C:\\windows\\system32\\x.txt", "a", "b");
        CHK(strstr(tool_out, "路径在工作目录外") != NULL);
        RemoveDirectoryA("cagent_test_ws");
        g_workspace[0] = '\0';
    }

    /* ---- 会话命名: 同秒冲突要能避开 ---- */
    {
        char sp1[MAX_PATH], sp2[MAX_PATH];
        build_new_session_path(sp1, sizeof(sp1));
        FILE *tf = fopen_utf8(sp1, "wb");     /* 模拟该路径已存在 */
        if (tf) { fputc('x', tf); fclose(tf); }
        build_new_session_path(sp2, sizeof(sp2));
        CHK(strcmp(sp1, sp2) != 0);           /* 冲突时应换名, 而非覆盖 */
        remove(sp1);
    }

    /* ---- 上下文压缩: 消息遍历辅助 ---- */
    {
        reset_conversation();
        strcat(messages, ",{\"role\":\"user\",\"content\":\"u1\"}");
        strcat(messages, ",{\"role\":\"assistant\",\"content\":\"\","
                         "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
                         "\"function\":{\"name\":\"execute_bash\",\"arguments\":\"{}\"}}]}");
        strcat(messages, ",{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"out\"}");
        strcat(messages, ",{\"role\":\"user\",\"content\":\"u2\"}");
        strcat(messages, ",{\"role\":\"assistant\",\"content\":\"done\"}");

        const char *conv = messages + strlen(g_system_prompt);
        CHK(msg_count(conv) == 5);
        size_t s, e;
        CHK(msg_bounds(conv, 0, &s, &e) == 1 && conv[s] == '{');
        CHK(msg_bounds(conv, 4, &s, &e) == 1 && strstr(conv + s, "done") != NULL);
        CHK(msg_bounds(conv, 5, &s, &e) == 0);            /* 越界: 找不到 */
        char role[16];
        msg_bounds(conv, 2, &s, &e);
        msg_role(conv + s, e - s, role, sizeof(role));
        CHK(strcmp(role, "tool") == 0);
        msg_bounds(conv, 0, &s, &e);
        msg_role(conv + s, e - s, role, sizeof(role));
        CHK(strcmp(role, "user") == 0);
    }

    /* ---- 上下文压缩: 超限识别 ---- */
    CHK(is_context_overflow_error("...This model's maximum context length is 8192 tokens...") == 1);
    CHK(is_context_overflow_error("{\"error\":{\"code\":\"context_length_exceeded\"}}") == 1);
    CHK(is_context_overflow_error("prompt is too long: 200 tokens > 100 maximum") == 1);
    CHK(is_context_overflow_error("[HTTP 400] Invalid API key") == 0);
    CHK(is_context_overflow_error("") == 0);
    CHK(is_context_overflow_error(NULL) == 0);

    /* ---- 上下文压缩: 水位判定 (token 优先, 字节兜底) ---- */
    {
        g_context_tokens = 0;
        g_last_prompt_tokens = 0;
        g_last_prompt_bytes = 0;
        reset_conversation();                             /* 只有系统提示词, 远小于水位 */
        CHK(context_pressure() == 0);                     /* 字节兜底: 未超 */

        g_context_tokens = 100;                           /* 窗口极小: est=strlen/3 必然超 */
        CHK(context_pressure() == 1);

        /* 有服务端读数: 上次 1000 token + 新增 900 字节 (~300 token) = 1300 */
        g_last_prompt_tokens = 1000;
        g_last_prompt_bytes = strlen(messages) - 900;
        g_context_tokens = 1500;                          /* 1500*4/5=1200 < 1300 -> 压 */
        CHK(context_pressure() == 1);
        g_context_tokens = 2000;                          /* 1600 >= 1300 -> 不压 */
        CHK(context_pressure() == 0);

        g_context_tokens = 0;
        g_last_prompt_tokens = 0;
        g_last_prompt_bytes = 0;
    }

    /* ---- 上下文压缩: 保底丢弃 (不经模型), 必须保持消息配对完整 ---- */
    {
        reset_conversation();
        strcat(messages, ",{\"role\":\"user\",\"content\":\"u1\"}");
        strcat(messages, ",{\"role\":\"assistant\",\"content\":\"\","
                         "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
                         "\"function\":{\"name\":\"execute_bash\",\"arguments\":\"{}\"}}]}");
        strcat(messages, ",{\"role\":\"tool\",\"tool_call_id\":\"c1\",\"content\":\"out\"}");
        strcat(messages, ",{\"role\":\"user\",\"content\":\"u2\"}");
        strcat(messages, ",{\"role\":\"assistant\",\"content\":\"done\"}");
        size_t before = strlen(messages);

        /* 目标极小 -> 全部要丢 -> 返回 0 且 messages 原样 (交调用方重置) */
        g_context_tokens = 8;
        CHK(compact_drop_oldest() == 0);
        CHK(strlen(messages) == before);

        /* 目标约一半 -> 丢最旧若干条, 但保留段起点绝不能是孤立 tool 结果 */
        const char *conv0 = messages + strlen(g_system_prompt);
        g_context_tokens = (long)(strlen(conv0) / 3);     /* target ≈ conv 长度的一半 */
        int ok3 = compact_drop_oldest();
        CHK(ok3 == 1);
        CHK(strlen(messages) < before);
        {
            const char *conv = messages + strlen(g_system_prompt);
            if (conv[0] == ',') conv++;
            char *arr = (char*)malloc(strlen(conv) + 3);
            CHK(arr != NULL);
            if (arr) {
                sprintf(arr, "[%s]", conv);
                JValue *root = json_parse(arr);
                CHK(root != NULL && root->type == J_ARR && root->arr.n >= 1);
                if (root && root->type == J_ARR && root->arr.n >= 1) {
                    const JValue *m0 = root->arr.items[0];
                    const JValue *r = json_obj_get(m0, "role");
                    CHK(r && r->type == J_STR && strcmp(r->str, "tool") != 0);  /* 无孤立 tool */
                    const JValue *ml = root->arr.items[root->arr.n - 1];
                    const JValue *c = json_obj_get(ml, "content");
                    CHK(c && c->type == J_STR && strcmp(c->str, "done") == 0);  /* 最新消息保留 */
                }
                json_free(root);
                free(arr);
            }
        }
        g_context_tokens = 0;
    }

    /* ---- 工具: read_file 的 offset 分块续读 ---- */
    {
        CreateDirectoryA("cagent_test_ws", NULL);
        strcpy(g_workspace, "cagent_test_ws");

        /* 20KB 文件: 默认 16KB 上限必须截断并给出续读 offset */
        {
            char *big = (char*)malloc(20004);
            if (big) {
                memset(big, 'a', 20000);
                big[20000] = '\0';
                strcat(big, "END");
                tool_write_file("big.txt", big);
                free(big);
            }
            size_t cap = tool_output_cap();
            tool_read_file("big.txt", 0);
            CHK(strstr(tool_out, "已截断") != NULL);
            {
                char hint[64];
                snprintf(hint, sizeof(hint), "offset=%zu", cap);
                CHK(strstr(tool_out, hint) != NULL);      /* 提示里给下一次的 offset */
            }
            /* 续读: 从上次的 offset 拿到剩余部分 (含 END, 无截断提示) */
            tool_read_file("big.txt", (long long)tool_output_cap());
            CHK(strstr(tool_out, "END") != NULL);
            CHK(strstr(tool_out, "已截断") == NULL);

            /* 超出文件末尾: 明确说明, 不报错 */
            tool_read_file("big.txt", 999999);
            CHK(strstr(tool_out, "已无内容") != NULL);

            remove("cagent_test_ws/big.txt");
        }

        /* offset 落在多字节字符中间: 前移到字符边界, 不出乱码
         * "aaaa"(4 字节) + "你你你"(9 字节); offset=5 切在第一个'你'中间,
         * 跳过 2 个续字节后从第二个'你'开头 -> 结果是"你你" */
        {
            tool_write_file("u8.txt", "aaaa\xe4\xbd\xa0\xe4\xbd\xa0\xe4\xbd\xa0");
            tool_read_file("u8.txt", 5);
            CHK(strcmp(tool_out, "你你") == 0);

            /* offset 也接受数字字符串: "2" -> 从第 3 字节起 = "aa你你你" */
            dispatch_tool("read_file", "{\"path\":\"u8.txt\",\"offset\":\"2\"}", "");
            CHK(strcmp(tool_out, "aa你你你") == 0);

            /* 非法字符串: 回退从头读 */
            dispatch_tool("read_file", "{\"path\":\"u8.txt\",\"offset\":\"abc\"}", "");
            CHK(strcmp(tool_out, "aaaa你你你") == 0);

            remove("cagent_test_ws/u8.txt");
        }

        RemoveDirectoryA("cagent_test_ws");
        g_workspace[0] = '\0';
    }

    /* ---- run_pipe / append_note 边界: 空/过小缓冲绝不越界写 ---- */
    {
        char sb[16];
        size_t pos = 0;

        /* out_cap=0: run_pipe 拒绝, append_note 直接忽略 */
        CHK(run_pipe("echo x", NULL, 0) == -1);
        strcpy(sb, "KEEP");
        pos = 4;
        append_note(sb, &pos, 0, "zz");
        CHK(pos == 4 && strcmp(sb, "KEEP") == 0);

        /* 放得下: 正常追加 */
        snprintf(sb, sizeof(sb), "0123456789abc");      /* 13 字节 */
        pos = 13;
        append_note(sb, &pos, sizeof(sb), "xy");
        CHK(pos == 15 && strcmp(sb, "0123456789abcxy") == 0);

        /* 放不下: 回退旧内容, 保 note 完整 */
        append_note(sb, &pos, sizeof(sb), "abcde");
        CHK(pos == 15 && strcmp(sb, "0123456789abcde") == 0);

        /* note 比整个缓冲还长: 截 note 而不是越界写 */
        append_note(sb, &pos, sizeof(sb), "LONG-NOTE-1234567890");   /* 20 字节 */
        CHK(pos == 15 && strcmp(sb, "LONG-NOTE-12345") == 0);
    }

    /* ---- 外置系统提示词: 转义包装 / 空原文 / 超限拒绝, 失败保持原指针 ---- */
    {
        const char *def = g_system_prompt;
        const char *cur;

        CHK(system_prompt_set_raw("hi") == 1);
        CHK(strcmp(g_system_prompt, "{\"role\":\"system\",\"content\":\"hi\"}") == 0);

        /* 含引号/换行的原文: 转义后仍是合法 JSON, content 还原一致 */
        CHK(system_prompt_set_raw("say \"hi\"\nnext") == 1);
        JValue *r = json_parse(g_system_prompt);
        CHK(r != NULL && r->type == J_OBJ);
        const JValue *c = r ? json_obj_get(r, "content") : NULL;
        CHK(c != NULL && c->type == J_STR && strcmp(c->str, "say \"hi\"\nnext") == 0);
        if (r) json_free(r);

        /* 空原文 / 超 32KB: 拒绝提交, 指针不动 */
        cur = g_system_prompt;
        CHK(system_prompt_set_raw("") == 0 && g_system_prompt == cur);
        char *big = (char*)malloc(SYSTEM_PROMPT_MAX_RAW + 2);
        memset(big, 'a', SYSTEM_PROMPT_MAX_RAW + 1);
        big[SYSTEM_PROMPT_MAX_RAW + 1] = '\0';
        CHK(system_prompt_set_raw(big) == 0 && g_system_prompt == cur);
        free(big);

        g_system_prompt = def;   /* 恢复默认, 不影响后续判定 */
        CHK(g_system_prompt == def);
    }

    #undef CHK
    if (fails == 0) printf("run_all_tests: OK\n");
    else printf("run_all_tests: %d FAIL(s)\n", fails);
    return fails ? 1 : 0;
}
