/*
 * core/log.h - 运行日志 (exe 同目录 log\ 下)
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 *
 * 目的: 错误可复现。HTTP 失败时请求体整体留档、arguments 解析失败时全文记录、
 * 请求体发送前本地做 UTF-8 预检并指出坏字节的偏移与所属消息。
 *
 * 文件: log\cagent_YYYYMMDD.log (按天追加) + log\request_fail_N.json (失败请求留档)。
 * 开关: g_log_enabled, config_load() 进入即置 1, ini 里 log=0 可关;
 *       测试二进制不调 config_load, 保持 0 —— make test 不落盘。
 */

#ifndef CAGENT_LOG_H
#define CAGENT_LOG_H

static int g_log_enabled = 0;
static int g_log_dir_ok = 0;                       /* log\ 目录已创建 */
static volatile LONG g_fail_seq = 0;               /* request_fail_N.json 序号 */
static SRWLOCK g_log_lock = SRWLOCK_INIT;          /* 静态初始化, 追加写互斥 */

static void get_app_path(char *out, size_t cap, const char *filename);  /* 定义在 session.h */
static FILE *fopen_utf8(const char *path, const char *mode);            /* 定义在 workspace.h */

/* 写一行进 log\cagent_YYYYMMDD.log (自动建目录; 关闭时零开销)。 */
static void log_line(const char *fmt, ...) {
    if (!g_log_enabled) return;

    char dir[MAX_PATH];
    get_app_path(dir, sizeof(dir), "");
    if (!g_log_dir_ok) {
        char d[MAX_PATH];
        snprintf(d, sizeof(d), "%slog", dir);
        if (CreateDirectoryA(d, NULL) || GetLastError() == ERROR_ALREADY_EXISTS)
            g_log_dir_ok = 1;
        else
            return;   /* 目录建不了就不写, 不干扰主流程 */
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    char path[MAX_PATH];
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

/* 请求体发送前的 UTF-8 预检: 发现坏字节即记录偏移、所属消息序号与邻近字节 hex。
 * 消息序号按 {"role" 出现次序计数, 与服务端 messages[i] 的下标对齐 (tools 里无此模式)。 */
static void log_check_utf8(const char *tag, const char *buf, size_t len) {
    if (!g_log_enabled || !buf) return;
    size_t i = 0, bad = (size_t)-1;
    for (; i < len; ) {
        unsigned char c = (unsigned char)buf[i];
        size_t adv = (c < 0x80) ? 1 :
                     ((c & 0xE0) == 0xC0) ? 2 :
                     ((c & 0xF0) == 0xE0) ? 3 :
                     ((c & 0xF8) == 0xF0) ? 4 : 0;
        if (!adv || i + adv > len) { bad = i; break; }
        for (size_t k = 1; k < adv; k++)
            if (((unsigned char)buf[i + k] & 0xC0) != 0x80) { bad = i; break; }
        if (bad != (size_t)-1) break;
        i += adv;
    }
    if (bad == (size_t)-1) return;

    size_t msg_index = 0;
    for (size_t p = 0; p + 7 <= bad && p + 7 <= len; p++)
        if (memcmp(buf + p, "{\"role\"", 7) == 0) msg_index++;

    char hex[64] = {0};
    size_t from = (bad > 8) ? bad - 8 : 0;
    int hn = 0;
    for (size_t p = from; p < bad + 8 && p < len && hn < (int)sizeof(hex) - 4; p++)
        hn += sprintf(hex + hn, "%02X", (unsigned char)buf[p]);
    log_line("[utf8] %s 含非法UTF-8字节: off=%zu (0x%zX) 消息序号=%zu 邻字节=%s",
         tag, bad, bad, msg_index, hex);
}

/* HTTP 失败时把完整请求体留档到 log\request_fail_N.json, 便于对服务端直接重放。 */
static void log_dump_request(const char *body, size_t len) {
    if (!g_log_enabled || !body) return;
    log_check_utf8("请求体", body, len);

    char dir[MAX_PATH];
    get_app_path(dir, sizeof(dir), "");
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%slog\\request_fail_%ld.json",
             dir, InterlockedIncrement(&g_fail_seq));
    AcquireSRWLockExclusive(&g_log_lock);
    FILE *f = fopen_utf8(path, "wb");
    if (f) { fwrite(body, 1, len, f); fclose(f); }
    ReleaseSRWLockExclusive(&g_log_lock);
    log_line("[http] 请求体已留档: %s (%zu bytes)", path, len);
}

#endif /* CAGENT_LOG_H */
