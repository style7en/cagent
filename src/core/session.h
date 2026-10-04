/*
 * core/session.h - 会话持久化: 存取 / 回放 / 多会话命名
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== 配置文件 cagent.ini ===== */

/* 取 exe 同目录下某文件的绝对路径 */
static void get_app_path(char *out, size_t cap, const char *filename) {
    char dir[MAX_PATH];
    get_exe_dir_utf8(dir, sizeof(dir));
    if (!dir[0]) { path_copy(out, cap, filename); return; }
    if (path_copy(out, cap, dir) && path_append(out, cap, "\\"))
        path_append(out, cap, filename);
}

static void get_ini_path(char *out, size_t cap)     { get_app_path(out, cap, "cagent.ini"); }

/* 会话目录: <exe>\sessions\ (与 get_app_path(x, "") 一样以 \ 结尾, 便于直接拼文件名)。
 * 首次使用时创建; 建不出来 (目录被占用/只读) 则退回 exe 目录, 保证对话仍能存盘。 */
static void sessions_dir(char *out, size_t cap) {
    char base[MAX_PATH];
    get_app_path(base, sizeof(base), "");
    char dir[PATHSZ];
    snprintf(dir, sizeof(dir), "%ssessions", base);   /* base <= 259 + 8 -> 267 < PATHSZ */

    int ok = 0;
    wchar_t wdir[PATHSZ];
    if (utf8_to_wide(dir, wdir, (int)sizeof(wdir))) {
        CreateDirectoryW(wdir, NULL);                 /* 已存在会失败, 忽略 */
        DWORD attr = GetFileAttributesW(wdir);
        ok = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
    }
    path_copy(out, cap, ok ? dir : base);

    size_t n = strlen(out);
    if (n > 0 && out[n - 1] != '\\' && n + 1 < cap) { out[n] = '\\'; out[n + 1] = '\0'; }
}

/* 把散落在 exe 目录的旧会话 (history_*.json) 一次性搬进 sessions\。
 * 只移动不改内容; 目标已存在则跳过 (不覆盖); 同步更新 ini 里记录的 last_session。 */
static CAGENT_MAYBE_UNUSED void migrate_legacy_sessions(void) {
    char root[MAX_PATH], dstdir[MAX_PATH], pat[PATHSZ];
    get_app_path(root, sizeof(root), "");
    sessions_dir(dstdir, sizeof(dstdir));
    if (strcmp(root, dstdir) == 0) return;            /* 退回模式: 无处可搬 */

    snprintf(pat, sizeof(pat), "%shistory_*.json", root);
    wchar_t wpat[PATHSZ];
    if (!utf8_to_wide(pat, wpat, (int)sizeof(wpat))) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char name[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL);
        char src[PATHSZ], dst[PATHSZ];
        snprintf(src, sizeof(src), "%s%s", root, name);
        snprintf(dst, sizeof(dst), "%s%s", dstdir, name);
        wchar_t wsrc[PATHSZ], wdst[PATHSZ];
        if (!utf8_to_wide(src, wsrc, (int)sizeof(wsrc)) ||
            !utf8_to_wide(dst, wdst, (int)sizeof(wdst))) continue;
        if (GetFileAttributesW(wdst) != INVALID_FILE_ATTRIBUTES) continue;   /* 已搬过 */
        if (!MoveFileW(wsrc, wdst)) continue;                                  /* 没权限等, 留在原地 */
        if (g_last_session[0] && strcmp(g_last_session, src) == 0)
            path_copy(g_last_session, sizeof(g_last_session), dst);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 清理孤儿备份: 会话主文件被删掉/改名之后, 它的 .bak / .pre_compact 会永远留在
 * sessions\ 里 (会话列表按 history_*.json 枚举, 列不出它们, 所以没人会注意到)。
 * 只删"对应 .json 已不存在"的备份 —— 主文件还在的绝对不动。启动时调一次即可。 */
static CAGENT_MAYBE_UNUSED void cleanup_orphan_session_backups(void) {
    char dir[MAX_PATH];
    sessions_dir(dir, sizeof(dir));                    /* 含结尾 \ */
    char pat[PATHSZ];
    snprintf(pat, sizeof(pat), "%shistory_*", dir);    /* 主文件与两种备份都落在这个前缀下 */
    wchar_t wpat[PATHSZ];
    if (!utf8_to_wide(pat, wpat, (int)sizeof(wpat))) return;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int removed = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char name[MAX_PATH];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL) <= 0)
            continue;

        size_t nl = strlen(name);
        const char *suffix = NULL;
        if (nl > 4  && strcmp(name + nl - 4,  ".bak") == 0)         suffix = ".bak";
        else if (nl > 12 && strcmp(name + nl - 12, ".pre_compact") == 0) suffix = ".pre_compact";
        if (!suffix) continue;                         /* .json 主文件本身, 不动 */

        char base[PATHSZ];
        snprintf(base, sizeof(base), "%s%.*s", dir, (int)(nl - strlen(suffix)), name);
        wchar_t wbase[PATHSZ];
        if (!utf8_to_wide(base, wbase, (int)sizeof(wbase))) continue;
        if (GetFileAttributesW(wbase) != INVALID_FILE_ATTRIBUTES) continue;   /* 主文件还在 */

        char victim[PATHSZ];
        snprintf(victim, sizeof(victim), "%s%s", dir, name);
        wchar_t wv[PATHSZ];
        if (utf8_to_wide(victim, wv, (int)sizeof(wv)) && DeleteFileW(wv)) removed++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (removed) log_line("[session] 已清理 %d 个孤儿备份 (对应的会话文件已不存在)", removed);
}

/* djb2 字符串哈希, 用于工作目录的稳定摘要 (防文件名超长/冲突) */
static unsigned int djb2_hash(const char *s) {
    unsigned int h = 5381; int c;
    while ((c = (unsigned char)*s++)) h = ((h << 5) + h) + (unsigned int)c;
    return h;
}

/* 把工作目录编码为安全的历史文件名: 替换非法字符 + 附 8 位哈希后缀。
   不同工作目录 => 不同文件名; 同名前导便于人读, 哈希保证唯一。 */
static void ws_to_histname(const char *ws, char *out, size_t cap) {
    char san[128];
    size_t j = 0;
    for (size_t i = 0; ws[i] && j + 1 < sizeof(san); i++) {
        char c = ws[i];
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' ||
            c == '"'  || c == '<' || c == '>' || c == '|' || c == ' ') {
            if (j == 0 || san[j-1] != '_') san[j++] = '_';
        } else {
            san[j++] = c;
        }
    }
    if (j == 0) { san[0]='d'; san[1]='e'; san[2]='f'; san[3]='\0'; }
    else san[j] = '\0';
    snprintf(out, cap, "history_%s_%08X.json", san, djb2_hash(ws));
}

/* 为新会话生成一个不存在的会话文件路径 (sessions 目录 + 工作目录哈希 + 时间戳, 支持多会话) */
static void build_new_session_path(char *out, size_t cap) {
    char base[256];
    ws_to_histname(g_workspace, base, sizeof(base));
    base[strlen(base) - 5] = '\0';             /* 去掉 ".json" */
    char dir[MAX_PATH];
    sessions_dir(dir, sizeof(dir));
    for (int n = 0; ; n++) {
        char name[300];
        if (n == 0)
            snprintf(name, sizeof(name), "%s_%lld.json", base, (long long)time(NULL));
        else
            snprintf(name, sizeof(name), "%s_%lld_%d.json", base, (long long)time(NULL), n);
        /* out 的 cap 由调用方决定, 用拼接助手: 放不下时 out 置空, 下面 fopen 失败即退出 */
        path_copy(out, cap, dir);
        path_append(out, cap, name);
        FILE *test = fopen_utf8(out, "rb");
        if (!test) return;                     /* 不存在 -> 可用 */
        fclose(test);
    }
}

/* 新建会话: 当前对话若有内容先落盘 (旧会话文件保留), 然后重开一个空对话。 */
static CAGENT_MAYBE_UNUSED void history_start_new(void) {
    if (g_active_ws[0] && strlen(messages) > strlen(g_system_prompt))
        history_save();                        /* 已有内容 -> 存到它绑定的文件 */
    reset_conversation();
    g_history_file[0] = '\0';                  /* 下次保存时生成新文件 */
}

/* 返回首个顶层 JSON 对象的结束下标 (指向其闭合 '}' 之后)。
 * 正确处理字符串内的括号与转义。找不到返回 0。 */
static size_t first_object_end(const char *s) {
    size_t i = 0;
    while (s[i] && s[i] != '{') i++;          /* 跳过前导空白/逗号 */
    if (!s[i]) return 0;
    int depth = 0, in_str = 0;
    for (; s[i]; i++) {
        char c = s[i];
        if (in_str) {
            if (c == '\\') { i++; continue; }
            if (c == '"') in_str = 0;
            continue;
        }
        if (c == '"') { in_str = 1; continue; }
        if (c == '{') depth++;
        else if (c == '}') { if (--depth == 0) return i + 1; }
    }
    return 0;
}

/* 在 text 中定位 "messages":[ ... ] 数组内部内容区间 (字符串/嵌套感知)。
 * 输出内部起点 (首 '[' 之后) 与逻辑终点 (末 ']' 之前) 下标; 找不到返回 -1。 */
static long find_messages_inner(const char *t, long *out_start, long *out_end) {
    const char *p = strstr(t, "\"messages\"");
    if (!p) return -1;
    p += 10; /* "messages" 含引号共 10 字符 */
    while (*p && *p != ':') p++;
    if (!*p) return -1;
    p++;
    while (*p && *p != '[') p++;
    if (!*p) return -1;
    *out_start = (long)(p - t) + 1;            /* 跳过 '[' */
    int depth = 0, in_str = 0; const char *q = p;
    for (; *q; q++) {
        char c = *q;
        if (in_str) { if (c == '\\') { q++; continue; } if (c == '"') in_str = 0; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '[') depth++;
        else if (c == ']') { if (--depth == 0) { *out_end = (long)(q - t); return 0; } }
    }
    return -1;
}

/* 把文件文本重建进 messages: 始终以最新系统提示词开场, 再追加对话。
 * 支持两种格式: 自描述 {"workspace":...,"messages":[...]} 与旧版纯对话。
 * 返回 1 成功; 若含 workspace 则同步写回 g_workspace / g_active_ws。 */
static int load_messages_from_text(const char *text) {
    /* 解析调用方读入堆缓冲的完整文本, 不截断; messages 仍受 BUFSZ 容量上限约束。 */
    JValue *root = json_parse(text);
    if (root && root->type == J_OBJ) {
        const JValue *ws = json_obj_get(root, "workspace");
        if (ws && ws->type == J_STR && ws->str && ws->str[0]) {
            snprintf(g_workspace, sizeof(g_workspace), "%s", ws->str);
            snprintf(g_active_ws, sizeof(g_active_ws), "%s", ws->str);
        }
        long s, e;
        if (find_messages_inner(text, &s, &e) >= 0) {
            long ilen = e - s;
            reset_conversation();
            if (ilen > 0 && strlen(messages) + (size_t)ilen + 1 < BUFSZ) {
                strcat(messages, ",");
                strncat(messages, text + s, (size_t)ilen);
            } else if (ilen > 0) {
                /* 装不下就明确失败并说明原因, 不再静默丢弃整段历史 */
                append_text("(加载失败: 会话历史超出上下文缓冲容量 (1MB))\r\n");
                json_free(root);
                return 0;
            }
            json_free(root);
            return 1;
        }
        json_free(root);
    }

    /* 旧版: 纯对话文本 (messages 以系统提示词开头)。
     * 只接受形如对话的内容, 防止损坏文件把垃圾文本混进请求体。 */
    reset_conversation();
    const char *conv = text;
    if (strncmp(text, "{\"role\":\"system\"", 16) == 0) {
        size_t end = first_object_end(text);
        if (end) conv = text + end;
    }
    if (*conv == ',') conv++;
    if (strncmp(conv, "{\"role\":", 8) == 0 &&
        strlen(messages) + strlen(conv) < BUFSZ - 1) {
        strcat(messages, conv);
        return 1;
    }
    return 0;   /* 无法识别: 视为无有效历史, 从新对话开始 */
}

/* 原子写会话文件: 先写 <原名>.tmp, 内容完整落盘后再整体替换目标。
 *
 * 会话是整文件覆盖的, 直接 fopen(path,"wb") 会**先截断旧内容** —— 此时进程被杀、磁盘写满、
 * 或写一半出错, 原历史就没了 (事后补的 .bak 只有一代, 救不回更早的)。
 * 同卷上 MoveFileExW 的替换是原子的: 旧文件在新内容完整落盘之前一个字节都不动。
 * 任一步失败都删掉 .tmp 并保持目标原样, 返回 0 让调用方上报。 */
static int session_write_atomic(const char *path, const char *workspace_esc, const char *conv) {
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    wchar_t wtmp[MAX_PATH + 8], wdst[MAX_PATH];
    if (!utf8_to_wide(tmp, wtmp, MAX_PATH + 8)) return 0;
    if (!utf8_to_wide(path, wdst, MAX_PATH)) return 0;

    FILE *f = fopen_utf8(tmp, "wb");
    if (!f) return 0;
    /* workspace_esc 只含转义内容不带引号, 此处必须自己补上 */
    int ok = (fprintf(f, "{\"workspace\":\"%s\",\"messages\":[", workspace_esc) > 0);
    if (ok) ok = (fputs(conv, f) >= 0);
    if (ok) ok = (fputs("]}", f) >= 0);
    if (fclose(f) != 0) ok = 0;                 /* 磁盘满在这时才暴露, 不能只看写入调用 */
    if (!ok) { DeleteFileW(wtmp); return 0; }   /* 目标保持不动 */
    if (!MoveFileExW(wtmp, wdst, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(wtmp); return 0; }
    return 1;
}

/* 保存历史: 自描述格式 {"workspace":...,"messages":[...]}, 仅存对话部分。 */
static void history_save(void) {
    size_t skip = strlen(g_system_prompt);
    if (strlen(messages) < skip) skip = strlen(messages);  /* 防越界 (messages 未初始化时) */
    const char *conv = messages + skip;        /* ',{...}' 对话 (含前导逗号) */
    /* 空对话且尚未绑定文件: 不落盘 (压缩失败重置等场景, 避免产生空的会话文件) */
    if (g_history_file[0] == '\0' &&
        (conv[0] == '\0' || (conv[0] == ',' && conv[1] == '\0'))) return;
    /* 懒绑定: 新对话首次保存时生成自己的会话文件 */
    if (g_history_file[0] == '\0')
        build_new_session_path(g_history_file, sizeof(g_history_file));
    const char *path = g_history_file;
    /* 覆盖前备份上次内容。有了原子写, 它不再是"防写坏"的补丁, 而是"回退到上一轮"的入口:
     * 存的是一份能正常加载的完整历史, 比 .tmp 兜底更有用。 */
    {
        wchar_t wsrc[MAX_PATH], wbak[MAX_PATH];
        if (utf8_to_wide(path, wsrc, MAX_PATH) &&
            GetFileAttributesW(wsrc) != INVALID_FILE_ATTRIBUTES) {
            wcscpy(wbak, wsrc); wcscat(wbak, L".bak");
            CopyFileW(wsrc, wbak, FALSE);
        }
    }
    if (conv[0] == ',') conv++;                 /* 数组内部不需要前导逗号 */
    char *wse = json_escape_alloc(g_workspace);
    if (!wse) return;
    int ok = session_write_atomic(path, wse, conv);
    free(wse);
    if (!ok) { log_line("[session] 保存失败, 原文件未动: %s", path); return; }
    log_line("[session] 保存 %s (%zu bytes)", path, strlen(conv));
    snprintf(g_last_session, sizeof(g_last_session), "%s", path);
}

/* 读取单个历史文件并重建 messages + 工作目录。返回 1 成功。 */
static CAGENT_MAYBE_UNUSED int history_load_from_file(const char *path) {
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return 0;
    /* 读全文件到堆缓冲: 会话可能超过 256KB, 栈缓冲截断会导致历史永久丢失 */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long fsz = ftell(f);
    if (fsz < 0) { fclose(f); return 0; }
    char *buf = (char*)malloc((size_t)fsz + 1);
    if (!buf) { fclose(f); return 0; }
    rewind(f);
    size_t n = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    if (n == 0) { free(buf); return 0; }
    buf[n] = '\0';
    int ok = load_messages_from_text(buf);
    free(buf);
    if (ok) {
        /* 会话文件可能被旧版本写过非法字节 (转码失败留下的 GBK 原始字节) 或在外部损坏。
         * 载入即净化: 否则这串字节每轮都会被 agent_turn 的预检拦下, 会话永久卡死。 */
        size_t healed = utf8_sanitize_inplace(messages);
        if (healed)
            log_line("[utf8] 载入的会话含非法 UTF-8: 已就地净化 %zu 字节 (%s)", healed, path);
        snprintf(g_history_file, sizeof(g_history_file), "%s", path); /* 绑定本对话到该文件 */
        snprintf(g_last_session, sizeof(g_last_session), "%s", path);
    }
    return ok;
}

/* 把当前 messages 里的对话按角色回放到前端 (跳过系统提示词), 格式与实时对话一致。 */
static CAGENT_MAYBE_UNUSED void history_replay(void) {
    if (!cagent_emit) return;
    size_t skip = strlen(g_system_prompt);
    const char *conv = messages + skip;
    if (*conv == ',') conv++;
    if (!*conv) return;
    char *arr = (char*)malloc(strlen(conv) + 3);
    if (!arr) return;
    sprintf(arr, "[%s]", conv);
    JValue *root = json_parse(arr);
    free(arr);
    if (!root || root->type != J_ARR) { if (root) json_free(root); return; }
    for (size_t i = 0; i < root->arr.n; i++) {
        const JValue *m = root->arr.items[i];
        if (m->type != J_OBJ) continue;
        const JValue *role = json_obj_get(m, "role");
        const JValue *c = json_obj_get(m, "content");
        const char *r  = (role && role->type == J_STR) ? role->str : "";
        const char *ct = (c && c->type == J_STR) ? c->str : "";
        if (!*ct) continue;
        if (strcmp(r, "user") == 0) {
            char *line = (char*)malloc(strlen(ct) + 16);
            if (!line) break;
            snprintf(line, strlen(ct) + 16, "\r\n你：%s\r\n", ct);
            cagent_emit(line, CAGENT_ROLE_USER);
            free(line);
        } else if (strcmp(r, "assistant") == 0) {
            char *line = (char*)malloc(strlen(ct) + 8);
            if (!line) break;
            snprintf(line, strlen(ct) + 8, "%s\r\n", ct);
            cagent_emit(line, CAGENT_ROLE_AI);
            free(line);
        } else if (strcmp(r, "tool") == 0) {
            size_t tl = strlen(ct);
            size_t cut = utf8_trim_len(ct, tl > 800 ? 800 : tl);
            int trunc = (tl > 800);
            char *line = (char*)malloc(cut + 48);
            if (!line) break;
            snprintf(line, cut + 48, "[Output]\r\n%.*s%s\r\n",
                     (int)cut, ct, trunc ? "\r\n(已截断)" : "");
            cagent_emit(line, CAGENT_ROLE_SYS);
            free(line);
        }
    }
    json_free(root);
}

/* 旧格式文件提取首条 user 消息预览 (纯文本扫描, 不建 JSON 树)。 */
static void extract_old_preview(const char *text, char *out, size_t cap) {
    out[0] = '\0';
    const char *p = text;
    if (p[0] == ',') p++;
    if (strncmp(p, "{\"role\":\"system\"", 16) == 0) {
        size_t end = first_object_end(p);
        if (end) { p += end; if (p[0] == ',') p++; }
    }
    const char *u = strstr(p, "\"role\":\"user\"");
    if (!u) u = p;
    const char *c = strstr(u, "\"content\":\"");
    if (!c) return;
    c += strlen("\"content\":\"");
    size_t i = 0;
    while (*c && i + 1 < cap) {
        if (*c == '\\' && c[1]) {
            char nx = c[1];
            out[i++] = (nx == 'n' || nx == 't' || nx == 'r') ? ' ' : nx;
            c += 2;
            continue;
        }
        if (*c == '"') break;
        out[i++] = *c++;
    }
    out[i] = '\0';
}

/* 读取历史文件的元信息: 工作目录 + 首条 user 消息预览 + 消息条数, 供 GUI 列表展示。
 * 成功返回 1; ws_out / prev_out 始终以 '\0' 结尾 (无则空字符串, 旧格式自动回退解析)。 */
static CAGENT_MAYBE_UNUSED int session_read_meta(const char *path, char *ws_out, size_t ws_cap,
                             char *prev_out, size_t prev_cap, int *cnt_out) {
    ws_out[0] = prev_out[0] = '\0';
    if (cnt_out) *cnt_out = 0;
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return 0;
    /* 读全文件到堆: 会话可达 1MB+, 截断读取会让消息条数偏小 */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long fsz = ftell(f);
    if (fsz <= 0) { fclose(f); return 0; }
    rewind(f);
    char *buf = (char*)malloc((size_t)fsz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t n = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    if (n == 0) { free(buf); return 0; }
    buf[n] = '\0';

    JValue *root = json_parse(buf);
    if (root && root->type == J_OBJ) {
        const JValue *ws = json_obj_get(root, "workspace");
        if (ws && ws->type == J_STR && ws->str)
            snprintf(ws_out, ws_cap, "%s", ws->str);
        const JValue *msgs = json_obj_get(root, "messages");
        if (msgs && msgs->type == J_ARR) {
            if (cnt_out) *cnt_out = (int)msgs->arr.n;
            for (size_t i = 0; i < msgs->arr.n && prev_out[0] == '\0'; i++) {
                const JValue *m = msgs->arr.items[i];
                if (m->type != J_OBJ) continue;
                const JValue *role = json_obj_get(m, "role");
                const char *r = (role && role->type == J_STR) ? role->str : "";
                if (strcmp(r, "user") != 0) continue;
                const JValue *c = json_obj_get(m, "content");
                const char *cs = (c && c->type == J_STR) ? c->str : "";
                if (cs) { strncpy(prev_out, cs, prev_cap - 1); prev_out[prev_cap - 1] = '\0'; }
            }
        }
        json_free(root);
        free(buf);
        return 1;
    }
    /* 旧格式 (纯对话文本): 逐字扫描统计 + 提取预览 */
    const char *p = buf;
    if (p[0] == ',') p++;
    if (strncmp(p, "{\"role\":\"system\"", 16) == 0) {
        size_t end = first_object_end(p);
        if (end) { p += end; if (p[0] == ',') p++; }
    }
    if (cnt_out)
        for (const char *q = p; (q = strstr(q, "\"role\":")) != NULL; q += 7)
            (*cnt_out)++;
    extract_old_preview(p, prev_out, prev_cap);
    free(buf);
    return 1;
}

