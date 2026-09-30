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
    if (dir[0]) snprintf(out, cap, "%s\\%s", dir, filename);
    else        snprintf(out, cap, "%s", filename);
}

static void get_ini_path(char *out, size_t cap)     { get_app_path(out, cap, "cagent.ini"); }

/* 记录最近使用的会话文件路径 (存内存全局, 随 config_save 持久化到 ini)。 */
static void record_last_session(const char *path) {
    snprintf(g_last_session, sizeof(g_last_session), "%s", path);
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

/* 为新会话生成一个不存在的会话文件路径 (工作目录 + 哈希 + 时间戳, 支持同目录多会话) */
static void build_new_session_path(char *out, size_t cap) {
    char base[256];
    ws_to_histname(g_workspace, base, sizeof(base));
    base[strlen(base) - 5] = '\0';             /* 去掉 ".json" */
    for (int n = 0; ; n++) {
        char name[300];
        if (n == 0)
            snprintf(name, sizeof(name), "%s_%lld.json", base, (long long)time(NULL));
        else
            snprintf(name, sizeof(name), "%s_%lld_%d.json", base, (long long)time(NULL), n);
        get_app_path(out, cap, name);
        FILE *test = fopen_utf8(out, "rb");
        if (!test) return;                     /* 不存在 -> 可用 */
        fclose(test);
    }
}

/* 新建会话: 当前对话若有内容先落盘 (旧会话文件保留), 然后重开一个空对话。 */
static void history_start_new(void) {
    if (g_active_ws[0] && strlen(messages) > strlen(SYSTEM_PROMPT))
        history_save();                        /* 已有内容 -> 存到它绑定的文件 */
    reset_conversation();
    g_history_file[0] = '\0';                  /* 下次保存时生成新文件 */
}

/* 把 messages 写入历史文件 (失败静默) */
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
static int load_messages_from_text(const char *text, size_t len) {
    char buf[BUFSZ];
    if (len >= BUFSZ) len = BUFSZ - 1;
    memcpy(buf, text, len); buf[len] = '\0';

    JValue *root = json_parse(buf);
    if (root && root->type == J_OBJ) {
        const JValue *ws = json_obj_get(root, "workspace");
        if (ws && ws->type == J_STR && ws->str && ws->str[0]) {
            snprintf(g_workspace, sizeof(g_workspace), "%s", ws->str);
            snprintf(g_active_ws, sizeof(g_active_ws), "%s", ws->str);
        }
        long s, e;
        if (find_messages_inner(buf, &s, &e) >= 0) {
            long ilen = e - s;
            reset_conversation();
            if (ilen > 0 && strlen(messages) + (size_t)ilen + 1 < BUFSZ) {
                strcat(messages, ",");
                strncat(messages, buf + s, (size_t)ilen);
            }
            json_free(root);
            return 1;
        }
        json_free(root);
    }

    /* 旧版: 纯对话文本 (messages 以系统提示词开头)。
     * 只接受形如对话的内容, 防止损坏文件把垃圾文本混进请求体。 */
    reset_conversation();
    const char *conv = buf;
    if (strncmp(buf, "{\"role\":\"system\"", 16) == 0) {
        size_t end = first_object_end(buf);
        if (end) conv = buf + end;
    }
    if (*conv == ',') conv++;
    if (strncmp(conv, "{\"role\":", 8) == 0 &&
        strlen(messages) + strlen(conv) < BUFSZ - 1) {
        strcat(messages, conv);
        return 1;
    }
    return 0;   /* 无法识别: 视为无有效历史, 从新对话开始 */
}

/* 保存历史: 自描述格式 {"workspace":...,"messages":[...]}, 仅存对话部分。 */
static void history_save(void) {
    /* 懒绑定: 新对话首次保存时生成自己的会话文件 */
    if (g_history_file[0] == '\0')
        build_new_session_path(g_history_file, sizeof(g_history_file));
    const char *path = g_history_file;
    FILE *f = fopen_utf8(path, "wb");
    if (!f) return;
    char *wse = json_escape_alloc(g_workspace);
    if (!wse) { fclose(f); return; }
    size_t skip = strlen(SYSTEM_PROMPT);
    if (strlen(messages) < skip) skip = strlen(messages);  /* 防越界 (messages 未初始化时) */
    const char *conv = messages + skip;        /* ',{...}' 对话 (含前导逗号) */
    /* wse 只含转义内容不带引号, 此处必须自己补上 */
    fprintf(f, "{\"workspace\":\"%s\",\"messages\":[", wse);
    if (conv[0] == ',') conv++;                 /* 数组内部不需要前导逗号 */
    fputs(conv, f);
    fputs("]}", f);
    free(wse);
    fclose(f);
    record_last_session(path);
}

/* 读取单个历史文件并重建 messages + 工作目录。返回 1 成功。 */
static int history_load_from_file(const char *path) {
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return 0;
    char buf[BUFSZ];
    size_t n = fread(buf, 1, BUFSZ - 1, f);
    fclose(f);
    if (n == 0) return 0;
    int ok = load_messages_from_text(buf, n);
    if (ok) {
        snprintf(g_history_file, sizeof(g_history_file), "%s", path); /* 绑定本对话到该文件 */
        record_last_session(path);
    }
    return ok;
}

/* 把当前 messages 里的对话按角色回放到前端 (跳过系统提示词), 格式与实时对话一致。 */
static void history_replay(void) {
    if (!cagent_emit) return;
    size_t skip = strlen(SYSTEM_PROMPT);
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

/* 读取历史文件的元信息: 工作目录 + 首条 user 消息预览, 供 GUI 列表展示。
 * 成功返回 1; ws_out / prev_out 始终以 '\0' 结尾 (无则空字符串)。 */
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
static int session_read_meta(const char *path, char *ws_out, size_t ws_cap,
                             char *prev_out, size_t prev_cap, int *cnt_out) {
    ws_out[0] = prev_out[0] = '\0';
    if (cnt_out) *cnt_out = 0;
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return 0;
    char buf[BUFSZ];
    size_t n = fread(buf, 1, BUFSZ - 1, f);
    fclose(f);
    if (n == 0) return 0;
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
    return 1;
}

