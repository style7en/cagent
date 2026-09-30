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

        /* 写一个测试文件 (ASCII, 合法 UTF-8) */
        tool_write_file("e.txt", "alpha\nbeta\ngamma\n");
        CHK(strstr(tool_out, "已写入") != NULL);

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

    #undef CHK
    if (fails == 0) printf("run_all_tests: OK\n");
    else printf("run_all_tests: %d FAIL(s)\n", fails);
    return fails ? 1 : 0;
}
