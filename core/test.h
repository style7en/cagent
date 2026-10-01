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

    /* ---- HTTP 重试判定 (缺口 ③) ---- */
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
        CHK(strstr(g_touched_files, "e.txt") != NULL);   /* 改过的文件要记账 (A6) */

        /* 唯一匹配 -> 成功替换 */
        tool_edit_file("e.txt", "beta", "BETA");
        CHK(strstr(tool_out, "已完成定点替换") != NULL);

        /* 读回确认替换生效且其余内容完好 */
        tool_read_file("e.txt");
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

    /* ---- 命令执行: 退出码 (A2) ---- */
    {
        /* 无输出的命令也要能看到退出码, 否则模型只能靠猜命令是否成功 */
        dispatch_tool("execute_bash", "{\"command\":\"exit 3\"}", "");
        CHK(strstr(tool_out, "[exit=3]") != NULL);
        dispatch_tool("execute_bash", "{\"command\":\"echo hi\"}", "");
        CHK(strstr(tool_out, "hi") != NULL);
        CHK(strstr(tool_out, "[exit=0]") != NULL);
    }

    /* ---- SSE: 超上限的 tool_calls 要记下 id/name (A3) ---- */
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

    /* ---- SSE: content 装不下要置截断标志并留出标记空间 (A4) ---- */
    {
        StreamCtx *c = (StreamCtx*)calloc(1, sizeof(StreamCtx));
        CHK(c != NULL);
        if (c) {
            char delta[4096];
            memset(delta, 'x', sizeof(delta) - 1);
            delta[sizeof(delta) - 1] = '\0';
            for (int i = 0; i < 100 && !c->truncated; i++)
                on_content_delta(c, delta);
            CHK(c->truncated == 1);                                    /* 不再静默丢尾 */
            CHK(c->content_len + CONTENT_TAIL_RESERVE <= sizeof(c->content_buf));
            CHK(c->content_buf[c->content_len] == '\0');
            free(c);
        }
    }

    /* ---- JSON: 递归深度上限, 不可让不可信输入栈溢出 (A5) ---- */
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

    #undef CHK
    if (fails == 0) printf("run_all_tests: OK\n");
    else printf("run_all_tests: %d FAIL(s)\n", fails);
    return fails ? 1 : 0;
}
