/*
 * core/tools.h - 结构化工具: execute_bash / read_file / write_file
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* 工具结果上限 (字节): 默认 16KB, 可用环境变量 CAGENT_TOOL_OUTPUT_MAX 覆盖。
 * 目的: 单次工具输出不至于撑爆上下文; 截断时明确告诉模型如何取到余下内容。 */
static size_t tool_output_cap(void) {
    char *e = getenv("CAGENT_TOOL_OUTPUT_MAX");
    long v = (e && atoi(e) > 0) ? atol(e) : (long)TOOL_OUTPUT_CAP;
    if (v > (long)BUFSZ - 512) v = (long)BUFSZ - 512;   /* 不得超过 tool_out 缓冲 */
    return (size_t)v;
}

static void execute_bash(const char *command) {
    /* 极简理念: 不拦截命令, 由用户自己承担运行环境的风险 (建议跑在容器中)。 */
    int n = run_pipe(command, tool_out, tool_output_cap());
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
    size_t cap = tool_output_cap();
    size_t raw = fread(tool_out, 1, cap, f);
    size_t n = utf8_trim_len(tool_out, raw);   /* 对齐字符边界, 避免切出半个字符 */
    /* 文本不是合法 UTF-8 (常见: 中文 Windows 下的 GBK 文本) -> 转成 UTF-8 */
    if (n > 0 && !is_valid_utf8((const unsigned char *)tool_out, n)) {
        tool_out[n] = '\0';
        oem_to_utf8(tool_out, BUFSZ);
        n = strlen(tool_out);
    }
    /* 读满上限时确认是否还有剩余内容, 并给出文件总大小 */
    int more = 0;
    long long total = -1;
    if (raw == cap) {
        if (_fseeki64(f, 0, SEEK_END) == 0) {
            total = _ftelli64(f);
            more = (total > (long long)raw);
        } else {
            more = 1;
        }
    }
    fclose(f);
    tool_out[n] = '\0';
    if (more) {
        char note[256];
        if (total > 0)
            snprintf(note, sizeof(note),
                     "\n...(已截断: 文件共 %lld 字节, 仅返回前 %zu 字节; "
                     "剩余部分可用 execute_bash 配合 findstr/分段命令查看)", total, n);
        else
            snprintf(note, sizeof(note),
                     "\n...(已截断: 仅返回前 %zu 字节; 文件较大, 请分段查看)", n);
        strncat(tool_out, note, BUFSZ - strlen(tool_out) - 1);
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


/* 按名称分发一次工具调用: arguments 是字符串化 JSON。
 * 出错时把"可读的原因"写进 tool_out 交回模型 (缺失参数/非法 JSON/未知工具),
 * 由模型自行决定重试或换策略。finish 用于在截断场景下给出更准确的提示。 */
static void dispatch_tool(const char *name, const char *args_json, const char *finish) {
    const char *nm = (name && name[0]) ? name : "(空)";
    JValue *argsj = json_parse(args_json ? args_json : "");

    if (argsj && argsj->type == J_OBJ) {
        if (strcmp(nm, "execute_bash") == 0) {
            const char *cmd = json_as_str(json_obj_get(argsj, "command"));
            if (cmd) execute_bash(cmd);
            else     snprintf(tool_out, BUFSZ, "(参数缺失: %s 需要字符串参数 \"command\")", nm);
        } else if (strcmp(nm, "read_file") == 0) {
            const char *p = json_as_str(json_obj_get(argsj, "path"));
            if (p) tool_read_file(p);
            else   snprintf(tool_out, BUFSZ, "(参数缺失: %s 需要字符串参数 \"path\")", nm);
        } else if (strcmp(nm, "write_file") == 0) {
            const char *p  = json_as_str(json_obj_get(argsj, "path"));
            const char *ct = json_as_str(json_obj_get(argsj, "content"));
            if (p && ct)  tool_write_file(p, ct);
            else if (!p)  snprintf(tool_out, BUFSZ, "(参数缺失: write_file 需要字符串参数 \"path\")");
            else          snprintf(tool_out, BUFSZ, "(参数缺失: write_file 需要字符串参数 \"content\")");
        } else {
            snprintf(tool_out, BUFSZ, "(未知工具: %s; 可用工具: execute_bash / read_file / write_file)", nm);
        }
    } else if (!argsj) {
        snprintf(tool_out, BUFSZ,
                 "(参数解析失败: %s 的 arguments 不是合法 JSON%s, 原始内容前 200 字节: %.200s)",
                 nm, (finish && strcmp(finish, "length") == 0) ? " (疑似被长度上限截断)" : "",
                 (args_json && args_json[0]) ? args_json : "(空)");
    } else {
        snprintf(tool_out, BUFSZ, "(参数类型错误: arguments 应为 JSON 对象)");
    }
    json_free(argsj);
}
