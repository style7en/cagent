/*
 * core/session.h - 会话持久化: 存取 / 回放 / 多会话命名
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* 会话文件名的通配模式: 本模块的旧会话迁移/目录枚举与界面会话列表 (session_dlg.h,
 * 经聚合头同一 TU 可见) 共用一份 —— 文件名约定一改, 两边必须同时跟着变。 */
#define HISTORY_FILE_GLOB "history_*.json"

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

    snprintf(pat, sizeof(pat), "%s" HISTORY_FILE_GLOB, root);
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

/* 从已解析的会话文件重建思考旁路存档。数组只在"长度恰好 == messages 里 assistant
 * 条数且不超内存上限"时才采用 —— 对不上的一律丢弃, 宁可回放不出思考, 也不让
 * 错位存档混进来; 最后用 reasonings_align_to 把数组补齐到 assistant 条数
 * (旧格式文件 / 对不上的文件在这里补成全 NULL), 后续新对话的归档才不会错位。 */
static void session_load_reasonings(const JValue *root) {
    g_reasoning_n = 0;              /* load_messages_from_text 前面已 reset, 这里兜底 */
    const JValue *msgs = json_obj_get(root, "messages");
    if (!msgs || msgs->type != J_ARR) return;
    int n_ass = 0;
    for (size_t i = 0; i < msgs->arr.n; i++) {
        const char *r = json_as_str(json_obj_get(json_arr_at(msgs, i), "role"));
        if (r && strcmp(r, "assistant") == 0) n_ass++;
    }
    const JValue *rr = json_obj_get(root, "reasonings");
    if (rr && rr->type == J_ARR && rr->arr.n > 0) {
        if (rr->arr.n == (size_t)n_ass && n_ass <= REASONINGS_MAX) {
            for (size_t i = 0; i < rr->arr.n; i++) {
                const JValue *it = json_arr_at(rr, i);
                reasonings_push((it && it->type == J_STR && it->str && it->str[0]) ? it->str : NULL);
            }
        } else {
            /* 对不上 (长度 != assistant 条数) 或超内存上限: 整份丢弃, 宁可回放不出思考
             * 也不让错位存档混进来 —— 但下面的补齐永远要做 */
            log_line("[session] 会话文件思考存档不可用 (%zu 条 / assistant %d 条%s), 丢弃",
                     rr->arr.n, n_ass, n_ass > REASONINGS_MAX ? ", 超内存上限" : "");
        }
    }
    /* **无论存档有没有被采用, 都要补齐到 assistant 条数** (未采用 = 全 NULL 占位):
     * 不然后续新对话第一条 assistant 的归档会挤进旧消息的槽位, 从此错位 */
    reasonings_align_to(n_ass);
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
                emit_err("(加载失败: 会话历史超出上下文缓冲容量 (1MB))\r\n");
                json_free(root);
                return 0;
            }
            session_load_reasonings(root);   /* 思考旁路存档: 载入即对齐 (见函数说明) */
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
        /* 旧格式文件没有思考存档字段, 也必须把数组补到 assistant 条数 (全 NULL 占位)
         * —— 不然后续新对话第一条 assistant 的归档会挤进旧消息的槽位, 从此错位 */
        reasonings_align_to(count_assistant_msgs(messages + strlen(g_system_prompt)));
        return 1;
    }
    return 0;   /* 无法识别: 视为无有效历史, 从新对话开始 */
}

/* ===== 思考旁路存档的落盘 / 载入 =====
 * 数据本体与对齐不变量见 agent.h 的 reasonings[]; 这里只管出入会话文件。
 * 文件格式: {"workspace":..., "messages":[...], "reasonings":[null,"...",...]}
 * reasonings 数组与 messages 里的 **assistant 消息**一一对齐 (与 user/tool 无关)。 */

/* 落盘判定与写入。**文档收口 "]}" 由本函数负责**, 每条出口 (含"决定不写") 都必须
 * 把它写到位 —— 载入方 json_parse 的合法性与这里一一对应。返回 1 成功, 包含两种
 * "决定不写"的情况:
 *   - 对齐失效 (数组长度 != 文件里 assistant 条数): 跳过写数组, 回放安全退化成无思考;
 *   - 没有任何非空条目 (会话自始没产生过思考): 跳过, 省掉一串 null 的大 JSON 数组。
 * 写出中途某条转义失败 (几乎只在 OOM): 就地降级为 null —— 数组长度对齐比条目完整
 * 更重要, 错位会让之后的整份存档永久失效。真正的 I/O 失败返回 0, 上层回整份原子写。 */
static int session_write_reasonings(FILE *f, const char *conv) {
    int n_ass = count_assistant_msgs(conv);
    if (g_reasoning_n != n_ass) {
        if (g_reasoning_n > 0)
            log_line("[session] 思考存档对齐失效 (数组 %d / assistant %d 条), 本次不落盘",
                     g_reasoning_n, n_ass);
        return (fputs("]}", f) >= 0);            /* 只写文档收口 */
    }
    int have = 0;
    for (int i = 0; i < g_reasoning_n; i++)
        if (g_reasonings[i] && g_reasonings[i][0]) { have = 1; break; }
    if (!have) return (fputs("]}", f) >= 0);
    if (fprintf(f, "],\"reasonings\":[") <= 0) return 0;
    for (int i = 0; i < g_reasoning_n; i++) {
        if (i > 0 && fputc(',', f) == EOF) return 0;
        char *esc = g_reasonings[i] ? json_escape_alloc(g_reasonings[i]) : NULL;
        if (!esc) { if (fputs("null", f) == EOF) return 0; continue; }
        int ok = fprintf(f, "\"%s\"", esc) > 0;
        free(esc);
        if (!ok) return 0;
    }
    return (fprintf(f, "]}") > 0);              /* 数组收口 + 文档收口一起完成 */
}

/* 原子写会话文件: 先写 <原名>.tmp, 内容完整落盘后再整体替换目标。
 *
 * 会话是整文件覆盖的, 直接 fopen(path,"wb") 会**先截断旧内容** —— 此时进程被杀、磁盘写满、
 * 或写一半出错, 原历史就没了 (事后补的 .bak 只有一代, 救不回更早的)。
 * 同卷上 MoveFileExW 的替换是原子的: 旧文件在新内容完整落盘之前一个字节都不动。
 * 任一步失败都删掉 .tmp 并保持目标原样, 返回 0 让调用方上报。 */
static int session_write_atomic(const char *path, const char *workspace_esc, const char *conv) {
    /* conv 是"messages 数组内部内容"—— 允许并且**必须**容忍带前导逗号的历史形态
     * (messages 里的对话以 ",{" 开头)。在写入端统一剥掉, 而不是靠每个调用方自觉。 */
    if (*conv == ',') conv++;
    char tmp[MAX_PATH + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    wchar_t wtmp[MAX_PATH + 8], wdst[MAX_PATH];
    if (!utf8_to_wide(tmp, wtmp, MAX_PATH + 8)) return 0;
    if (!utf8_to_wide(path, wdst, MAX_PATH)) return 0;

    FILE *f = fopen_utf8(tmp, "wb");
    if (!f) return 0;
    /* workspace_esc 只含转义内容不带引号, 此处必须自己补上。文档收口 "]}" 由
     * session_write_reasonings 统一负责 (它可能还要在数组后再写 reasonings 字段)。 */
    int ok = (fprintf(f, "{\"workspace\":\"%s\",\"messages\":[", workspace_esc) > 0);
    if (ok) ok = (fputs(conv, f) >= 0);
    if (ok) ok = session_write_reasonings(f, conv);
    if (fclose(f) != 0) ok = 0;                 /* 磁盘满在这时才暴露, 不能只看写入调用 */
    if (!ok) { DeleteFileW(wtmp); return 0; }   /* 目标保持不动 */
    if (!MoveFileExW(wtmp, wdst, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(wtmp); return 0; }
    return 1;
}

/* 保存历史: 自描述格式 {"workspace":...,"messages":[...],"reasonings":[...]} (仅存对话部分;
 * reasonings 是思考旁路存档, 对齐与准入条件见 session_write_reasonings)。 */
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
     * 存的是一份能正常加载的完整历史, 比 .tmp 兜底更有用。
     * (conv 的前导逗号由 session_write_atomic 内部统一剥, 见那里的说明。) */
    {
        wchar_t wsrc[MAX_PATH], wbak[MAX_PATH];
        if (utf8_to_wide(path, wsrc, MAX_PATH) &&
            GetFileAttributesW(wsrc) != INVALID_FILE_ATTRIBUTES) {
            wcscpy(wbak, wsrc); wcscat(wbak, L".bak");
            CopyFileW(wsrc, wbak, FALSE);
        }
    }
    char *wse = json_escape_alloc(g_workspace);
    if (!wse) return;
    int ok = session_write_atomic(path, wse, conv);
    free(wse);
    if (!ok) { log_line("[session] 保存失败, 原文件未动: %s", path); return; }
    log_line("[session] 保存 %s (%zu bytes)", path, strlen(conv));
    snprintf(g_last_session, sizeof(g_last_session), "%s", path);
}

/* 就地净化 messages 里 tool_calls 的非法 arguments。旧版 BUG-1 (截断的 tool_call
 * 原样写回) 曾把 "arguments":"" / 半截 JSON 存进会话文件; 服务端要求历史里
 * tool_calls.arguments 必须是合法 JSON, 坏值会让恢复后的每轮请求都 400, 而
 * 请求失败不回滚 —— 会话永久卡死。与 utf8_sanitize_inplace 同理: 载入即净化。
 * 扫描结构级的 "arguments":" 字面量 (转义文本中的 \" 形式不会被误匹配),
 * 反转义后用 json_parse 校验, 非法者把字面量内容替换为 "{}"。返回修复个数。 */
static CAGENT_MAYBE_UNUSED size_t heal_bad_tool_args(char *msgs) {
    size_t healed = 0;
    char *p = msgs;
    while ((p = strstr(p, "\"arguments\":\"")) != NULL) {
        char *val = p + strlen("\"arguments\":\"");
        char *q = val;                       /* 找字面量收尾: 跳过转义对, 首个裸 " 结束 */
        while (*q && *q != '"') {
            if (*q == '\\' && q[1]) q += 2; else q++;
        }
        if (*q != '"') { p = val; continue; }/* 未闭合: 留给上层 JSON 校验报错 */
        size_t rlen = (size_t)(q - val);     /* 转义形式长度 */
        char *tmp = (char*)malloc(rlen + 1); /* 反转义只会变短, 上限即 rlen */
        if (!tmp) break;
        size_t w = 0;
        for (size_t r = 0; r < rlen; ) {
            if (val[r] == '\\' && r + 1 < rlen) {
                char e = val[r + 1]; r += 2;
                switch (e) {
                case '"':  tmp[w++] = '"';  break;
                case '\\': tmp[w++] = '\\'; break;
                case '/':  tmp[w++] = '/';  break;
                case 'b':  tmp[w++] = '\b'; break;
                case 'f':  tmp[w++] = '\f'; break;
                case 'n':  tmp[w++] = '\n'; break;
                case 'r':  tmp[w++] = '\r'; break;
                case 't':  tmp[w++] = '\t'; break;
                case 'u':                    /* 校验用途无需真转码, 跳过 4 位十六进制 */
                    if (r + 4 <= rlen) r += 4;
                    tmp[w++] = '?';
                    break;
                default:   tmp[w++] = e;    break;
                }
            } else tmp[w++] = val[r++];
        }
        tmp[w] = '\0';
        JValue *aj = json_parse(tmp);
        if (!aj || aj->type != J_OBJ) {
            /* 非法: "..." 内容替换为 "{}"。闭引号 (q 处) 必须随尾部一起搬到 val+2,
             * 搬运源从 q 开始 (含引号); 从 q+1 开始会丢引号, 结构直接损坏。
             * 内容短于 2 字节时替换会让整段变长 (最多 +2), 先确认 messages 还装得下,
             * 否则停在半途, 不越界写。 */
            size_t grow = (rlen < 2) ? (2 - rlen) : 0;
            if (strlen(msgs) + grow + 1 > BUFSZ) { json_free(aj); free(tmp); break; }
            memmove(val + 2, q, strlen(q) + 1);
            memcpy(val, "{}", 2);
            healed++;
            p = val + 2;                     /* 从替换点之后继续扫 */
            log_line("[session] 净化非法 tool_call arguments (原 %zu 字节 -> {})", rlen);
        } else {
            p = q + 1;
        }
        json_free(aj);
        free(tmp);
    }
    return healed;
}

/* 读取单个历史文件并重建 messages + 工作目录。返回 1 成功。 */
static CAGENT_MAYBE_UNUSED int history_load_from_file(const char *path) {
    /* 读全文件到堆缓冲: 会话可能超过 256KB, 栈缓冲截断会导致历史永久丢失 */
    char *buf = read_file_alloc(path, 0, 0, NULL, NULL);
    if (!buf) return 0;
    int ok = load_messages_from_text(buf);
    free(buf);
    if (ok) {
        /* 会话文件可能被旧版本写过非法字节 (转码失败留下的 GBK 原始字节) 或在外部损坏。
         * 载入即净化: 否则这串字节每轮都会被 agent_turn 的预检拦下, 会话永久卡死。 */
        size_t healed = utf8_sanitize_inplace(messages);
        if (healed)
            log_line("[utf8] 载入的会话含非法 UTF-8: 已就地净化 %zu 字节 (%s)", healed, path);
        size_t fixed_args = heal_bad_tool_args(messages);
        if (fixed_args)
            log_line("[session] 载入的会话含 %zu 处非法 tool_call arguments, 已净化 (%s)",
                     fixed_args, path);
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
    size_t a_seq = 0;   /* 已回放的 assistant 计数 —— thinking 存档按它对齐 */
    for (size_t i = 0; i < root->arr.n; i++) {
        const JValue *m = root->arr.items[i];
        if (m->type != J_OBJ) continue;
        const JValue *role = json_obj_get(m, "role");
        const JValue *c = json_obj_get(m, "content");
        const char *r  = (role && role->type == J_STR) ? role->str : "";
        const char *ct = (c && c->type == J_STR) ? c->str : "";
        if (strcmp(r, "user") == 0) {
            if (!*ct) continue;
            char *line = (char*)malloc(strlen(ct) + 16);
            if (!line) break;
            /* 不加"你："前缀 —— 与实时回显一致 (样式已经区分了说话人, 前缀只是噪音)。
             * 前导换行**不能要**: 上一块结尾的换行已经闭合了段落, 再补一个就是带 USER
             * 段前距的空段 (段前距 + 整行行高 + 段后距, 实测约 90px 的空白)。真要分隔
             * 的时候 do_append 的 g_last_no_lf 兜底会补, 两边都不该在这里补。 */
            snprintf(line, strlen(ct) + 16, "%s\r\n", ct);
            cagent_emit(line, CAGENT_ROLE_USER);
            free(line);
        } else if (strcmp(r, "assistant") == 0) {
            /* **思考过程回放** (旁路数组, 见 agent.h 的 reasonings[]): 本条 assistant 有
             * 存档就先发思考块 —— 角色序列与实时路径同构 (THINK_HEAD 标题 + THINK 正文
             * + 必要的收尾换行), 前端按 THINK_HEAD 识别折叠块, 改符号先看两边的契约。
             * 计数 aa 对每条 assistant +1 (无存档也一样), 存档下标才能对上。 */
            size_t aa = a_seq++;
            if (aa < (size_t)g_reasoning_n && g_reasonings[aa] && g_reasonings[aa][0]) {
                cagent_emit(CAGENT_THINK_TITLE "\r\n", CAGENT_ROLE_THINK_HEAD);
                cagent_emit(g_reasonings[aa], CAGENT_ROLE_THINK);
                size_t tln = strlen(g_reasonings[aa]);
                if (g_reasonings[aa][tln-1] != '\n' && g_reasonings[aa][tln-1] != '\r')
                    cagent_emit("\r\n", CAGENT_ROLE_THINK);   /* 与 close_reasoning_block 同一条规则 */
            }
            /* 正文可能为空 (只有 tool_calls 的那一轮), 所以这里**不能**拿空 content 提前跳过 */
            if (*ct) {
                char *line = (char*)malloc(strlen(ct) + 8);
                if (!line) break;
                /* 末尾已有换行则不补 (agent.h 实时路径同一条规则: "避免双倍空行")。
                 * 模型回复常以 "\n" 收尾, 无条件补 "\r\n" 会多出一个空段落 —— 空段也吃
                 * AI 的段前距加整行行高, 回放时表现为正文与工具标题之间一大块空白。 */
                size_t cl = strlen(ct);
                int eol = (cl && (ct[cl-1] == '\n' || ct[cl-1] == '\r'));
                snprintf(line, cl + 8, eol ? "%s" : "%s\r\n", ct);
                cagent_emit(line, CAGENT_ROLE_AI);
                free(line);
            }
            /* **工具调用也要重建** (标题行 + 参数行)。只回放 tool 结果的话, 历史会话里就只剩
             * 一堆没头没尾的输出块: 既看不出是哪次调用, 也没法折叠 —— 折叠块的抓手正是那行
             * 标题。这里发出的角色序列与实时路径完全同构, 前端因此不用为回放写第二套逻辑。 */
            const JValue *tcs = json_obj_get(m, "tool_calls");
            if (tcs && tcs->type == J_ARR) {
                for (size_t k = 0; k < tcs->arr.n; k++) {
                    const JValue *fn = json_obj_get(json_arr_at(tcs, k), "function");
                    if (!fn) continue;
                    const char *nm = json_as_str(json_obj_get(fn, "name"));
                    const char *ag = json_as_str(json_obj_get(fn, "arguments"));
                    if (!nm || !*nm) continue;
                    size_t al  = ag ? strlen(ag) : 0;
                    size_t lim = al > ARGS_DISPLAY_MAX ? ARGS_DISPLAY_MAX : al;
                    size_t cap = strlen(nm) + lim + 64;
                    char *line = (char*)malloc(cap);
                    if (!line) break;
                    /* 与实时路径同构: "▾ 工具名" + 参数行, 符号取自同一份契约 */
                    snprintf(line, cap, CAGENT_FOLD_GLYPH_OPEN " 调用工具 %s\r\n", nm);
                    cagent_emit(line, CAGENT_ROLE_TOOL);
                    if (al) {
                        size_t cut = utf8_trim_len(ag, lim);
                        snprintf(line, cap, "%.*s%s\r\n", (int)cut, ag, (al > lim) ? "…" : "");
                        cagent_emit(line, CAGENT_ROLE_ARGS);
                    }
                    free(line);
                }
            }
        } else if (strcmp(r, "tool") == 0) {
            /* 与实时路径逐字同构 (agent.h): 先剥尾部说明 —— "(已截断: 文件共 N 字节,
             * 用 offset=... 继续读)" 这类话是回传给模型的协议层, 用户关心的是内容本身;
             * 输出为空给 "(无输出)"; 尾换行按 eol 判定 (已带换行则不补)。
             * **不再截到 800 字符**: 回放比实时少看内容才叫不一致, 长输出有折叠块兜底
             * (默认收起只留标题行, 用户手动展开才看全文), 不存在刷屏问题。 */
            const char *note = tool_note_split(ct);
            size_t shown = note ? (size_t)(note - ct) : strlen(ct);
            if (shown == 0) {
                cagent_emit("(无输出)\r\n", CAGENT_ROLE_OUTPUT);
                continue;
            }
            char *line = (char*)malloc(shown + 64);
            if (!line) break;
            int eol = (ct[shown-1] == '\n' || ct[shown-1] == '\r');
            snprintf(line, shown + 64, eol ? "%.*s" : "%.*s\r\n", (int)shown, ct);
            cagent_emit(line, CAGENT_ROLE_OUTPUT);
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
    /* 读全文件到堆: 会话可达 1MB+, 截断读取会让消息条数偏小 */
    char *buf = read_file_alloc(path, 0, 0, NULL, NULL);
    if (!buf) return 0;

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

