/*
 * core/net.h - WinHTTP 与 SSE 流式响应解析
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

static const char *winhttp_err_msg(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:        return "DNS 解析失败, 请检查 Url-Base";
    case ERROR_WINHTTP_CANNOT_CONNECT:           return "无法连接服务器";
    case ERROR_WINHTTP_CONNECTION_ERROR:         return "连接被重置";
    case ERROR_WINHTTP_TIMEOUT:                  return "请求超时";
    case ERROR_WINHTTP_SECURE_INVALID_CERT:      return "SSL 证书无效";
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:   return "证书主机名不匹配";
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID: return "证书已过期或未生效 (请检查系统时间)";
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:     return "SSL/TLS 通道建立失败";
    default: return NULL;
    }
}

static void http_set_err(char *out, size_t cap) {
    DWORD e = GetLastError();
    const char *m = winhttp_err_msg(e);
    if (m) snprintf(out, cap, "[网络错误] %s", m);
    else snprintf(out, cap, "[网络错误] WinHTTP 错误 %lu", e);
}

/* ===== HTTP 重试/退避 (缺口 ③) =====
 * 网络抖动 (DNS/连接重置/超时/限流/5xx) 不应让整轮对话回滚。
 * 仅对"瞬时可恢复"的错误重试; 取消、成功、以及 4xx 客户端错误 (参数/鉴权问题,
 * 重试无益) 一律不重试。退避采用指数增长, 可用环境变量覆盖默认。 */

static int http_max_retries(void) {
    char *e = getenv("CAGENT_HTTP_RETRIES");
    int v = (e && atoi(e) > 0) ? atoi(e) : 3;
    if (v > 8) v = 8;           /* 上限, 避免退避过久 */
    return v;
}

static DWORD http_retry_base_ms(void) {
    char *e = getenv("CAGENT_HTTP_RETRY_MS");
    long v = (e && atol(e) > 0) ? atol(e) : 800;   /* 首次退避 */
    if (v > 30000) v = 30000;
    return (DWORD)v;
}

/* 该次失败是否值得重试: 1=重试, 0=不重试 */
static int http_should_retry(int status) {
    if (status == -1) return 1;                 /* 网络层错误 (DNS/连接/超时) */
    if (status == -2) return 0;                 /* 用户取消: 立即退出 */
    if (status == 200) return 0;                /* 成功 */
    if (status == 429) return 1;                /* 限流 */
    if (status == 408) return 1;                /* 请求超时 */
    if (status >= 500 && status <= 599) return 1; /* 服务端错误 */
    return 0;                                   /* 其余 4xx 客户端错误 */
}

/* 可中断睡眠: 期间若被取消则提前返回, 避免卡在退避里 */
static void cancelable_sleep_ms(DWORD total) {
    DWORD step = 50;
    while (total > 0) {
        if (InterlockedCompareExchange(&g_cancel, 0, 0)) break;
        DWORD d = (total < step) ? total : step;
        Sleep(d);
        total -= d;
    }
}

/* ===== SSE 流式 ===== */

/* 流式累积的 tool_call (按 index 累积 delta 片段) */
typedef struct {
    char id[256];
    char name[64];
    char args[ARGS_MAX];   /* arguments 片段累积 */
} StreamToolCall;

/* 流式上下文: 跨 http_post_stream 传递 */
typedef struct {
    char content_buf[BUFSZ];   /* 累积完整 content 供 messages */
    size_t content_len;
    StreamToolCall calls[8];
    int n_calls;
    char finish[24];           /* stop / length / tool_calls / content_filter 等 */
} StreamCtx;

/* content delta 回调: 增量显示 + 累积到 content_buf。
 * 首个增量跳过前导换行, 避免 (thinking...) 后出现多余空行。 */
static void on_content_delta(void *ud, const char *delta) {
    StreamCtx *ctx = (StreamCtx*)ud;
    if (ctx->content_len == 0) {
        while (*delta == '\r' || *delta == '\n') delta++;
        if (!*delta) return;
    }
    if (cagent_emit) cagent_emit(delta, CAGENT_ROLE_AI);
    size_t dl = strlen(delta);
    if (ctx->content_len + dl < sizeof(ctx->content_buf) - 1) {
        memcpy(ctx->content_buf + ctx->content_len, delta, dl);
        ctx->content_len += dl;
        ctx->content_buf[ctx->content_len] = '\0';
    }
}

/* 单次请求尝试 (无重试): 建立连接、发送、流式读取 SSE、解析 tool_calls。
 * completed 输出参数: 是否完整收到 [DONE] (用于区分"200 但流被截断")。
 * 返回 HTTP 状态码; -2=取消; -1=网络错误 (err_out 写诊断)。 */
static int http_post_stream_once(const char *url, const char *api_key,
                                 const char *body, size_t body_len,
                                 char *err_out, size_t err_cap,
                                 StreamCtx *ctx, int *completed) {
    if (err_cap > 0) err_out[0] = '\0';
    if (completed) *completed = 0;

    const char *p = url;
    int https = 0;
    if (strncmp(p, "https://", 8) == 0) { https = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else { snprintf(err_out, err_cap, "[网络错误] URL 非法"); return -1; }

    const char *host_start = p;
    const char *path_start = strchr(p, '/');
    const char *port_start = strchr(p, ':');
    const char *host_end;
    INTERNET_PORT port = https ? 443 : 80;
    if (port_start && (!path_start || port_start < path_start)) {
        host_end = port_start;
        port = (INTERNET_PORT)atoi(port_start + 1);
    } else if (path_start) {
        host_end = path_start;
    } else {
        host_end = host_start + strlen(host_start);
    }
    char host[256] = {0};
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len >= sizeof(host)) { snprintf(err_out, err_cap, "[网络错误] 主机名过长"); return -1; }
    memcpy(host, host_start, host_len);
    const char *path = path_start ? path_start : "/";

    WCHAR whost[256], wpath[1024];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024);

    HINTERNET hSession = WinHttpOpen(L"cagent/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { http_set_err(err_out, err_cap); return -1; }
    HINTERNET hConnect = WinHttpConnect(hSession, whost, port, 0);
    if (!hConnect) { http_set_err(err_out, err_cap); WinHttpCloseHandle(hSession); return -1; }
    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) { http_set_err(err_out, err_cap); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return -1; }
    if (g_skip_cert_verify && https) {
        DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                  | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
    }

    char hdrs_a[1024];
    snprintf(hdrs_a, sizeof(hdrs_a),
        "Content-Type: application/json\r\nAuthorization: Bearer %s\r\n", api_key);
    WCHAR hdrs[1024];
    MultiByteToWideChar(CP_UTF8, 0, hdrs_a, -1, hdrs, 1024);
    WinHttpAddRequestHeaders(hReq, hdrs, (DWORD)wcslen(hdrs), WINHTTP_ADDREQ_FLAG_ADD);

    DWORD blen = (DWORD)body_len;
    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)body, blen, blen, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, NULL);
    if (!ok) { http_set_err(err_out, err_cap); WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return -1; }

    DWORD st = 0, ss = sizeof(st);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &st, &ss, WINHTTP_NO_HEADER_INDEX);
    int status = (int)st;

    if (status != 200) {
        size_t pos = 0;
        char tmp[8192];
        DWORD nread;
        char prefix[64];
        int plen = snprintf(prefix, sizeof(prefix), "[HTTP %d] ", status);
        if (plen < (int)err_cap) { memcpy(err_out, prefix, plen); pos = plen; }
        while (pos + 1 < err_cap && WinHttpReadData(hReq, tmp, sizeof(tmp), &nread) && nread > 0) {
            size_t copy = nread;
            if (pos + copy >= err_cap) copy = err_cap - 1 - pos;
            memcpy(err_out + pos, tmp, copy);
            pos += copy;
        }
        err_out[pos] = '\0';
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return status;
    }

    /* 流式读取 + SSE 解析 */
    char linebuf[8192];
    size_t lpos = 0;
    char tmp[8192];
    DWORD nread;
    int cancelled = 0;
    while (WinHttpReadData(hReq, tmp, sizeof(tmp), &nread) && nread > 0) {
        if (InterlockedCompareExchange(&g_cancel, 0, 0)) { cancelled = 1; break; }
        for (DWORD i = 0; i < nread; i++) {
            char c = tmp[i];
            if (c == '\n' || lpos >= sizeof(linebuf) - 1) {
                linebuf[lpos] = '\0';
                size_t lbLen = strlen(linebuf);
                while (lbLen > 0 && linebuf[lbLen-1] == '\r') linebuf[--lbLen] = '\0';
                if (strncmp(linebuf, "data: ", 6) == 0) {
                    const char *json = linebuf + 6;
                    if (strcmp(json, "[DONE]") == 0) { if (completed) *completed = 1; lpos = 0; goto stream_done; }
                    JValue *root = json_parse(json);
                    if (root) {
                        const JValue *choices = json_obj_get(root, "choices");
                        /* finish_reason: 中间分片为 null, 末片给出 stop/length/tool_calls/... */
                        const char *frs = json_as_str(json_obj_get(json_arr_at(choices, 0), "finish_reason"));
                        if (frs && *frs) snprintf(ctx->finish, sizeof(ctx->finish), "%s", frs);
                        const JValue *delta = json_obj_get(json_arr_at(choices, 0), "delta");
                        const char *content = json_as_str(json_obj_get(delta, "content"));
                        if (content) on_content_delta(ctx, content);
                        const JValue *tcs = json_obj_get(delta, "tool_calls");
                        if (tcs && tcs->type == J_ARR) {
                            for (size_t j = 0; j < tcs->arr.n; j++) {
                                const JValue *tc = tcs->arr.items[j];
                                int idx = 0;
                                const JValue *idxv = json_obj_get(tc, "index");
                                if (idxv && idxv->type == J_NUM) idx = (int)idxv->num;
                                if (idx < 0 || idx >= 8) continue;
                                if (idx >= ctx->n_calls) ctx->n_calls = idx + 1;
                                StreamToolCall *sc = &ctx->calls[idx];
                                const char *id = json_as_str(json_obj_get(tc, "id"));
                                if (id && *id) snprintf(sc->id, sizeof(sc->id), "%s", id);
                                const JValue *fn = json_obj_get(tc, "function");
                                const char *name = json_as_str(json_obj_get(fn, "name"));
                                if (name && *name) snprintf(sc->name, sizeof(sc->name), "%s", name);
                                const char *args = json_as_str(json_obj_get(fn, "arguments"));
                                if (args) {
                                    size_t al = strlen(sc->args);
                                    snprintf(sc->args + al, sizeof(sc->args) - al, "%s", args);
                                }
                            }
                        }
                        json_free(root);
                    }
                }
                lpos = 0;
            } else {
                linebuf[lpos++] = c;
            }
        }
    }
stream_done:
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    if (cancelled) return -2;
    return status;
}

/* 流式 SSE POST (对外入口): 在单次尝试之上叠加指数退避重试。
 * 仅在瞬时错误时重试; 取消/成功/客户端错误直接返回。
 * 返回 HTTP 状态码; -2=取消; -1=网络错误 (err_out 写诊断)。 */
static int http_post_stream(const char *url, const char *api_key,
                            const char *body, size_t body_len,
                            char *err_out, size_t err_cap,
                            StreamCtx *ctx) {
    int maxr = http_max_retries();
    DWORD base = http_retry_base_ms();
    int last_status = -1;

    for (int attempt = 0; ; attempt++) {
        /* 取消优先: 任何阶段被取消都直接退出, 不重试 */
        if (InterlockedCompareExchange(&g_cancel, 0, 0)) return -2;

        /* 每次尝试前清空上下文与错误, 避免上次部分结果泄漏 */
        memset(ctx, 0, sizeof(*ctx));
        if (err_cap > 0) err_out[0] = '\0';

        int completed = 0;
        int status = http_post_stream_once(url, api_key, body, body_len,
                                          err_out, err_cap, ctx, &completed);
        last_status = status;

        if (status == -2) return -2;                  /* 取消 */
        if (status == 200 && completed) return 200;   /* 成功且流完整 */

        /* 判断是否值得重试: 瞬时错误, 或 200 但流被截断 (需重取) */
        int retryable = http_should_retry(status) || (status == 200 && !completed);
        if (!retryable) break;
        if (attempt >= maxr) break;

        if (cagent_emit) append_text("(网络瞬时错误, 正在重试...)\r\n");
        /* 指数退避: base, 2*base, 4*base ... (累计可在长链路上叠加) */
        DWORD wait = base * (1u << attempt);
        cancelable_sleep_ms(wait);
    }
    return last_status;
}
