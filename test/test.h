/*
 * test/test.h - 自动化回归测试套件 (纯静态)
 *
 * run_all_tests() 覆盖: JSON 解析/转义、编码检测与字符边界、工具分发与参数校验、
 * edit_file 定点编辑的各类边界、HTTP 重试判定。无网络/GUI 依赖, 可离线运行。
 *
 * 位置: 独立于 src/ —— 核心与产品二进制都不该背测试代码。
 * 前置条件: **必须先 include src/cagent_core.h** (本文件直接调用那里的 static 函数)。
 *
 * 唯一的运行方式是命令行: make test 编译 test/main.c, 退出码即失败数。
 * (曾经还有个 `cagent.exe --selftest` 入口把结果写进 selftest.txt, 已移除 ——
 *  GUI 子系统没有控制台才需要文件中转, 而它逼着产品二进制带上整套测试代码, 不划算。)
 *
 * 设计取向: 不依赖 LLM/网络, 直接调用各 static 函数并断言行为, 作为"改动即回归"的护栏。
 */

/* ---- 测试用的隔离工作目录 ----
 * 必须是**绝对路径**: g_workspace 的相对值现在按 exe 目录解析 (见 workspace.h 的
 * normalize_workspace), 若沿用"塞个相对路径、靠 CWD 规范化"的旧写法, 测试结果就会随
 * "从哪个目录启动"变化 —— 从仓库根跑能过, 换个目录全挂。 */
static char g_test_ws[MAX_PATH];

static void test_ws_begin(void) {
    char exedir[MAX_PATH];
    get_exe_dir_utf8(exedir, sizeof(exedir));
    snprintf(g_test_ws, sizeof(g_test_ws), "%s\\cagent_test_ws", exedir);
    wchar_t wd[MAX_PATH];
    if (utf8_to_wide(g_test_ws, wd, MAX_PATH)) CreateDirectoryW(wd, NULL);
    snprintf(g_workspace, sizeof(g_workspace), "%s", g_test_ws);
    g_touched_files[0] = '\0';
}

/* 删掉测试工作目录里的一个文件 (走绝对路径, 不受 CWD 影响) */
static void test_ws_rm(const char *name) {
    char p[MAX_PATH];
    wchar_t w[MAX_PATH];
    snprintf(p, sizeof(p), "%s\\%s", g_test_ws, name);
    if (utf8_to_wide(p, w, MAX_PATH)) DeleteFileW(w);
}

static void test_ws_end(void) {
    wchar_t w[MAX_PATH];
    if (utf8_to_wide(g_test_ws, w, MAX_PATH)) RemoveDirectoryW(w);
    g_workspace[0] = '\0';
}

/* ---- JSON 解析 / 转义 / DPAPI 自检 ----
 * 原在 core/json.h 里与真实实现挤在同一个产品头文件里。
 * 既然测试已独立到 test/, 一并搬过来 —— 产品二进制不该带测试代码。 */

static int json_selftest(void) {
    int fails = 0;
    #define CHK(cond) do { if(!(cond)) { printf("FAIL: %s\n", #cond); fails++; } } while(0)

    /* 基本对象 + 各类型 */
    {
        JValue *r = json_parse("{\"name\":\"abc\",\"n\":3,\"b\":true,\"x\":null}");
        CHK(r != NULL && r->type == J_OBJ);
        CHK(json_as_str(json_obj_get(r,"name")) && strcmp(json_as_str(json_obj_get(r,"name")),"abc")==0);
        CHK(json_obj_get(r,"n") && json_obj_get(r,"n")->num == 3.0);
        CHK(json_obj_get(r,"b") && json_obj_get(r,"b")->b == 1);
        CHK(json_obj_get(r,"x") && json_obj_get(r,"x")->type == J_NULL);
        json_free(r);
    }
    /* 数组 + 嵌套对象 */
    {
        JValue *r = json_parse("{\"arr\":[1,2,{\"k\":\"v\"}]}");
        CHK(r != NULL);
        const JValue *a = json_obj_get(r,"arr");
        CHK(a && a->type == J_ARR && a->arr.n == 3);
        CHK(json_arr_at(a,1) && json_arr_at(a,1)->num == 2.0);
        CHK(json_as_str(json_obj_get(json_arr_at(a,2),"k")) && strcmp(json_as_str(json_obj_get(json_arr_at(a,2),"k")),"v")==0);
        json_free(r);
    }
    /* 转义 + 中文 + 代理对 */
    {
        JValue *r = json_parse("\"a\\nb\\tc\\\\d\\\"e\\/\\u4e2d\\uD83D\\uDE00\"");
        CHK(r != NULL && r->type == J_STR);
        CHK(r && strcmp(r->str, "a\nb\tc\\d\"e/中😀") == 0);
        json_free(r);
    }
    /* OpenAI 风格 tool_calls (arguments 是字符串化 JSON, 需二次解析) */
    {
        JValue *r = json_parse("{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":[{\"id\":\"call_1\",\"function\":{\"name\":\"execute_bash\",\"arguments\":\"{\\\"command\\\":\\\"ls\\\"}\"}}]}}]}");
        CHK(r != NULL);
        const JValue *msg = json_obj_get(json_arr_at(json_obj_get(r,"choices"),0),"message");
        CHK(msg != NULL);
        const JValue *tcs = json_obj_get(msg,"tool_calls");
        CHK(tcs && tcs->type==J_ARR && tcs->arr.n==1);
        const JValue *tc0 = json_arr_at(tcs,0);
        CHK(json_as_str(json_obj_get(tc0,"id")) && strcmp(json_as_str(json_obj_get(tc0,"id")),"call_1")==0);
        const char *args = json_as_str(json_obj_get(json_obj_get(tc0,"function"),"arguments"));
        CHK(args != NULL);
        JValue *argsj = json_parse(args);
        CHK(argsj != NULL);
        CHK(json_as_str(json_obj_get(argsj,"command")) && strcmp(json_as_str(json_obj_get(argsj,"command")),"ls")==0);
        json_free(argsj);
        json_free(r);
    }
    /* 非法输入 */
    CHK(json_parse("{") == NULL);
    CHK(json_parse("[1,]") == NULL);
    CHK(json_parse("\"unterminated") == NULL);
    CHK(json_parse("") == NULL);
    /* BOM + 前后空白 */
    {
        JValue *r = json_parse("\xEF\xBB\xBF  {\"k\":\"v\"}  ");
        CHK(r != NULL && strcmp(json_as_str(json_obj_get(r,"k")),"v")==0);
        json_free(r);
    }

    /* DPAPI round-trip + 明文兼容 */
    {
        const char *plain = "sk-test-key-123";
        char *enc = dpapi_protect(plain);
        CHK(enc != NULL && strncmp(enc, "dpapi:", 6) == 0);
        char *dec = dpapi_unprotect(enc);
        CHK(dec != NULL && strcmp(dec, plain) == 0);
        free(enc); free(dec);
        char *dec2 = dpapi_unprotect("sk-plain-key");
        CHK(dec2 != NULL && strcmp(dec2, "sk-plain-key") == 0);
        free(dec2);
    }

    if (fails == 0) printf("json_selftest: OK\n");
    else printf("json_selftest: %d FAIL(s)\n", fails);
    return fails ? 1 : 0;
    #undef CHK
}


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
        test_ws_begin();   /* 隔离工作目录 (绝对路径, 建在 exe 同目录) */

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
        test_ws_rm("gbk.txt");

        /* 清理测试目录 */
        test_ws_rm("e.txt");
        test_ws_end();
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

    /* ---- SSE: arguments 超过旧 8KB 上限要完整保留, 超硬上限置 overflow 而非截断 ---- */
    {
        StreamCtx *c = (StreamCtx*)calloc(1, sizeof(StreamCtx));
        CHK(c != NULL);
        if (c) {
            enum { CHUNK = 4096 };              /* 每个 delta 片段字节数; 取 4KB 便于整除 ARGS_MAX */
            char piece[CHUNK + 1];
            memset(piece, 'a', CHUNK); piece[CHUNK] = '\0';
            /* 先灌到硬上限的四分之一: 旧版 8192 上限在这里就静默截出非法 JSON 了 */
            int quarter = (int)(ARGS_MAX / 4 / CHUNK);
            if (quarter < 1) quarter = 1;
            for (int i = 0; i < quarter; i++) {
                char text[CHUNK + 104];
                snprintf(text, sizeof(text),
                         "[{\"index\":0,\"function\":{\"name\":\"write_file\","
                         "\"arguments\":\"%s\"}}]", piece);
                JValue *tcs = json_parse(text);
                CHK(tcs != NULL);
                stream_apply_tool_calls(c, tcs);
                json_free(tcs);
            }
            CHK(c->calls[0].args_overflow == 0);
            CHK(strlen(c->calls[0].args) == (size_t)quarter * CHUNK);   /* 一个字节不少 */
            /* 继续灌到超过硬上限: 置标志、停止拼接, 由执行期拒绝执行 */
            for (int i = 0; i < 64 && !c->calls[0].args_overflow; i++) {
                char text[CHUNK + 104];
                snprintf(text, sizeof(text),
                         "[{\"index\":0,\"function\":{\"name\":\"write_file\","
                         "\"arguments\":\"%s\"}}]", piece);
                JValue *tcs = json_parse(text);
                CHK(tcs != NULL);
                stream_apply_tool_calls(c, tcs);
                json_free(tcs);
            }
            CHK(c->calls[0].args_overflow == 1);              /* 超限有信号, 不再静默 */
            /* 原来是 strlen(args) < sizeof(args) —— 对正确 NUL 结尾的缓冲恒真, 等于没测。
             * 换成真检查: 停在上限之内、正好落在片段边界、内容没错位。 */
            size_t al2 = strlen(c->calls[0].args);
            CHK(al2 > 0 && al2 < (size_t)ARGS_MAX);
            CHK(al2 % CHUNK == 0);
            int all_a = 1;
            for (size_t k = 0; k < al2; k++)
                if (c->calls[0].args[k] != 'a') { all_a = 0; break; }
            CHK(all_a);
            free(c);
        }
    }

    /* ---- UTF-8 预检返回值 (供发送前拦截) + 错误预览的安全截取 ---- */
    {
        CHK(log_check_utf8("t", "你好", strlen("你好")) == 0);       /* 合法 → 0 */
        const char half[] = {'a', (char)0xE4, 'b', 0};               /* 半截多字节 */
        CHK(log_check_utf8("t", half, 3) == 1);                      /* 非法 → 1 (可拦截) */
        CHK(log_check_utf8("t", "\xC0\xAF", 2) == 1);                /* 过长编码 → 1 */
        CHK(log_check_utf8("t", "\xED\xA0\x80", 3) == 1);            /* 代理区 → 1 */
        CHK(log_check_utf8("t", "\x80", 1) == 1);                    /* 孤立续字节 → 1 */

        char out[64];
        utf8_safe_copy("a中b", 2, out, sizeof(out));                 /* 第 2 字节切在"中"中间 */
        CHK(strcmp(out, "a") == 0);                                  /* 整字符丢弃, 不出半个 */
        utf8_safe_copy("a中b", 4, out, sizeof(out));                 /* 恰好完整"中" */
        CHK(strcmp(out, "a中") == 0);
        utf8_safe_copy("a中b", 99, out, sizeof(out));                /* 不需要截断 */
        CHK(strcmp(out, "a中b") == 0);
        const char badsrc[] = {'x', (char)0xFF, 'y', 0};             /* 源本身含非法字节 */
        utf8_safe_copy(badsrc, 10, out, sizeof(out));
        CHK(strcmp(out, "xy") == 0);                                 /* 输出仍合法 */
        CHK(log_check_utf8("t", out, strlen(out)) == 0);

        /* 严格性回归: 旧实现用位掩码判定首字节, 下列非法序列全被放过 → 预检形同虚设 */
        CHK(log_check_utf8("t", "\xC0\x80", 2) == 1);                /* overlong 2B */
        CHK(log_check_utf8("t", "\xC1\xBF", 2) == 1);                /* overlong 2B */
        CHK(log_check_utf8("t", "\xE0\x80\x80", 3) == 1);            /* overlong 3B */
        CHK(log_check_utf8("t", "\xF0\x80\x80\x80", 4) == 1);        /* overlong 4B */
        CHK(log_check_utf8("t", "\xF4\x90\x80\x80", 4) == 1);        /* 超 U+10FFFF */
        CHK(log_check_utf8("t", "\xF4\x8F\xBF\xBF", 4) == 0);        /* 边界 U+10FFFF 合法 */
    }

    /* ---- 就地净化: 长度不变 (messages/固定缓冲按容量算), 结果必为合法 UTF-8 ---- */
    {
        char b1[] = "a中b";                                       /* 合法: 一个字节都不动 */
        CHK(utf8_sanitize_inplace(b1) == 0);
        CHK(strcmp(b1, "a中b") == 0);

        char b2[] = {'a', (char)0xE4, (char)0xB8, 'b', 0};         /* "中" 少一个字节 */
        size_t n2 = strlen(b2);
        CHK(utf8_sanitize_inplace(b2) == 2);                       /* E4 与 B8 各是一个坏字节 */
        CHK(strlen(b2) == n2);                                     /* 长度不变 */
        CHK(strcmp(b2, "a??b") == 0);
        CHK(is_valid_utf8((const unsigned char*)b2, strlen(b2)) == 1);

        char b3[] = "GBK:\xc4\xe3\xba\xc3";                        /* 中文 Windows 上的 GBK 字节 */
        n2 = strlen(b3);
        CHK(utf8_sanitize_inplace(b3) == 4);
        CHK(strlen(b3) == n2);
        CHK(is_valid_utf8((const unsigned char*)b3, strlen(b3)) == 1);
    }

    /* ---- 坏字节归属的消息序号: 0-based, 且跳过字符串内的字面 {"role" ---- */
    {
        const char *b = "{\"model\":\"m\",\"messages\":["
                        "{\"role\":\"user\",\"content\":\"hi\"},"
                        "{\"role\":\"assistant\",\"content\":\"x {\\\"role\\\":\\\"fake\\\"} y\"},"
                        "{\"role\":\"user\",\"content\":\"ZZ\"}"
                        "],\"tools\":[{\"role\":\"x\"}]}";
        size_t bl = strlen(b);
        const char *p = strstr(b, "\"ZZ\"");   /* 第 3 条: 旧实现会因 content 内假 {"role" 与 tools 段而数到 4 */
        CHK(p != NULL && msg_index_of(b, bl, (size_t)(p - b)) == 2);
        p = strstr(b, "\"hi\"");
        CHK(p != NULL && msg_index_of(b, bl, (size_t)(p - b)) == 0);   /* 第 1 条 → 0 (不再差 1) */
        CHK(msg_index_of(b, bl, 5) == 0);                              /* 坏字节在 messages 之前 */
    }

    /* ---- 429 重试策略: 限流窗口比普通抖动长, 单独给到 6 次 (实测 3 次会白丢整轮) ---- */
    if (!getenv("CAGENT_HTTP_RETRIES")) {        /* 环境变量覆盖时语义不同, 跳过 */
        CHK(http_max_retries_for(429) == HTTP_RATE_RETRIES);
        CHK(http_max_retries_for(500) == http_max_retries());   /* 其余维持默认 */
        CHK(http_max_retries_for(-1)  == http_max_retries());
        CHK(http_max_retries_for(429) > http_max_retries());     /* 429 确实更宽 */
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
        test_ws_begin();
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
        test_ws_end();
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

    /* ---- 会话原子写: 失败时目标必须原样不动, 且不留 .tmp ----
     * 会话是整文件覆盖的, 老的 fopen(path,"wb") 会先截断旧内容, 中途出事就全丢。
     * 这里用"把目标设为只读"构造 MoveFileEx 失败 (实测返回 ERROR_ACCESS_DENIED),
     * 验证旧内容完好 —— 这正是原子写相对直接覆盖的核心保证。 */
    {
        test_ws_begin();
        char atomic[MAX_PATH], atomictmp[MAX_PATH];
        snprintf(atomic,    sizeof(atomic),    "%s\\atomic.json",     g_test_ws);
        snprintf(atomictmp, sizeof(atomictmp), "%s\\atomic.json.tmp", g_test_ws);
        wchar_t wdst[MAX_PATH], wtmp[MAX_PATH];
        CHK(utf8_to_wide(atomic, wdst, MAX_PATH));
        CHK(utf8_to_wide(atomictmp, wtmp, MAX_PATH));

        char buf[256];

        /* 1. 正常写: 内容正确, 且不残留中间文件 */
        CHK(session_write_atomic(atomic, "WS", ",{\"role\":\"user\",\"content\":\"hi\"}") == 1);
        buf[0] = '\0';
        { FILE *f = fopen_utf8(atomic, "rb");
          if (f) { fread(buf, 1, sizeof(buf) - 1, f); fclose(f); } }
        CHK(strstr(buf, "\"workspace\":\"WS\"") != NULL);
        CHK(strstr(buf, "\"messages\":[") != NULL);
        CHK(strstr(buf, "hi") != NULL);
        CHK(GetFileAttributesW(wtmp) == INVALID_FILE_ATTRIBUTES);   /* .tmp 已随替换消失 */

        /* 2. 写入失败 (目标只读 -> MoveFileEx 拒绝): 旧内容完好, 新内容没混进去 */
        SetFileAttributesW(wdst, FILE_ATTRIBUTE_READONLY);
        CHK(session_write_atomic(atomic, "WS2", ",{\"role\":\"user\",\"content\":\"NEW\"}") == 0);
        SetFileAttributesW(wdst, FILE_ATTRIBUTE_NORMAL);            /* 复位, 否则删不掉 */
        buf[0] = '\0';
        { FILE *f = fopen_utf8(atomic, "rb");
          if (f) { fread(buf, 1, sizeof(buf) - 1, f); fclose(f); } }
        CHK(strstr(buf, "hi") != NULL);        /* 旧内容原封不动 */
        CHK(strstr(buf, "NEW") == NULL);       /* 失败的那次没污染目标 */
        CHK(GetFileAttributesW(wtmp) == INVALID_FILE_ATTRIBUTES);   /* 失败路径也清掉了 .tmp */

        test_ws_rm("atomic.json");
        test_ws_rm("atomic.json.tmp");
        test_ws_end();
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
        test_ws_begin();

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

            test_ws_rm("big.txt");
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

            test_ws_rm("u8.txt");
        }

        test_ws_end();
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

    /* ---- 技能: frontmatter 解析 / 目录扫描 / load_skill 分发 ---- */
    {
        char nm[SKILL_NAME_MAX], ds[SKILL_DESC_MAX];

        /* 标准 frontmatter: name + description 各取其值 */
        skill_parse_frontmatter("---\nname: demo\ndescription: \xe6\xbc\x94\xe7\xa4\xba\xe6\x8a\x80\xe8\x83\xbd\n---\n\xe6\xad\xa3\xe6\x96\x87",
                                nm, sizeof(nm), ds, sizeof(ds));
        CHK(strcmp(nm, "demo") == 0);
        CHK(strcmp(ds, "\xe6\xbc\x94\xe7\xa4\xba\xe6\x8a\x80\xe8\x83\xbd") == 0);   /* 演示技能 */

        /* CRLF 行尾同样识别 */
        skill_parse_frontmatter("---\r\nname: crlf\r\ndescription: d\r\n---\r\nbody",
                                nm, sizeof(nm), ds, sizeof(ds));
        CHK(strcmp(nm, "crlf") == 0 && strcmp(ds, "d") == 0);

        /* 无 frontmatter: 两个都为空, 由调用方回退目录名/正文首行 */
        skill_parse_frontmatter("plain text", nm, sizeof(nm), ds, sizeof(ds));
        CHK(nm[0] == '\0' && ds[0] == '\0');

        /* 只有 name: desc 为空 (frontmatter 合法收尾, 不吃进正文) */
        skill_parse_frontmatter("---\nname: only\n---\nbody line", nm, sizeof(nm), ds, sizeof(ds));
        CHK(strcmp(nm, "only") == 0 && ds[0] == '\0');

        /* 收尾 --- 缺失: 整体视为无 frontmatter (否则会吞掉正文) */
        skill_parse_frontmatter("---\nname: unclosed\nbody", nm, sizeof(nm), ds, sizeof(ds));
        CHK(nm[0] == '\0');

        /* 正文定位: 跳过 frontmatter, 无 frontmatter 原样返回 */
        CHK(strcmp(skill_body("---\nname: x\n---\nBODY") , "BODY") == 0);
        CHK(strcmp(skill_body("BODY2"), "BODY2") == 0);
    }
    /* 技能测试复位: 三状态 + 环境变量一起清, 不泄漏到 exe 目录的真实技能集 */
    #define SKILLS_RESET() \
        do { g_skills_dir[0] = '\0'; g_skill_count = 0; g_skills_suffix[0] = '\0'; \
             _putenv("CAGENT_SKILLS_DIR="); } while (0)
    {
        /* 扫描 + 索引 + 分发: CAGENT_SKILLS_DIR 指到测试目录, 不碰 exe 目录 */
        test_ws_begin();
        char sdir[MAX_PATH];
        snprintf(sdir, sizeof(sdir), "%s\\skills", g_test_ws);
        wchar_t w[MAX_PATH];
        {
            char d2[MAX_PATH];
            snprintf(d2, sizeof(d2), "%s\\demo", sdir);
            if (utf8_to_wide(sdir, w, MAX_PATH)) CreateDirectoryW(w, NULL);
            if (utf8_to_wide(d2, w, MAX_PATH)) CreateDirectoryW(w, NULL);
        }
        /* 技能文件落在工作目录内, 用 write_file 写 (注意它会记账, 无妨) */
        tool_write_file("skills\\demo\\SKILL.md",
            "---\nname: demo\ndescription: \xe6\xbc\x94\xe7\xa4\xba\n---\nDEMO-SKILL-BODY \xe6\xad\xa5\xe9\xaa\xa4");

        {
            char env[512];
            snprintf(env, sizeof(env), "CAGENT_SKILLS_DIR=%s", sdir);
            _putenv(env);
            skills_init();
            CHK(g_skill_count == 1);
            CHK(strcmp(g_skills[0].name, "demo") == 0);
            CHK(strstr(g_skills[0].desc, "\xe6\xbc\x94\xe7\xa4\xba") != NULL);   /* 演示 */
            CHK(strstr(g_skills_suffix, "load_skill") != NULL);
            CHK(strstr(g_skills_suffix, "demo") != NULL);
            /* 索引拼进 system 消息: 换行被转义, 中文原样透传 */
            CHK(system_prompt_set_raw("base") == 1);
            CHK(strstr(g_system_prompt, "\\n\\n## ") != NULL);
            CHK(strstr(g_system_prompt, "load_skill") != NULL);
            CHK(strstr(g_system_prompt, "可用技能") != NULL);
            g_system_prompt = SYSTEM_PROMPT_DEFAULT;   /* 复位, 免得泄漏到后续用例 */

            /* load_skill: 正常加载 (正文 + 执行指示) */
            dispatch_tool("load_skill", "{\"name\":\"demo\"}", "");
            CHK(strstr(tool_out, "[技能: demo]") != NULL);
            CHK(strstr(tool_out, "DEMO-SKILL-BODY") != NULL);
            CHK(strstr(tool_out, "严格按") != NULL);

            /* 未知名: 列出可用技能, 模型可自行纠正 */
            dispatch_tool("load_skill", "{\"name\":\"nope\"}", "");
            CHK(strstr(tool_out, "未找到技能") != NULL && strstr(tool_out, "demo") != NULL);

            /* 参数缺失 */
            dispatch_tool("load_skill", "{}", "");
            CHK(strstr(tool_out, "参数缺失") != NULL);

            SKILLS_RESET();
        }

        /* ini 相对路径 skills_dir: 按 exe 目录解析 (cagent_test.exe 在仓库根,
         * 与 test_ws_begin 建的 g_test_ws 同锚点), 不随 CWD 漂移 */
        snprintf(g_skills_dir, sizeof(g_skills_dir), "cagent_test_ws\\skills");
        skills_init();
        CHK(g_skill_count == 1 && strcmp(g_skills[0].name, "demo") == 0);
        SKILLS_RESET();

        /* UTF-8 BOM: 先剥 BOM 再解析, frontmatter 不受影响
         * (frontmatter name 故意与目录名不同, 证明确实来自解析而非回退) */
        {
            char d[MAX_PATH];
            snprintf(d, sizeof(d), "%s\\skills\\bom", g_test_ws);
            if (utf8_to_wide(d, w, MAX_PATH)) CreateDirectoryW(w, NULL);
        }
        tool_write_file("skills\\bom\\SKILL.md",
            "\xEF\xBB\xBF---\nname: bomskill\ndescription: BOM \xe6\xb5\x8b\xe8\xaf\x95\n---\nBOM-SKILL-BODY");
        snprintf(g_skills_dir, sizeof(g_skills_dir), "cagent_test_ws\\skills");
        skills_init();
        CHK(g_skill_count == 2);                                  /* demo + bom */
        {   /* 枚举按字母序, 不硬编码下标 */
            Skill *b = (strcmp(g_skills[0].name, "bomskill") == 0) ? &g_skills[0] : &g_skills[1];
            CHK(strcmp(b->name, "bomskill") == 0);                /* frontmatter 生效 */
            CHK(strstr(b->desc, "BOM") != NULL);
        }
        dispatch_tool("load_skill", "{\"name\":\"bomskill\"}", "");
        CHK(strstr(tool_out, "BOM-SKILL-BODY") != NULL);
        CHK(strstr(tool_out, "\xEF\xBB\xBF") == NULL);            /* BOM 不进正文 */
        test_ws_rm("skills\\bom\\SKILL.md");
        SKILLS_RESET();

        /* GBK 编码的 SKILL.md: 非 UTF-8 时按系统 OEM 代码页自动转码。
         * 依赖中文代码页 (936), 其他语言的机器上跳过。 */
        if (GetOEMCP() == 936) {
            char d[MAX_PATH];
            snprintf(d, sizeof(d), "%s\\skills\\gbk", g_test_ws);
            if (utf8_to_wide(d, w, MAX_PATH)) CreateDirectoryW(w, NULL);
            tool_write_file("skills\\gbk\\SKILL.md",
                "---\nname: gbk\ndescription: \xd1\xdd\xca\xbe\n---\nGBK-SKILL-BODY");
            snprintf(g_skills_dir, sizeof(g_skills_dir), "cagent_test_ws\\skills");
            skills_init();
            CHK(g_skill_count == 2);                              /* demo + gbk */
            {
                Skill *k = (strcmp(g_skills[0].name, "gbk") == 0) ? &g_skills[0] : &g_skills[1];
                CHK(strcmp(k->name, "gbk") == 0);
                CHK(strstr(k->desc, "\xe6\xbc\x94\xe7\xa4\xba") != NULL);  /* 转码后的 "演示" */
            }
            dispatch_tool("load_skill", "{\"name\":\"gbk\"}", "");
            CHK(strstr(tool_out, "GBK-SKILL-BODY") != NULL);
            test_ws_rm("skills\\gbk\\SKILL.md");
            SKILLS_RESET();
        }

        /* ini 手写误差: 首尾空白与成对引号剥掉; 技能目录名可含空格 */
        {
            char d[MAX_PATH];
            snprintf(d, sizeof(d), "%s\\skills\\my skill", g_test_ws);
            if (utf8_to_wide(d, w, MAX_PATH)) CreateDirectoryW(w, NULL);
        }
        tool_write_file("skills\\my skill\\SKILL.md",
            "---\nname: spaced\ndescription: \xe5\x90\xab\xe7\xa9\xba\xe6\xa0\xbc\n---\nSPACED-SKILL-BODY");
        snprintf(g_skills_dir, sizeof(g_skills_dir),
                 "  \"cagent_test_ws\\skills\"  ");
        skills_init();
        CHK(g_skill_count == 2);                                  /* demo + spaced */
        {
            Skill *k = NULL;
            for (int i = 0; i < g_skill_count; i++)
                if (strcmp(g_skills[i].name, "spaced") == 0) { k = &g_skills[i]; break; }
            CHK(k != NULL);                                       /* 引号/空白不影响发现 */
            dispatch_tool("load_skill", "{\"name\":\"spaced\"}", "");
            CHK(strstr(tool_out, "SPACED-SKILL-BODY") != NULL);
        }
        test_ws_rm("skills\\my skill\\SKILL.md");
        SKILLS_RESET();

        /* 递归扫描: 任意深度的 SKILL.md 都发现; 根下直接放 SKILL.md 也算;
         * 根互相嵌套/重叠时同一文件被多次发现, 靠重名先到先得去重 */
        {
            char d[MAX_PATH];
            const char *dirs[] = { "skills3", "skills3\\a", "skills3\\a\\deep",
                                   "skills3\\a\\deep\\inner" };
            for (int i = 0; i < 4; i++) {
                snprintf(d, sizeof(d), "%s\\%s", g_test_ws, dirs[i]);
                if (utf8_to_wide(d, w, MAX_PATH)) CreateDirectoryW(w, NULL);
            }
        }
        tool_write_file("skills3\\SKILL.md",
            "ROOT-LEVEL-SKILL-BODY");   /* 无 frontmatter: 回退名 = 所在目录名 */
        tool_write_file("skills3\\a\\deep\\inner\\SKILL.md",
            "---\nname: deepskill\ndescription: \xe6\xb7\xb1\xe5\xb1\x82\xe6\x8a\x80\xe8\x83\xbd\n---\nDEEP-SKILL-BODY");
        /* 双根且后者嵌套在前者里: inner 的 SKILL.md 被发现两次, 不重复计数 */
        snprintf(g_skills_dir, sizeof(g_skills_dir),
                 "cagent_test_ws\\skills3;cagent_test_ws\\skills3\\a");
        skills_init();
        CHK(g_skill_count == 2);
        {
            Skill *r = NULL, *d2 = NULL;
            for (int i = 0; i < g_skill_count; i++) {
                if (strcmp(g_skills[i].name, "skills3") == 0) r = &g_skills[i];
                if (strcmp(g_skills[i].name, "deepskill") == 0) d2 = &g_skills[i];
            }
            CHK(r != NULL && d2 != NULL);         /* 根下 SKILL.md 回退名 = 根目录名 */
            CHK(strstr(d2->desc, "\xe6\xb7\xb1\xe5\xb1\x82") != NULL);
        }
        dispatch_tool("load_skill", "{\"name\":\"deepskill\"}", "");
        CHK(strstr(tool_out, "DEEP-SKILL-BODY") != NULL);
        test_ws_rm("skills3\\SKILL.md");
        test_ws_rm("skills3\\a\\deep\\inner\\SKILL.md");
        SKILLS_RESET();

        /* 未闭合的 frontmatter (少收尾 ---): name 回退目录名, desc 回退跳过
         * "---" 与键值行取真正的正文首行, 不把元数据带进索引 */
        {
            char d[MAX_PATH];
            const char *dirs[] = { "skills4", "skills4\\broken" };
            for (int i = 0; i < 2; i++) {
                snprintf(d, sizeof(d), "%s\\%s", g_test_ws, dirs[i]);
                if (utf8_to_wide(d, w, MAX_PATH)) CreateDirectoryW(w, NULL);
            }
        }
        tool_write_file("skills4\\broken\\SKILL.md",
            "---\nname: brokenname\ndescription: \xe4\xb8\x8d\xe8\xaf\xa5\xe5\x87\xba\xe7\x8e\xb0\n"
            "\xe6\xad\xa3\xe6\x96\x87\xe9\xa6\x96\xe8\xa1\x8c\n");
        snprintf(g_skills_dir, sizeof(g_skills_dir), "cagent_test_ws\\skills4");
        skills_init();
        CHK(g_skill_count == 1);
        CHK(strcmp(g_skills[0].name, "broken") == 0);             /* 目录名回退 */
        CHK(strstr(g_skills[0].desc, "---") == NULL);
        CHK(strstr(g_skills[0].desc, "name:") == NULL);
        CHK(strstr(g_skills[0].desc, "\xe6\xad\xa3\xe6\x96\x87\xe9\xa6\x96\xe8\xa1\x8c") != NULL);
        test_ws_rm("skills4\\broken\\SKILL.md");
        SKILLS_RESET();

        /* 多目录: ';' 分隔, 按顺序扫描; 重名先到先得 (同一根内与跨根都去重) */
        {
            char d[MAX_PATH];
            /* CreateDirectoryW 不建父级, 父目录排在子目录之前 */
            const char *dirs[] = { "skills2", "skills\\demo2", "skills\\other",
                                   "skills2\\deep", "skills2\\demo" };
            for (int i = 0; i < 4; i++) {
                snprintf(d, sizeof(d), "%s\\%s", g_test_ws, dirs[i]);
                if (utf8_to_wide(d, w, MAX_PATH)) CreateDirectoryW(w, NULL);
            }
        }
        tool_write_file("skills\\demo2\\SKILL.md",
            "---\nname: demo\ndescription: \xe5\x90\x8c\xe6\xa0\xb9\xe9\x87\x8d\xe5\x90\x8d\n---\nSECOND-SKILL-BODY");
        tool_write_file("skills\\other\\SKILL.md",
            "---\nname: other\ndescription: \xe5\x8f\xa6\xe4\xb8\x80\xe4\xb8\xaa\n---\nOTHER-SKILL-BODY");
        tool_write_file("skills2\\deep\\SKILL.md",
            "---\nname: deep\ndescription: \xe7\xac\xac\xe4\xba\x8c\xe6\xa0\xb9\n---\nDEEP-SKILL-BODY");
        tool_write_file("skills2\\demo\\SKILL.md",
            "---\nname: demo\ndescription: \xe8\xb7\xa8\xe6\xa0\xb9\xe9\x87\x8d\xe5\x90\x8d\n---\nTHIRD-SKILL-BODY");

        /* 单根: demo(demo 目录) + other; 同根内重名的 demo2 被跳过 */
        snprintf(g_skills_dir, sizeof(g_skills_dir), "cagent_test_ws\\skills");
        skills_init();
        CHK(g_skill_count == 2);
        CHK(strcmp(g_skills[0].name, "demo") == 0);
        CHK(strcmp(g_skills[1].name, "other") == 0);

        /* 双根: 第二根贡献 deep; 跨根重名的 demo 仍是第一根的 */
        snprintf(g_skills_dir, sizeof(g_skills_dir),
                 "cagent_test_ws\\skills;cagent_test_ws\\skills2");
        skills_init();
        CHK(g_skill_count == 3);
        CHK(strcmp(g_skills[2].name, "deep") == 0);
        dispatch_tool("load_skill", "{\"name\":\"demo\"}", "");
        CHK(strstr(tool_out, "DEMO-SKILL-BODY") != NULL);
        CHK(strstr(tool_out, "THIRD-SKILL-BODY") == NULL);

        /* 同一根重复出现: 去重, 不重复计数 */
        snprintf(g_skills_dir, sizeof(g_skills_dir),
                 "cagent_test_ws\\skills;cagent_test_ws\\skills");
        skills_init();
        CHK(g_skill_count == 2);

        /* 空段与尾分号容忍 */
        snprintf(g_skills_dir, sizeof(g_skills_dir), ";cagent_test_ws\\skills;");
        skills_init();
        CHK(g_skill_count == 2);

        SKILLS_RESET();

        test_ws_rm("skills\\demo\\SKILL.md");
        test_ws_rm("skills\\demo2\\SKILL.md");
        test_ws_rm("skills\\other\\SKILL.md");
        test_ws_rm("skills2\\deep\\SKILL.md");
        test_ws_rm("skills2\\demo\\SKILL.md");
        {
            char d[MAX_PATH];
            const char *dirs[] = { "skills\\demo", "skills\\demo2", "skills\\other",
                                   "skills2\\deep", "skills2\\demo", "skills2" };
            for (int i = 0; i < 6; i++) {
                snprintf(d, sizeof(d), "%s\\%s", g_test_ws, dirs[i]);
                if (utf8_to_wide(d, w, MAX_PATH)) RemoveDirectoryW(w);
            }
        }
        {
            char d2[MAX_PATH];
            wchar_t w2[MAX_PATH];
            snprintf(d2, sizeof(d2), "%s\\demo", sdir);
            if (utf8_to_wide(d2, w2, MAX_PATH)) RemoveDirectoryW(w2);
            if (utf8_to_wide(sdir, w2, MAX_PATH)) RemoveDirectoryW(w2);
        }
        test_ws_end();
    }
    #undef SKILLS_RESET

    /* ===== HTTP 错误分类: 额度识别 / 友好消息 ===== */
    {
        /* 额度错误: 402 一律算; 403/429 只在报文命中关键字时算 */
        CHK(http_is_quota_error(402, "") == 1);
        CHK(http_is_quota_error(402, NULL) == 1);
        CHK(http_is_quota_error(403, "Insufficient Balance in account") == 1);
        CHK(http_is_quota_error(429, "{\"error\":{\"code\":\"insufficient_quota\"}}") == 1);
        CHK(http_is_quota_error(429, "Arrearage: account in debt") == 1);
        CHK(http_is_quota_error(429, "\xe4\xbd\x99\xe9\xa2\x9d\xe4\xb8\x8d\xe8\xb6\xb3") == 1);  /* 余额不足 */
        /* 普通限流/无关错误不算: 裸 "quota" 与 rate limit 不误判 */
        CHK(http_is_quota_error(429, "Too many requests, rate limit exceeded") == 0);
        CHK(http_is_quota_error(429, "This model's maximum context length is 8192 tokens") == 0);
        CHK(http_is_quota_error(400, "insufficient_quota") == 0);   /* 非 402/403/429 */
        CHK(http_is_quota_error(500, "Insufficient Balance") == 0);
        /* 大小写不敏感 */
        CHK(http_is_quota_error(403, "INSUFFICIENT BALANCE") == 1);

        /* 友好消息: 命中写 out 返回 1; 未命中返回 0 (调用方展示原始报文) */
        char fr[512];
        CHK(http_friendly_error(401, "", fr, sizeof(fr)) == 1);
        CHK(strstr(fr, "API Key") != NULL);
        CHK(http_friendly_error(402, "any", fr, sizeof(fr)) == 1);
        CHK(strstr(fr, "\xe9\xa2\x9d\xe5\xba\xa6\xe7\x94\xa8\xe5\xae\x8c") != NULL);   /* 额度用完 */
        CHK(http_friendly_error(429, "rate limited", fr, sizeof(fr)) == 1);
        CHK(strstr(fr, "429") != NULL);
        CHK(http_friendly_error(400, "bad request", fr, sizeof(fr)) == 0);   /* 未命中 */
        CHK(http_friendly_error(500, "server error", fr, sizeof(fr)) == 0);

        /* ascii_icontains 基础行为 */
        CHK(ascii_icontains("hello World", "world") == 1);
        CHK(ascii_icontains("hello", "hello!") == 0);
        CHK(ascii_icontains("abc", "") == 1);
    }

    #undef CHK
    if (fails == 0) printf("run_all_tests: OK\n");
    else printf("run_all_tests: %d FAIL(s)\n", fails);
    return fails ? 1 : 0;
}
