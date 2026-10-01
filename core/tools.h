/*
 * core/tools.h - 结构化工具: execute_bash / read_file / write_file / edit_file
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

/* 记录本轮改动过的文件: 对话可以回滚, 已落盘的改动不能, 回滚提示里要列清楚。 */
static void touch_file(const char *path) {
    size_t used = strlen(g_touched_files);
    size_t need = strlen(path) + 2;
    if (used + need >= sizeof(g_touched_files)) return;   /* 放不下就不再记, 不覆盖已有 */
    snprintf(g_touched_files + used, sizeof(g_touched_files) - used, "%s, ", path);
}

static void execute_bash(const char *command) {
    /* 极简理念: 不拦截命令, 由用户自己承担运行环境的风险 (建议跑在容器中)。
     * run_pipe 会在输出末尾附 [exit=N], 模型据此判断命令成败。 */
    int n = run_pipe(command, tool_out, tool_output_cap());
    if (n < 0) { strcpy(tool_out, "(命令启动失败: 无法创建进程)"); return; }
    if (n <= 0) { strcpy(tool_out, "(no output)"); return; }
    /* 输出已是合法 UTF-8 则原样保留, 否则才做 OEM(GBK) -> UTF-8 转换 */
    if (!is_valid_utf8((const unsigned char *)tool_out, (size_t)n))
        oem_to_utf8(tool_out, BUFSZ);
}

static void tool_read_file(const char *path, long long offset) {
    if (!path_in_workspace(path)) { snprintf(tool_out, BUFSZ, "(拒绝: 路径在工作目录外)"); return; }
    wchar_t wpath[MAX_PATH];
    if (!resolve_in_workspace(path, wpath, MAX_PATH)) { snprintf(tool_out, BUFSZ, "(读取失败: 路径过长)"); return; }
    FILE *f = _wfopen(wpath, L"rb");
    if (!f) { snprintf(tool_out, BUFSZ, "(读取失败: 无法打开 %s)", path); return; }
    /* offset 续读: 大文件不必反复整读, 按 note 里给的 next offset 分块取 */
    if (offset > 0 && _fseeki64(f, offset, SEEK_SET) != 0) {
        fclose(f);
        snprintf(tool_out, BUFSZ, "(读取失败: offset %lld 超出文件范围)", offset);
        return;
    }
    size_t cap = tool_output_cap();
    size_t raw = fread(tool_out, 1, cap, f);
    size_t used = raw;   /* 从文件实际消费的字节数 (含为对齐字符边界跳过的续字节) */
    if (offset > 0 && raw > 0) {
        /* 起点可能切在多字节字符中间: 跳过续字节对齐到字符边界 (UTF-8) */
        size_t skip = utf8_skip_cont((const unsigned char *)tool_out, raw);
        if (skip > 0 && skip < raw) {
            memmove(tool_out, tool_out + skip, raw - skip);
            raw -= skip;
        }
    }
    size_t n = utf8_trim_len(tool_out, raw);   /* 对齐字符边界, 避免切出半个字符 */
    /* 文本不是合法 UTF-8 (常见: 中文 Windows 下的 GBK 文本) -> 转成 UTF-8 */
    if (n > 0 && !is_valid_utf8((const unsigned char *)tool_out, n)) {
        tool_out[n] = '\0';
        oem_to_utf8(tool_out, BUFSZ);
        n = strlen(tool_out);
    }
    /* 文件总大小与是否还有剩余 (fseek 失败则 total 未知; used 含跳过的续字节) */
    int more = 0;
    long long total = -1;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        total = _ftelli64(f);
        more = (total > (long long)offset + (long long)used);
    } else {
        more = (raw == cap);
    }
    fclose(f);
    tool_out[n] = '\0';
    if (raw == 0) {
        snprintf(tool_out, BUFSZ, "(offset %lld 处已无内容; 文件共 %lld 字节)",
                 offset, total > 0 ? total : offset);
        return;
    }
    if (more) {
        char note[320];
        long long next = (long long)offset + (long long)used;
        if (total > 0)
            snprintf(note, sizeof(note),
                     "\n...(已截断: 文件共 %lld 字节, 本次返回第 %lld~%lld 字节; "
                     "用 read_file 的 offset=%lld 继续读取, 或用 execute_bash 的 findstr 定位内容)",
                     total, offset + 1, next, next);
        else
            snprintf(note, sizeof(note),
                     "\n...(已截断: 仅返回前 %zu 字节; 文件较大, 请分段查看)", n);
        strncat(tool_out, note, BUFSZ - strlen(tool_out) - 1);
    }
}

/* 定点替换编辑: old_text 必须在文件中"唯一"出现, 否则不改动文件并说明原因。
 * 大文件改一行无需重写全文; 文件非 UTF-8(如 GBK)时拒绝, 提示改用 write_file。 */
static void tool_edit_file(const char *path, const char *old_text, const char *new_text) {
    if (!path_in_workspace(path)) { snprintf(tool_out, BUFSZ, "(拒绝: 路径在工作目录外)"); return; }
    if (!old_text || !old_text[0]) { strcpy(tool_out, "(编辑失败: old_text 不能为空)"); return; }
    wchar_t wpath[MAX_PATH];
    if (!resolve_in_workspace(path, wpath, MAX_PATH)) { strcpy(tool_out, "(编辑失败: 路径过长)"); return; }

    FILE *f = _wfopen(wpath, L"rb");
    if (!f) { snprintf(tool_out, BUFSZ, "(编辑失败: 无法打开 %s)", path); return; }
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); strcpy(tool_out, "(编辑失败: 无法读取文件大小)"); return; }
    long long fsz = _ftelli64(f);
    if (fsz <= 0) { fclose(f); strcpy(tool_out, "(编辑失败: 文件为空)"); return; }
    if (fsz > EDIT_MAX_BYTES) {
        fclose(f);
        snprintf(tool_out, BUFSZ, "(编辑失败: 文件 %lld 字节超过增量编辑上限 %d 字节; "
                                  "请用 execute_bash 处理或分段修改大文件)", fsz, EDIT_MAX_BYTES);
        return;
    }
    if (_fseeki64(f, 0, SEEK_SET) != 0) { fclose(f); strcpy(tool_out, "(编辑失败: 无法回到文件头)"); return; }
    char *buf = (char*)malloc((size_t)fsz + 1);
    if (!buf) { fclose(f); strcpy(tool_out, "(编辑失败: 内存不足)"); return; }
    size_t got = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    buf[got] = '\0';

    if (!is_valid_utf8((const unsigned char *)buf, got)) {
        free(buf);
        snprintf(tool_out, BUFSZ, "(编辑失败: %s 不是 UTF-8 文本(可能是 GBK), 增量编辑无法定位; "
                                  "如确需改写请用 write_file 全量覆盖)", path);
        return;
    }

    /* 统计出现次数: 必须唯一, 避免改错位置。
     * 注: 按"非重叠"出现计数 (每次匹配后步进 strlen(old_text)),
     * 如 old_text="aa" 在 "aaa" 中记 1 处而非 2 处。 */
    const char *hit = NULL; int count = 0;
    for (const char *p2 = buf; (p2 = strstr(p2, old_text)) != NULL; p2 += strlen(old_text)) {
        if (++count == 1) hit = p2;
        if (count > 1) break;
    }
    if (count == 0) {
        free(buf);
        snprintf(tool_out, BUFSZ, "(未找到匹配文本, 文件未修改; 请先 read_file 确认原文, "
                                  "注意空白与换行需完全一致)");
        return;
    }
    if (count > 1) {
        free(buf);
        snprintf(tool_out, BUFSZ, "(匹配到 %d 处, 文件未修改; 请在 old_text 中带上更多上下文使其唯一)", count);
        return;
    }

    size_t off = (size_t)(hit - buf);
    size_t olen = strlen(old_text), nlen = strlen(new_text ? new_text : "");
    size_t nsz = got - olen + nlen;
    char *nw = (char*)malloc(nsz + 1);
    if (!nw) { free(buf); strcpy(tool_out, "(编辑失败: 内存不足)"); return; }
    memcpy(nw, buf, off);
    memcpy(nw + off, new_text ? new_text : "", nlen);
    memcpy(nw + off + nlen, buf + off + olen, got - off - olen);
    nw[nsz] = '\0';
    free(buf);

    FILE *w = _wfopen(wpath, L"wb");
    if (!w) { free(nw); snprintf(tool_out, BUFSZ, "(编辑失败: 无法写入 %s)", path); return; }
    fwrite(nw, 1, nsz, w);
    fclose(w);
    free(nw);
    touch_file(path);
    snprintf(tool_out, BUFSZ, "(已完成定点替换: %s, %lld 字节 -> %zu 字节)", path, fsz, nsz);
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
    touch_file(path);
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
            if (p) {
                /* offset 可选: 数字或数字字符串都接受 (模型偶发传 "4096" 而非 4096),
                 * 缺省/非法一律从头读 */
                const JValue *ov = json_obj_get(argsj, "offset");
                long long off = 0;
                if (ov && ov->type == J_NUM && ov->num > 0) {
                    off = (long long)ov->num;
                } else if (ov && ov->type == J_STR && ov->str && ov->str[0]) {
                    char *end = NULL;
                    long long v = strtoll(ov->str, &end, 10);
                    if (v > 0 && end && *end == '\0') off = v;
                }
                tool_read_file(p, off);
            }
            else   snprintf(tool_out, BUFSZ, "(参数缺失: %s 需要字符串参数 \"path\")", nm);
        } else if (strcmp(nm, "edit_file") == 0) {
            const char *p  = json_as_str(json_obj_get(argsj, "path"));
            const char *ot = json_as_str(json_obj_get(argsj, "old_text"));
            const char *nt = json_as_str(json_obj_get(argsj, "new_text"));
            if (!p)       snprintf(tool_out, BUFSZ, "(参数缺失: edit_file 需要字符串参数 \"path\")");
            else if (!ot) snprintf(tool_out, BUFSZ, "(参数缺失: edit_file 需要字符串参数 \"old_text\")");
            else if (!nt) snprintf(tool_out, BUFSZ, "(参数缺失: edit_file 需要字符串参数 \"new_text\")");
            else          tool_edit_file(p, ot, nt);
        } else if (strcmp(nm, "write_file") == 0) {
            const char *p  = json_as_str(json_obj_get(argsj, "path"));
            const char *ct = json_as_str(json_obj_get(argsj, "content"));
            if (p && ct)  tool_write_file(p, ct);
            else if (!p)  snprintf(tool_out, BUFSZ, "(参数缺失: write_file 需要字符串参数 \"path\")");
            else          snprintf(tool_out, BUFSZ, "(参数缺失: write_file 需要字符串参数 \"content\")");
        } else {
            snprintf(tool_out, BUFSZ, "(未知工具: %s; 可用工具: execute_bash / read_file / write_file / edit_file)", nm);
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
