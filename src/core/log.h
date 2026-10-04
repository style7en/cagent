/*
 * core/log.h - 运行日志 (exe 同目录 log\ 下)
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 *
 * 目的: 错误可复现。HTTP 失败时请求体整体留档、arguments 解析失败时全文记录、
 * 请求体发送前本地做 UTF-8 预检并指出坏字节的偏移与所属消息。
 *
 * 文件: log\cagent_YYYYMMDD.log (按天追加) + log\request_fail_<时间>_<序号>.json (失败请求留档)。
 * 保留: 超过 LOG_KEEP_DAYS 天的日志与留档, 在进程启动后首次写日志时清理一次。
 * 开关: g_log_enabled, config_load() 进入即置 1, ini 里 log=0 可关;
 *       测试二进制不调 config_load, 保持 0 —— make test 不落盘。
 */

#ifndef CAGENT_LOG_H
#define CAGENT_LOG_H

#define LOG_KEEP_DAYS 7     /* log\ 下文件保留天数; 进程启动后首次写日志时清理一次 */

static int g_log_enabled = 0;
static int g_log_dir_ok = 0;                       /* log\ 目录已创建 */
static int g_log_purged = 0;                       /* 过期日志清理已做 (进程内一次) */
static volatile LONG g_fail_seq = 0;               /* request_fail 文件名序号 (进程内递增) */
static SRWLOCK g_log_lock = SRWLOCK_INIT;          /* 静态初始化, 追加写互斥 */

/* 前向声明: 三者都定义在聚合顺序靠后的模块 (单 TU 静态聚合, 同 exec.h/agent.h 的做法)。 */
static void get_app_path(char *out, size_t cap, const char *filename);  /* 定义在 session.h */
static FILE *fopen_utf8(const char *path, const char *mode);            /* 定义在 workspace.h */
static int utf8_to_wide(const char *u8, wchar_t *out, int cap);         /* 定义在 workspace.h */

/* ---- 过期日志清理 ---- */

/* 删除 dirlog 下匹配 pattern 且最后写入时间早于 cutoff 的文件, 返回删除数。
 * dirlog 形如 "<exe目录>\log\" (已含结尾反斜杠)。全程走宽字符: 非 ASCII 安装路径
 * (中文目录) 下 ANSI 版 API 会静默失败, 与项目其余部分 (utf8_to_wide/fopen_utf8) 不一致。 */
static int log_purge(const char *dirlog, const char *pattern, const FILETIME *cutoff) {
    char pat[PATHSZ];
    if (!path_copy(pat, sizeof(pat), dirlog) ||
        !path_append(pat, sizeof(pat), pattern)) return 0;   /* 拼不上就放弃清理, 不静默用半截 pattern */
    wchar_t wpat[PATHSZ];
    if (!utf8_to_wide(pat, wpat, (int)sizeof(wpat))) return 0;

    ULARGE_INTEGER co;
    co.LowPart = cutoff->dwLowDateTime; co.HighPart = cutoff->dwHighDateTime;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int removed = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER ft;
        ft.LowPart  = fd.ftLastWriteTime.dwLowDateTime;
        ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (ft.QuadPart >= co.QuadPart) continue;          /* 未过期 */

        char name[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL);
        char victim[PATHSZ];
        snprintf(victim, sizeof(victim), "%s%s", dirlog, name);
        wchar_t wvictim[PATHSZ];
        if (utf8_to_wide(victim, wvictim, (int)sizeof(wvictim)) && DeleteFileW(wvictim)) removed++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return removed;
}

/* 进程内一次性清理 log\ 下超过 LOG_KEEP_DAYS 天的日志与失败留档, 返回清理数。
 * 留档只为近期排查, 而日志默认开启且永不轮转, 不清会无限增长。按写入时间判定,
 * 不解析文件名 (文件名日期一旦改格式就不该连带失效)。 */
static int log_housekeep(const char *dir) {
    char dirlog[PATHSZ];
    snprintf(dirlog, sizeof(dirlog), "%slog\\", dir);   /* dir <= 259 + 4 -> 263 < PATHSZ */

    FILETIME nowft;
    GetSystemTimeAsFileTime(&nowft);                       /* 与 FindFirstFile 同为 UTC */
    ULARGE_INTEGER u;
    u.LowPart = nowft.dwLowDateTime; u.HighPart = nowft.dwHighDateTime;
    u.QuadPart -= (ULONGLONG)LOG_KEEP_DAYS * 24ULL * 60ULL * 60ULL * 10000000ULL;
    FILETIME cutoff;
    cutoff.dwLowDateTime = u.LowPart; cutoff.dwHighDateTime = u.HighPart;

    return log_purge(dirlog, "cagent_*.log", &cutoff)
         + log_purge(dirlog, "request_fail_*.json", &cutoff);
}

/* 写一行进 log\cagent_YYYYMMDD.log (自动建目录; 关闭时零开销)。 */
static void log_line(const char *fmt, ...) {
    if (!g_log_enabled) return;

    char dir[MAX_PATH];
    get_app_path(dir, sizeof(dir), "");
    if (!g_log_dir_ok) {
        char d[PATHSZ];
        snprintf(d, sizeof(d), "%slog", dir);
        wchar_t wd[PATHSZ];
        if (utf8_to_wide(d, wd, (int)sizeof(wd)) &&
            (CreateDirectoryW(wd, NULL) || GetLastError() == ERROR_ALREADY_EXISTS))
            g_log_dir_ok = 1;
        else
            return;   /* 目录建不了就不写, 不干扰主流程 */
    }
    /* 首次写日志时清理过期留档; g_log_purged 先置位, 避免递归调用自身时重复清理 */
    if (!g_log_purged) {
        g_log_purged = 1;
        int n = log_housekeep(dir);
        if (n > 0) log_line("[log] 已清理 %d 个超过 %d 天的日志文件", n, LOG_KEEP_DAYS);
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[PATHSZ];
    snprintf(path, sizeof(path), "%slog\\cagent_%04d%02d%02d.log",
             dir, st.wYear, st.wMonth, st.wDay);

    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) { va_end(ap2); return; }

    char prefix[64];
    int pn = snprintf(prefix, sizeof(prefix), "[%02d:%02d:%02d.%03d] ",
                      st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    size_t total = (size_t)pn + (size_t)need + 2;   /* +\r\n */
    char *line = (char*)malloc(total);
    if (!line) { va_end(ap2); return; }
    memcpy(line, prefix, (size_t)pn);
    vsnprintf(line + pn, (size_t)need + 1, fmt, ap2);
    va_end(ap2);
    memcpy(line + pn + need, "\r\n", 2);

    AcquireSRWLockExclusive(&g_log_lock);
    FILE *f = fopen_utf8(path, "ab");
    if (f) { fwrite(line, 1, total, f); fclose(f); }
    ReleaseSRWLockExclusive(&g_log_lock);
    free(line);
}

/* 坏字节落在第几条 messages (0-based, 与服务端 messages[i] 下标对齐)。
 * 旧实现用 memcmp("{\"role\"") 数出现次数, 有两个错: ① 计数从 1 起却声称 0-based,
 * 排查会看错消息; ② 扫全文, message 内容里字面出现的 {"role" 也被计入 (tools 段同理)。
 * 这里改走小扫描器: 定位 "\"messages\":[" 起点后, 只数数组顶层的对象起始 '{',
 * 且跳过 JSON 字符串 (含转义), 因此字符串里的花括号/引号不再干扰。 */
static size_t msg_index_of(const char *buf, size_t len, size_t bad) {
    static const char key[] = "\"messages\":[";
    const size_t klen = sizeof(key) - 1;               /* 不含结尾 NUL */
    size_t ks = (size_t)-1;
    for (size_t p = 0; p + klen <= len; p++)
        if (memcmp(buf + p, key, klen) == 0) { ks = p + klen; break; }
    /* body 由固定模板拼出, 真正的 "messages":[ 必在任何消息内容之前, 故首个匹配即真身。
     * 坏字节在 messages 之前 (如 model 字段) 时无从归属, 记 0。 */
    if (ks == (size_t)-1 || ks > bad) return 0;

    size_t idx = 0;
    int instr = 0, esc = 0, depth = 0;
    for (size_t p = ks; p < bad; p++) {
        char ch = buf[p];
        if (instr) {
            if (esc) esc = 0;
            else if (ch == '\\') esc = 1;
            else if (ch == '"')  instr = 0;
            continue;
        }
        if      (ch == '"') instr = 1;
        else if (ch == '{') { if (depth == 0) idx++; depth++; }   /* 顶层数组元素起始 */
        else if (ch == '}') { if (depth > 0) depth--; }
        else if (ch == ']' && depth == 0) break;                  /* messages 数组结束 */
    }
    return idx > 0 ? idx - 1 : 0;    /* 含坏字节的那条是最后一个已起始的对象 */
}

/* 请求体发送前的 UTF-8 预检: 发现坏字节即记录偏移、所属消息序号与邻近字节 hex。 */
static int log_check_utf8(const char *tag, const char *buf, size_t len) {
    if (!buf) return 0;
    size_t i = 0, bad = (size_t)-1;
    for (; i < len; ) {
        size_t adv = utf8_seq_len(buf, i, len);
        if (!adv) { bad = i; break; }
        i += adv;
    }
    if (bad == (size_t)-1) return 0;

    size_t msg_index = msg_index_of(buf, len, bad);

    char hex[64] = {0};
    size_t from = (bad > 8) ? bad - 8 : 0;
    int hn = 0;
    for (size_t p = from; p < bad + 8 && p < len && hn < (int)sizeof(hex) - 4; p++)
        hn += sprintf(hex + hn, "%02X", (unsigned char)buf[p]);
    log_line("[utf8] %s 含非法UTF-8字节: off=%zu (0x%zX) 消息序号=%zu 邻字节=%s",
         tag, bad, bad, msg_index, hex);
    return 1;
}

/* HTTP 失败时把完整请求体留档到 log\request_fail_<本地时间>_<序号>.json, 便于对服务端直接重放。
 * 文件名必须带时间戳: 旧实现只有进程内序号 (g_fail_seq 每进程从 0), 又用 "wb" 截断写 ——
 * 重启后第一次失败就把 request_fail_1.json 覆盖回上次运行的档案, "可复现"直接落空。
 * 序号再配合存在性探测, 跨进程撞名也能自动让位。 */
static void log_dump_request(const char *payload, size_t len) {
    if (!g_log_enabled || !payload) return;
    log_check_utf8("请求体", payload, len);

    char dir[MAX_PATH];
    get_app_path(dir, sizeof(dir), "");
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[PATHSZ];
    for (int attempt = 0; attempt < 1000; attempt++) {
        snprintf(path, sizeof(path),
                 "%slog\\request_fail_%04d%02d%02d_%02d%02d%02d_%ld.json",
                 dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                 (long)InterlockedIncrement(&g_fail_seq));
        FILE *probe = fopen_utf8(path, "rb");   /* "rb" 不创建文件; 能打开=已存在, 换个序号 */
        if (!probe) break;
        fclose(probe);
    }
    AcquireSRWLockExclusive(&g_log_lock);
    FILE *f = fopen_utf8(path, "wb");
    if (f) { fwrite(payload, 1, len, f); fclose(f); }
    ReleaseSRWLockExclusive(&g_log_lock);
    log_line("[http] 请求体已留档: %s (%zu bytes)", path, len);
}

#endif /* CAGENT_LOG_H */
