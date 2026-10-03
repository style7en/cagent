/*
 * core/net.h - WinHTTP 与 SSE 流式响应解析
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

static const char *winhttp_err_msg(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:        return "DNS 解析失败, 请检查 Base Url";
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

/* ===== HTTP 重试/退避 =====
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
    long v = (e && atol(e) > 0) ? atol(e) : 2000;  /* 首次退避 (原 800ms 实测太密) */
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

/* 429 限流窗口通常远长于普通网络抖动: 实测 3 次 (约 7 秒) 不够, 会白白回滚整轮。
 * 给 429 单独提到 6 次 (5→10→20→40→60→60s ≈ 3.2 分钟窗口); 其余错误维持默认。
 * 环境变量 CAGENT_HTTP_RETRIES 显式调高时以较大者为准。 */
#define HTTP_RATE_RETRIES 6
/* 429 的退避基数: 与网络抖动 (200ms*2^n) 分开, 限流窗口以几十秒计 */
#define HTTP_RATE_BASE_MS 5000

static int http_max_retries_for(int status) {
    int n = http_max_retries();
    if (status == 429 && n < HTTP_RATE_RETRIES) n = HTTP_RATE_RETRIES;
    return n;
}

/* ===== 错误分类 ===== */

/* ASCII 大小写不敏感子串搜索 (字节级, 对 UTF-8 中文关键字同样安全) */
static int ascii_icontains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < nl && p[k]) {
            char a = p[k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            k++;
        }
        if (k == nl) return 1;
    }
    return 0;
}

/* 是否"额度用完"类错误: 402 欠费 (DeepSeek 等), 403/429 携带配额报文。
 * 刻意用窄关键字列表, 不搜裸 "quota" —— 普通限流的报文里也常带这个词。
 * 额度问题重试无益 (等再久也不会恢复), 必须与限流分开对待。 */
static int http_is_quota_error(int status, const char *resp_body) {
    if (status != 402 && status != 403 && status != 429) return 0;
    if (status == 402) return 1;   /* Payment Required 本身即欠费语义, 无需看报文 */
    if (!resp_body || !resp_body[0]) return 0;
    static const char *kw[] = {
        "insufficient balance", "insufficient_quota", "arrearage",
        "billing hard limit", "plan quota exceeded",
        "\xe4\xbd\x99\xe9\xa2\x9d\xe4\xb8\x8d\xe8\xb6\xb3",             /* 余额不足 */
        "\xe6\xac\xa0\xe8\xb4\xb9",                                     /* 欠费   */
        "\xe9\xa2\x9d\xe5\xba\xa6\xe5\xb7\xb2\xe7\x94\xa8\xe5\xae\x8c", /* 额度已用完 */
        NULL
    };
    for (int i = 0; kw[i]; i++)
        if (ascii_icontains(resp_body, kw[i])) return 1;
    return 0;
}

/* 面向界面的友好错误消息 (命中返回 1 并写 out; 未命中返回 0, 调用方展示原始报文)。
 * 原始报文始终进日志, 界面只给人看的结论。 */
static int http_friendly_error(int status, const char *resp_body, char *out, size_t cap) {
    if (status == 401) {
        snprintf(out, cap, "[鉴权失败] API Key 无效或已过期 (HTTP 401), 请检查 cagent.ini 里的 api_key");
        return 1;
    }
    if (http_is_quota_error(status, resp_body)) {
        snprintf(out, cap, "[额度用完] 账户余额不足或配额耗尽 (HTTP %d), 请充值或更换 API Key 后重试", status);
        return 1;
    }
    if (status == 429) {
        snprintf(out, cap, "[请求过于频繁] 已多轮退避重试仍被限流 (HTTP 429), 请稍等片刻再发");
        return 1;
    }
    return 0;
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
    int  args_overflow;    /* 累积将超上限: 置位并停止拼接, 执行期拒绝执行而非截尾成非法 JSON */
} StreamToolCall;

/* 单轮允许的最大并行 tool_calls (超出部分只记 id/name, 回填"未执行"结果) */
#define STREAM_CALLS_MAX 8

/* content 满时给截断标记预留的尾部空间 */
#define CONTENT_TAIL_RESERVE 128

/* 流式上下文: 跨 http_post_stream 传递 */
typedef struct {
    char content_buf[BUFSZ];   /* 累积完整 content 供 messages */
    size_t content_len;
    StreamToolCall calls[STREAM_CALLS_MAX];
    int n_calls;
    char finish[24];           /* stop / length / tool_calls / content_filter 等 */
    long prompt_tokens;        /* 服务端报告的 prompt_tokens (SSE usage; 不发则 0) */
    /* 诊断留痕: 服务端返回 200 却解析不出 content/tool_calls 时, 日志里必须能看出它到底
     * 发了什么 —— 否则那条回滚完全无从下手 (err_out 在成功路径上是空的, 一点线索都没有)。 */
    char sse_head[512];        /* 第一条 data: 行的原文 (截断到 511 字节) */
    int  n_data_lines;         /* 收到的 data: 行数 (含 [DONE]) */
    int  n_bad_json;           /* 其中 JSON 解析失败的行数 */
    /* 本次 HTTP 分段的状态 (每次尝试前 memset 清零) */
    int emitted;               /* 已向前端上屏过 AI 内容 (重试前据此回删) */
    int truncated;             /* content 超出缓冲上限, 尾部被丢弃 */
    struct { char id[256]; char name[64]; } dropped[STREAM_CALLS_MAX]; /* 超上限被丢弃的调用 */
    int n_dropped;
} StreamCtx;

/* content delta 回调: 增量显示 + 累积到 content_buf。
 * 首个增量跳过前导换行, 避免 (thinking...) 后出现多余空行。
 * 缓冲只收到离尾部 CONTENT_TAIL_RESERVE 字节处, 剩下留给截断标记 —— 否则尾部被静默
 * 丢掉, 模型下一轮看不到自己说过什么; 界面照常显示完整内容。 */
static void on_content_delta(void *ud, const char *delta) {
    StreamCtx *ctx = (StreamCtx*)ud;
    if (ctx->content_len == 0) {
        while (*delta == '\r' || *delta == '\n') delta++;
        if (!*delta) return;
    }
    if (cagent_emit) { cagent_emit(delta, CAGENT_ROLE_AI); ctx->emitted = 1; }
    if (ctx->truncated) return;
    size_t dl = strlen(delta);
    if (ctx->content_len + dl + CONTENT_TAIL_RESERVE > sizeof(ctx->content_buf)) {
        ctx->truncated = 1;
        return;
    }
    memcpy(ctx->content_buf + ctx->content_len, delta, dl);
    ctx->content_len += dl;
    ctx->content_buf[ctx->content_len] = '\0';
}

/* 把一个 delta 里的 tool_calls 按 index 累积进 ctx (流式分片拼接)。
 * 超出 STREAM_CALLS_MAX 的调用不再保存参数, 只记下 id/name —— 必须记下来, 否则这些
 * 调用既不出现在回传的 assistant 消息里、也没有 tool 结果, 模型会以为它们已经执行过。 */
static void stream_apply_tool_calls(StreamCtx *ctx, const JValue *tcs) {
    if (!tcs || tcs->type != J_ARR) return;
    for (size_t j = 0; j < tcs->arr.n; j++) {
        const JValue *tc = tcs->arr.items[j];
        int idx = 0;
        const JValue *idxv = json_obj_get(tc, "index");
        if (idxv && idxv->type == J_NUM) idx = (int)idxv->num;
        if (idx < 0) continue;
        const char *id   = json_as_str(json_obj_get(tc, "id"));
        const JValue *fn = json_obj_get(tc, "function");
        const char *name = json_as_str(json_obj_get(fn, "name"));
        const char *args = json_as_str(json_obj_get(fn, "arguments"));
        if (idx >= STREAM_CALLS_MAX) {
            if (ctx->n_dropped >= STREAM_CALLS_MAX || !id || !id[0]) continue;
            snprintf(ctx->dropped[ctx->n_dropped].id,
                     sizeof(ctx->dropped[ctx->n_dropped].id), "%s", id);
            snprintf(ctx->dropped[ctx->n_dropped].name,
                     sizeof(ctx->dropped[ctx->n_dropped].name), "%s", name ? name : "");
            ctx->n_dropped++;
            continue;
        }
        if (idx >= ctx->n_calls) ctx->n_calls = idx + 1;
        StreamToolCall *sc = &ctx->calls[idx];
        if (id && *id) snprintf(sc->id, sizeof(sc->id), "%s", id);
        if (name && *name) snprintf(sc->name, sizeof(sc->name), "%s", name);
        if (args && !sc->args_overflow) {
            size_t al = strlen(sc->args);
            size_t dl = strlen(args);
            if (al + dl >= sizeof(sc->args)) {
                /* 旧版在这里 snprintf 静默截尾, 产生"arguments 不是合法 JSON";
                 * 现在停止拼接并置标志, 由执行期拒绝执行 + 交回模型明确原因 */
                sc->args_overflow = 1;
                log_line("[tool] arguments 超上限: name=%s 已累积=%zu 本片段=%zu 上限=%zu",
                         sc->name[0] ? sc->name : "?", al, dl, sizeof(sc->args));
            } else {
                memcpy(sc->args + al, args, dl + 1);
            }
        }
    }
}

/* 单次请求尝试 (无重试): 建立连接、发送、流式读取 SSE、解析 tool_calls。
 * completed 输出参数: 是否完整收到 [DONE] (用于区分"200 但流被截断")。
 * retry_after_sec 输出参数: 服务端 Retry-After 头 (秒; 无或非数字为 0)。
 * 返回 HTTP 状态码; -2=取消; -1=网络错误 (err_out 写诊断)。 */
static int http_post_stream_once(const char *url, const char *api_key,
                                 const char *req_body, size_t req_body_len,
                                 char *err_out, size_t err_cap,
                                 StreamCtx *ctx, int *completed,
                                 int *retry_after_sec) {
    if (err_cap > 0) err_out[0] = '\0';
    if (completed) *completed = 0;
    if (retry_after_sec) *retry_after_sec = 0;

    const char *p = url;
    int https = 0;
    if (strncmp(p, "https://", 8) == 0) { https = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else { snprintf(err_out, err_cap, "[网络错误] URL 非法"); return -1; }

    const char *host_start, *host_end;
    INTERNET_PORT port = https ? 443 : 80;
    const char *path_start = strchr(p, '/');
    if (*p == '[') {
        /* IPv6 字面量: http://[::1]/v1 或 http://[::1]:8080/v1
         * 方括号内的 ':' 是地址组成, 不是端口分隔符。传给 WinHttpConnect 时
         * 去掉方括号 (裸 IPv6 字面量可正常解析)。 */
        const char *close = strchr(p, ']');
        if (!close) { snprintf(err_out, err_cap, "[网络错误] URL 非法 (IPv6 方括号未闭合)"); return -1; }
        host_start = p + 1;
        host_end = close;
        if (close[1] == ':') port = (INTERNET_PORT)atoi(close + 2);
    } else {
        const char *port_start = strchr(p, ':');
        host_start = p;
        if (port_start && (!path_start || port_start < path_start)) {
            host_end = port_start;
            port = (INTERNET_PORT)atoi(port_start + 1);
        } else if (path_start) {
            host_end = path_start;
        } else {
            host_end = host_start + strlen(host_start);
        }
    }
    char host[256] = {0};
    size_t host_len = (size_t)(host_end - host_start);
    if (host_len >= sizeof(host)) { snprintf(err_out, err_cap, "[网络错误] 主机名过长"); return -1; }
    memcpy(host, host_start, host_len);
    const char *slash = strchr(host_end, '/');
    const char *path = slash ? slash : "/";

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

    DWORD blen = (DWORD)req_body_len;
    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, (LPVOID)req_body, blen, blen, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, NULL);
    if (!ok) { http_set_err(err_out, err_cap); WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return -1; }

    DWORD st = 0, ss = sizeof(st);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &st, &ss, WINHTTP_NO_HEADER_INDEX);
    int status = (int)st;

    if (status != 200) {
        /* 限流/服务端错误时读服务端告知的等待时长 (Retry-After: 秒数)。
         * HTTP-date 形式的值 atoi 得 0, 自动忽略。 */
        if ((status == 429 || status == 503) && retry_after_sec) {
            WCHAR wra[64], rav[32];
            DWORD ras = sizeof(rav);
            if (utf8_to_wide("Retry-After", wra, 64) &&
                WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CUSTOM, wra, rav, &ras, WINHTTP_NO_HEADER_INDEX)) {
                char ra[32];
                if (WideCharToMultiByte(CP_UTF8, 0, rav, -1, ra, sizeof(ra), NULL, NULL) > 0) {
                    int s = atoi(ra);
                    if (s > 0 && s <= 3600) *retry_after_sec = s;
                }
            }
        }
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

    /* 流式读取 + SSE 解析 (linebuf 按需增长, 避免超长 SSE 行被截断丢弃) */
    size_t lbcap = 8192;
    char *linebuf = (char*)malloc(lbcap);
    int cancelled = 0;
    if (!linebuf) { status = -1; goto stream_done; }
    size_t lpos = 0;
    char tmp[8192];
    DWORD nread;
    while (WinHttpReadData(hReq, tmp, sizeof(tmp), &nread) && nread > 0) {
        if (InterlockedCompareExchange(&g_cancel, 0, 0)) { cancelled = 1; break; }
        for (DWORD i = 0; i < nread; i++) {
            char c = tmp[i];
            if (c == '\n') {
                linebuf[lpos] = '\0';
                size_t lbLen = strlen(linebuf);
                while (lbLen > 0 && linebuf[lbLen-1] == '\r') linebuf[--lbLen] = '\0';
                if (strncmp(linebuf, "data: ", 6) == 0) {
                    const char *json = linebuf + 6;
                    if (ctx->n_data_lines == 0)
                        utf8_safe_copy(json, sizeof(ctx->sse_head) - 1, ctx->sse_head, sizeof(ctx->sse_head));
                    ctx->n_data_lines++;
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
                        stream_apply_tool_calls(ctx, tcs);
                        /* usage (常在末片): 记下 prompt_tokens 供水位估算, 没有则保持 0 */
                        const JValue *usage = json_obj_get(root, "usage");
                        const JValue *pt = usage ? json_obj_get(usage, "prompt_tokens") : NULL;
                        if (pt && pt->type == J_NUM && pt->num > 0)
                            ctx->prompt_tokens = (long)pt->num;
                        json_free(root);
                    } else {
                        ctx->n_bad_json++;      /* 诊断计数: 200 但全是解析不了的 data 行 */
                    }
                }
                lpos = 0;
            } else {
                if (lpos + 1 >= lbcap) {
                    /* 单行过长: 扩容 (设 16MB 上限, 防异常输入撑爆内存) */
                    if (lbcap >= 16u * 1024u * 1024u) { lpos = 0; continue; }
                    size_t ncap = lbcap * 2;
                    char *nb = (char*)realloc(linebuf, ncap);
                    if (!nb) { lpos = 0; continue; }
                    linebuf = nb; lbcap = ncap;
                }
                linebuf[lpos++] = c;
            }
        }
    }
stream_done:
    free(linebuf);
    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    if (cancelled) return -2;
    return status;
}

/* 流式 SSE POST (对外入口): 在单次尝试之上叠加指数退避重试。
 * 仅在瞬时错误时重试; 取消/成功/客户端错误直接返回。
 * 返回 HTTP 状态码; -2=取消; -1=网络错误 (err_out 写诊断)。 */
static int http_post_stream(const char *url, const char *api_key,
                            const char *req_body, size_t req_body_len,
                            char *err_out, size_t err_cap,
                            StreamCtx *ctx) {
    DWORD base = http_retry_base_ms();
    int last_status = -1;

    for (int attempt = 0; ; attempt++) {
        /* 取消优先: 任何阶段被取消都直接退出, 不重试 */
        if (InterlockedCompareExchange(&g_cancel, 0, 0)) return -2;

        /* 每次尝试前清空上下文与错误, 避免上次部分结果泄漏 */
        memset(ctx, 0, sizeof(*ctx));
        if (err_cap > 0) err_out[0] = '\0';

        int completed = 0, retry_after_sec = 0;
        int status = http_post_stream_once(url, api_key, req_body, req_body_len,
                                          err_out, err_cap, ctx, &completed,
                                          &retry_after_sec);
        last_status = status;

        if (status == -2) return -2;                  /* 取消 */
        if (status == 200 && completed) return 200;   /* 成功且流完整 */

        /* 额度用完 (402/403/429+欠费报文): 等再久也不会恢复, 一次都不重试 */
        if (http_is_quota_error(status, err_out)) break;

        /* 判断是否值得重试: 瞬时错误, 或 200 但流被截断 (需重取) */
        int retryable = http_should_retry(status) || (status == 200 && !completed);
        if (!retryable) break;
        int nmax = http_max_retries_for(status);
        if (attempt >= nmax) break;

        /* 本分段已上屏的半截回复必须先回删, 否则重试后界面会拼出两段重复内容 */
        if (ctx->emitted && cagent_stream_undo) cagent_stream_undo();

        /* 退避计算: 网络/服务端错误 base*2^n (上限 60s); 429 用更长的基数
         * (5s*2^n, 上限 60s), 若服务端 Retry-After 更长则以服务端为准 (上限 120s)。
         * 移位前夹紧指数: 重试次数上限 8 保证 1u<<attempt 不溢出, 这里仍显式夹紧,
         * 不依赖别处的夹紧 —— 那个上限一旦被放宽, 移位就是 UB。 */
        int sh = (attempt > 16) ? 16 : attempt;
        DWORD wait = (status == 429 ? HTTP_RATE_BASE_MS : base) * (1u << sh);
        if (wait > 60000) wait = 60000;
        if (status == 429 && retry_after_sec > 0) {
            DWORD ra = (DWORD)retry_after_sec * 1000u;
            if (ra > wait) wait = ra;
            if (wait > 120000) wait = 120000;
        }

        /* 按错误类别给界面不同的提示 (不再一律"网络瞬时错误") */
        if (cagent_emit) {
            if (status == -1)
                append_text("(网络瞬时错误, 等待后重试...)\r\n");
            else if (status == 429)
                append_text("(请求限流, 等待后重试...)\r\n");
            else if (status == 200 && !completed)
                append_text("(响应流中断, 重试...)\r\n");
            else
                append_text("(服务端错误, 等待后重试...)\r\n");
        }
        /* err 按字符边界截取: %.300s 会切半汉字, 日志自己先变成非法 UTF-8 */
        char eprev[304];
        utf8_safe_copy(err_out, 300, eprev, sizeof(eprev));
        log_line("[http] 瞬时错误重试 attempt=%d/%d status=%d wait=%lu ms ra=%d err=%s",
             attempt + 1, nmax, status, (unsigned long)wait, retry_after_sec, eprev);
        cancelable_sleep_ms(wait);
    }
    return last_status;
}
