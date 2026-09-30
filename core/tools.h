/*
 * core/tools.h - 结构化工具: execute_bash / read_file / write_file
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

static void execute_bash(const char *command) {
    /* 极简理念: 不拦截命令, 由用户自己承担运行环境的风险 (建议跑在容器中)。 */
    int n = run_pipe(command, tool_out, BUFSZ);
    if (n <= 0) { strcpy(tool_out, "(no output)"); return; }
    /* 输出已是合法 UTF-8 则原样保留, 否则才做 OEM(GBK) -> UTF-8 转换 */
    if (!is_valid_utf8((const unsigned char *)tool_out, (size_t)n))
        oem_to_utf8(tool_out, BUFSZ);
}



static void tool_read_file(const char *path) {
    if (!path_in_workspace(path)) { snprintf(tool_out, BUFSZ, "(拒绝: 路径在工作目录外)"); return; }
    wchar_t wpath[MAX_PATH];
    if (!resolve_in_workspace(path, wpath, MAX_PATH)) { snprintf(tool_out, BUFSZ, "(读取失败: 路径过长)"); return; }
    FILE *f = _wfopen(wpath, L"rb");
    if (!f) { snprintf(tool_out, BUFSZ, "(读取失败: 无法打开 %s)", path); return; }
    size_t n = fread(tool_out, 1, BUFSZ - 64, f);
    n = utf8_trim_len(tool_out, n);          /* 截断对齐到字符边界, 避免半个字符 */
    fclose(f);
    tool_out[n] = '\0';
    if (n >= BUFSZ - 64) {
        strcat(tool_out, "\n(已截断, 文件过大)");
    }
}

static void tool_write_file(const char *path, const char *content) {
    if (!path_in_workspace(path)) { strcpy(tool_out, "(拒绝: 路径在工作目录外)"); return; }
    wchar_t wpath[MAX_PATH];
    if (!resolve_in_workspace(path, wpath, MAX_PATH)) { strcpy(tool_out, "(写入失败: 路径过长)"); return; }
    FILE *f = _wfopen(wpath, L"wb");
    if (!f) { strcpy(tool_out, "(写入失败)"); return; }
    size_t len = strlen(content);
    fwrite(content, 1, len, f);
    fclose(f);
    snprintf(tool_out, BUFSZ, "(已写入 %zu 字节)", len);
}

