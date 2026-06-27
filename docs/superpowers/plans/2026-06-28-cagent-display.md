# cagent 显示重构 实现计划(阶段二 - 子项目 3)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 历史框换 RichEdit 2.0,LLM 响应改 SSE 流式增量显示,content 结束后渲染 Markdown 基础集。

**Architecture:** 仍单文件。RichEdit(`riched20`)替换 EDIT 控件。新增 `http_post_stream` 流式读取 SSE,`content` delta 实时 `append_text`,`tool_calls` delta 按 index 累积。`md_render` 在 content 结束后把纯文本段替换为格式化文本。手动测试验证。

**Tech Stack:** C(MinGW-w64)、Win32、RichEdit 2.0、WinHTTP、DPAPI、单文件。

**对应 spec:** `docs/superpowers/specs/2026-06-28-cagent-display-design.md`

---

## 文件结构

- **Modify** `cagent_gui.c` —— RichEdit 控件、`http_post_stream`、`StreamCtx`、`md_render`、agent_thread 重构
- **Modify** `Makefile` —— `GUI_LDFLAGS` 追加 `-lriched20`
- **Modify** `README.md` —— 技术特性/操作补充

---

### Task 1: RichEdit 替换历史框

**Files:**
- Modify: `Makefile`、`cagent_gui.c`(include、WinMain、WM_CREATE 历史框创建)

- [ ] **Step 1: Makefile 加 -lriched20**

定位:
```makefile
GUI_LDFLAGS = -mwindows -lcomctl32 -lwinhttp -lcrypt32
```
改为:
```makefile
GUI_LDFLAGS = -mwindows -lcomctl32 -lwinhttp -lcrypt32 -lriched20
```

- [ ] **Step 2: 加 #include <richedit.h>**

定位 include 区(`#include <wincrypt.h>` 之后):
```c
#include <wincrypt.h>
#include <stdio.h>
```
改为:
```c
#include <wincrypt.h>
#include <richedit.h>
#include <stdio.h>
```

- [ ] **Step 3: WinMain 加 LoadLibrary riched20**

定位 `WinMain` 中 `enable_dpi_awareness();` 之前(`--selftest` 检查之后):
```c
    enable_dpi_awareness();   /* 必须在创建任何窗口之前调用 */
    InitCommonControls();
```
改为:
```c
    LoadLibraryW(L"riched20.dll");   /* 注册 RichEdit 控件类 */
    enable_dpi_awareness();   /* 必须在创建任何窗口之前调用 */
    InitCommonControls();
```

- [ ] **Step 4: 历史框控件 EDIT → RichEdit**

定位 `WM_CREATE` 中历史框创建:
```c
        g_hHistory = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_HISTORY, NULL, NULL);
        SendMessage(g_hHistory, WM_SETFONT, (WPARAM)g_hFont, TRUE);
```
改为:
```c
        g_hHistory = CreateWindowExW(WS_EX_CLIENTEDGE, RICHEDIT_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_HISTORY, NULL, NULL);
        /* RichEdit 默认字符格式 (16pt Microsoft YaHei UI) */
        {
            CHARFORMAT2W cf;
            memset(&cf, 0, sizeof(cf));
            cf.cbSize = sizeof(cf);
            cf.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET;
            cf.yHeight = 320;   /* 16pt × 20 */
            cf.bCharSet = DEFAULT_CHARSET;
            wcscpy(cf.szFaceName, L"Microsoft YaHei UI");
            SendMessage(g_hHistory, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
        }
```

注意:RichEdit 不用 `WM_SETFONT`(用 `CHARFORMAT2`)。`SendMessage(g_hHistory, WM_SETFONT, ...)` 行删除,由 `CHARFORMAT2` 替代。

- [ ] **Step 5: 编译 + selftest 回归**

Run: `make && ./cagent_gui.exe --selftest; echo exit=$?`
Expected: 零警告;selftest OK。

- [ ] **Step 6: 手动验证历史框显示正常**

启动 `./cagent_gui.exe`(填可用配置),发一条消息:历史框应正常显示文本(`>>>`、`(thinking...)`、回复),滚动/选中/复制正常。若 Edit→RichEdit 后 `do_append` 的 `EM_SETSEL`/`EM_REPLACESEL` 兼容(RichEdit 支持),应无需改 `do_append`。

- [ ] **Step 7: 提交**

```bash
git add Makefile cagent_gui.c
git commit -m "feat: 历史框换 RichEdit 2.0 (riched20) + 默认字符格式"
```

---

### Task 2: 流式 SSE(http_post_stream + agent_thread 重构)

**Files:**
- Modify: `cagent_gui.c`(新增 `StreamCtx`/`http_post_stream`、`body` 加 `stream:true`、agent_thread for 循环重构)

- [ ] **Step 1: 加 StreamCtx 结构与 on_content_delta 回调**

在 `http_post` 函数之前(或 `call_llm` 之前)插入:

```c
/* ===== SSE 流式 ===== */

/* 流式累积的 tool_call (按 index 累积 delta 片段) */
typedef struct {
    char id[256];
    char name[64];
    char args[8192];   /* arguments 片段累积 */
} StreamToolCall;

/* 流式上下文: 跨 http_post_stream 传递 */
typedef struct {
    char content_buf[BUFSZ];   /* 累积完整 content 供 md_render */
    size_t content_len;
    LONG content_start;        /* 流式开始时历史框长度 (供 Markdown 替换定位) */
    StreamToolCall calls[8];
    int n_calls;
} StreamCtx;

/* content delta 回调: 增量显示 + 累积到 content_buf */
static void on_content_delta(void *ud, const char *delta) {
    StreamCtx *ctx = (StreamCtx*)ud;
    append_text(delta);
    size_t dl = strlen(delta);
    if (ctx->content_len + dl < sizeof(ctx->content_buf) - 1) {
        memcpy(ctx->content_buf + ctx->content_len, delta, dl);
        ctx->content_len += dl;
        ctx->content_buf[ctx->content_len] = '\0';
    }
}
```

- [ ] **Step 2: 加 http_post_stream 函数**

在 `http_post` 之后插入(复用 URL 解析逻辑;为简洁,独立实现完整流程):

```c
/* SSE 流式 POST: 增量读取, content delta 回调, tool_calls delta 累积到 ctx。
 * 返回 HTTP 状态码; -2=取消; -1=网络错误 (err_out 写诊断). */
static int http_post_stream(const char *url, const char *api_key,
                            const char *body, size_t body_len,
                            char *err_out, size_t err_cap,
                            StreamCtx *ctx) {
    if (err_cap > 0) err_out[0] = '\0';

    /* URL 解析 (同 http_post) */
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

    HINTERNET hSession = WinHttpOpen(L"cagent-gui/1.0",
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
        /* 非 200: 读完整响应体到 err_out 供诊断 */
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
                /* 去行尾 \r */
                size_t lbLen = strlen(linebuf);
                while (lbLen > 0 && linebuf[lbLen-1] == '\r') linebuf[--lbLen] = '\0';
                if (strncmp(linebuf, "data: ", 6) == 0) {
                    const char *json = linebuf + 6;
                    if (strcmp(json, "[DONE]") == 0) { lpos = 0; goto stream_done; }
                    JValue *root = json_parse(json);
                    if (root) {
                        const JValue *choices = json_obj_get(root, "choices");
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
                                if (id) snprintf(sc->id, sizeof(sc->id), "%s", id);
                                const JValue *fn = json_obj_get(tc, "function");
                                const char *name = json_as_str(json_obj_get(fn, "name"));
                                if (name) snprintf(sc->name, sizeof(sc->name), "%s", name);
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
```

- [ ] **Step 3: body 加 stream:true**

定位 `agent_thread` 中 `body` 组装:
```c
        snprintf(body, BUFSZ,
            "{\"model\":\"%s\",\"messages\":[%s],\"tools\":%s}",
            g_model, messages, TOOLS_JSON);
```
改为:
```c
        snprintf(body, BUFSZ,
            "{\"model\":\"%s\",\"messages\":[%s],\"tools\":%s,\"stream\":true}",
            g_model, messages, TOOLS_JSON);
```

- [ ] **Step 4: agent_thread for 循环重构(用 http_post_stream)**

定位 `agent_thread` 的 for 循环体(从 `append_text("(thinking...)\r\n");` 到 content/tool_calls 分支结束,即整个一轮的处理)。当前结构:
```c
        append_text("(thinking...)\r\n");
        int status = call_llm();

        /* === 失败判定 ... === */
        if (status != 200) {
            append_text(resp);
            append_text(ROLLBACK_HINT);
            rolled_back = 1;
            goto done;
        }

        /* 无工具调用: 用解析器取 ... */
        {
            JValue *root = json_parse(resp);
            ... content 分支 ...
        }

        /* === 工具调用分支 === */
        JValue *root = json_parse(resp);
        ... tool_calls 分支 ...
```

整段替换为:
```c
        append_text("(thinking...)\r\n");

        /* 流式 SSE 请求 */
        StreamCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.content_start = GetWindowTextLength(g_hHistory);

        char full_url[1280];
        size_t nurl = strlen(g_api_url);
        int has_slash = (nurl > 0 && g_api_url[nurl-1] == '/');
        snprintf(full_url, sizeof(full_url), "%s%schat/completions",
                 g_api_url, has_slash ? "" : "/");

        int status = http_post_stream(full_url, g_api_key, body, strlen(body),
                                      resp, BUFSZ, &ctx);

        /* 取消 */
        if (status == -2) {
            append_text("(已取消)\r\n");
            rolled_back = 1;
            goto done;
        }
        /* 失败 (网络错误或非 200) */
        if (status != 200) {
            append_text(resp);
            append_text(ROLLBACK_HINT);
            rolled_back = 1;
            goto done;
        }

        /* 有 tool_calls: 执行 (复用现有 ToolCall 分发) */
        if (ctx.n_calls > 0) {
            typedef struct {
                char id[256]; char name[64]; char args[4096]; char *output;
            } ToolCall;
            ToolCall calls[8];
            int n_calls = 0;
            for (int i = 0; i < ctx.n_calls && n_calls < 8; i++) {
                if (!ctx.calls[i].name[0]) continue;   /* 跳过未累积到 name 的空槽 */
                ToolCall *c = &calls[n_calls++];
                c->id[0] = c->name[0] = c->args[0] = '\0';
                c->output = NULL;
                snprintf(c->id, sizeof(c->id), "%s", ctx.calls[i].id);
                snprintf(c->name, sizeof(c->name), "%s", ctx.calls[i].name);
                snprintf(c->args, sizeof(c->args), "%s", ctx.calls[i].args);
            }
            if (n_calls == 0) {
                append_text("(tool_calls 解析失败)\r\n");
                append_text(ROLLBACK_HINT);
                rolled_back = 1;
                goto done;
            }
            /* 执行 (分发逻辑同原) */
            for (int i = 0; i < n_calls; i++) {
                if (InterlockedCompareExchange(&g_cancel, 0, 0)) {
                    append_text("(已取消)\r\n");
                    rolled_back = 1;
                    for (int j = 0; j < n_calls; j++) free(calls[j].output);
                    goto done;
                }
                ToolCall *c = &calls[i];
                {
                    char line[8192];
                    snprintf(line, sizeof(line), "[Tool] %s(%s)\r\n", c->name, c->args);
                    append_text(line);
                }
                JValue *argsj = json_parse(c->args);
                if (strcmp(c->name, "execute_bash") == 0) {
                    const char *cmd = json_as_str(json_obj_get(argsj, "command"));
                    execute_bash(cmd ? cmd : "");
                } else if (strcmp(c->name, "read_file") == 0) {
                    const char *pp = json_as_str(json_obj_get(argsj, "path"));
                    tool_read_file(pp ? pp : "");
                } else if (strcmp(c->name, "write_file") == 0) {
                    const char *pp = json_as_str(json_obj_get(argsj, "path"));
                    const char *ct = json_as_str(json_obj_get(argsj, "content"));
                    tool_write_file(pp ? pp : "", ct ? ct : "");
                } else if (strcmp(c->name, "list_dir") == 0) {
                    const char *pp = json_as_str(json_obj_get(argsj, "path"));
                    tool_list_dir(pp ? pp : "");
                } else if (strcmp(c->name, "search") == 0) {
                    const char *pat = json_as_str(json_obj_get(argsj, "pattern"));
                    const char *pp = json_as_str(json_obj_get(argsj, "path"));
                    tool_search(pat ? pat : "", pp ? pp : "");
                } else {
                    strcpy(tool_out, "(未知工具)");
                }
                json_free(argsj);
                c->output = strdup(tool_out);
                {
                    char line[BUFSZ + 32];
                    snprintf(line, sizeof(line), "[Output]\r\n%s\r\n", tool_out);
                    append_text(line);
                }
            }
            /* 写 assistant + tool 消息 (同原第二/三遍逻辑, 保持不变) */
            size_t mstart = strlen(messages);
            int an = snprintf(messages + mstart, BUFSZ - mstart,
                ",{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[");
            int okf = (an > 0 && (size_t)an < BUFSZ - mstart);
            for (int i = 0; i < n_calls && okf; i++) {
                ToolCall *c = &calls[i];
                char *esc_id = json_escape_alloc(c->id);
                char *esc_name = json_escape_alloc(c->name);
                char *esc_args = json_escape_alloc(c->args);
                if (!esc_id || !esc_name || !esc_args) { okf = 0; free(esc_id); free(esc_name); free(esc_args); break; }
                size_t len = strlen(messages);
                int n2 = snprintf(messages + len, BUFSZ - len,
                    "%s{\"id\":\"%s\",\"type\":\"function\","
                    "\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}",
                    (i == 0 ? "" : ","), esc_id, esc_name, esc_args);
                free(esc_id); free(esc_name); free(esc_args);
                if (n2 <= 0 || (size_t)n2 >= BUFSZ - len) okf = 0;
            }
            if (okf) {
                size_t len = strlen(messages);
                int n2 = snprintf(messages + len, BUFSZ - len, "]}");
                if (n2 <= 0 || (size_t)n2 >= BUFSZ - len) okf = 0;
            }
            for (int i = 0; i < n_calls && okf; i++) {
                ToolCall *c = &calls[i];
                char *esc_id = json_escape_alloc(c->id);
                char *esc_out = json_escape_alloc(c->output ? c->output : "");
                if (!esc_id || !esc_out) { okf = 0; free(esc_id); free(esc_out); break; }
                size_t len = strlen(messages);
                int n2 = snprintf(messages + len, BUFSZ - len,
                    ",{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"%s\"}",
                    esc_id, esc_out);
                free(esc_id); free(esc_out);
                if (n2 <= 0 || (size_t)n2 >= BUFSZ - len) okf = 0;
            }
            for (int i = 0; i < n_calls; i++) free(calls[i].output);
            if (!okf) {
                messages[mstart] = '\0';
                append_text("(历史空间或内存不足, 本轮回滚)\r\n");
                rolled_back = 1;
                goto done;
            }
            /* 工具轮完成, 进入下一轮迭代 */
        } else {
            /* 无 tool_calls: content 收尾。Task 3 在此处插入 md_render 替换。
             * 当前先只把完整 content 追加进 messages (纯文本已由流式 append 显示)。 */
            if (ctx.content_len > 0) {
                char *esc = json_escape_alloc(ctx.content_buf);
                if (esc) {
                    size_t len = strlen(messages);
                    int an = snprintf(messages + len, BUFSZ - len,
                        ",{\"role\":\"assistant\",\"content\":\"%s\"}", esc);
                    free(esc);
                    if (an <= 0 || (size_t)an >= BUFSZ - len) {
                        append_text("(历史空间不足, 本轮回滚)\r\n");
                        rolled_back = 1;
                    }
                }
            } else {
                append_text("(响应解析失败)\r\n");
                append_text(ROLLBACK_HINT);
                rolled_back = 1;
            }
            goto done;
        }
```

注意:此替换删除了原 content 分支(`if (!strstr(resp, "\"tool_calls\"")) {...}`)和原 tool_calls 分支(`JValue *root = json_parse(resp);...`),由新的 `ctx.n_calls > 0` 分支统一处理。`StreamToolCall` 首次累积前 `memset(ctx,0)` 已把 id/name/args 清零;累积时 id/name 有则覆盖(首次 delta 带,后续不带),args 只追加不清空(片段拼接)。

- [ ] **Step 5: 删除已不再使用的 call_llm 与 http_post 函数**

`call_llm` 与 `http_post` 都被 `http_post_stream` 取代(agent_thread 不再调它们)。删除这两个函数(避免 unused 警告)。`http_set_err`/`winhttp_err_msg` 保留(`http_post_stream` 用)。删除前 `grep -n "call_llm\|http_post(" cagent_gui.c` 确认无其他引用。

- [ ] **Step 6: 编译 + selftest 回归**

Run: `make && ./cagent_gui.exe --selftest; echo exit=$?`
Expected: 零警告;selftest OK。

- [ ] **Step 7: 手动验证流式显示**

启动 GUI(需可用 API,且 API 支持 SSE),发问题:content 应逐字流式显示(`(thinking...)` 后增量出现文字),无 tool_calls 时收尾。发需工具的问题:`[Tool]` 显示后执行。

- [ ] **Step 8: 提交**

```bash
git add cagent_gui.c
git commit -m "feat: SSE 流式响应 (http_post_stream) + agent_thread 重构"
```

---

### Task 3: Markdown 渲染

**Files:**
- Modify: `cagent_gui.c`(新增 `rich_set_fmt`/`md_render`,agent_thread content 收尾替换)

- [ ] **Step 1: 加 rich_set_fmt 辅助函数**

在 `md_render` 之前插入:

```c
/* 选中 [start, end), 设置字符格式 */
static void rich_set_fmt(HWND h, LONG start, LONG end,
                         DWORD mask, DWORD effects, int yHeight,
                         COLORREF back, const WCHAR *face) {
    SendMessage(h, EM_SETSEL, start, end);
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = mask;
    cf.dwEffects = effects;
    if (yHeight > 0) cf.yHeight = yHeight;
    if (back != 0) { cf.crBackColor = back; }
    if (face) wcscpy(cf.szFaceName, face);
    SendMessage(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
}

/* 追加 UTF-16 文本到 RichEdit 末尾, 返回追加起始位置 */
static LONG rich_append(HWND h, const WCHAR *wtext) {
    LONG start = GetWindowTextLength(h);
    SendMessage(h, EM_SETSEL, start, start);
    SendMessage(h, EM_REPLACESEL, FALSE, (LPARAM)wtext);
    return start;
}
```

- [ ] **Step 2: 加 md_render 函数**

在 `rich_set_fmt` 之后插入:

```c
/* 把 Markdown 基础集 (标题/粗体/斜体/代码块/行内代码) 渲染追加到 RichEdit。
 * utf8: 完整 content 文本。 */
static void md_render(HWND h, const char *utf8) {
    /* 转 UTF-16 按行处理 */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return;
    WCHAR *wbuf = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (!wbuf) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wlen);

    const COLORREF code_bg = RGB(245, 245, 245);
    int in_codeblock = 0;
    WCHAR *line = wbuf;
    while (line < wbuf + wlen) {
        WCHAR *eol = wcschr(line, L'\n');
        int linelen = eol ? (int)(eol - line) : (int)(wbuf + wlen - 1 - line);
        /* 取一行到临时 (含 \0) */
        WCHAR save = line[linelen];
        line[linelen] = L'\0';

        /* 代码块围栏 ``` */
        if (wcsncmp(line, L"```", 3) == 0) {
            in_codeblock = !in_codeblock;
            LONG s = rich_append(h, line);
            rich_set_fmt(h, s, s + linelen, CFM_FACE | CFM_BACKCOLOR,
                         0, 0, code_bg, L"Consolas");
            rich_append(h, L"\r\n");
            line[linelen] = save;
            if (!eol) break;
            line = eol + 1;
            continue;
        }

        if (in_codeblock) {
            /* 代码块内: 等宽 + 灰底 */
            LONG s = rich_append(h, line);
            rich_set_fmt(h, s, s + linelen, CFM_FACE | CFM_BACKCOLOR,
                         0, 0, code_bg, L"Consolas");
            rich_append(h, L"\r\n");
            line[linelen] = save;
            if (!eol) break;
            line = eol + 1;
            continue;
        }

        /* 标题 # / ## / ### */
        int hlvl = 0, hoff = 0;
        if (wcsncmp(line, L"### ", 4) == 0) { hlvl = 3; hoff = 4; }
        else if (wcsncmp(line, L"## ", 3) == 0) { hlvl = 2; hoff = 3; }
        else if (wcsncmp(line, L"# ", 2) == 0) { hlvl = 1; hoff = 2; }
        if (hlvl > 0) {
            int yh = (hlvl == 1) ? 480 : (hlvl == 2) ? 400 : 360;
            LONG s = rich_append(h, line + hoff);
            rich_set_fmt(h, s, s + (linelen - hoff), CFM_SIZE | CFM_BOLD,
                         CFE_BOLD, yh, 0, NULL);
            rich_append(h, L"\r\n");
            line[linelen] = save;
            if (!eol) break;
            line = eol + 1;
            continue;
        }

        /* 普通行: 先追加, 再处理行内 ** ` * */
        LONG s = rich_append(h, line);
        /* 行内代码 `...` */
        {
            WCHAR *p = line;
            while ((p = wcsstr(p, L"`")) != NULL) {
                WCHAR *q = wcsstr(p + 1, L"`");
                if (!q) break;
                LONG cs = s + (LONG)(p + 1 - line);
                LONG ce = s + (LONG)(q - line);
                rich_set_fmt(h, cs, ce, CFM_FACE | CFM_BACKCOLOR, 0, 0, code_bg, L"Consolas");
                p = q + 1;
            }
        }
        /* 粗体 **...** */
        {
            WCHAR *p = line;
            while ((p = wcsstr(p, L"**")) != NULL) {
                WCHAR *q = wcsstr(p + 2, L"**");
                if (!q) break;
                LONG cs = s + (LONG)(p + 2 - line);
                LONG ce = s + (LONG)(q - line);
                rich_set_fmt(h, cs, ce, CFM_BOLD, CFE_BOLD, 0, 0, NULL);
                p = q + 2;
            }
        }
        /* 斜体 *...* (单 *, 避开 **; 简化: 只在无 ** 时处理, 或跳过 ** 已处理的) */
        /* 简化: 斜体处理略 (避免与 ** 冲突), 基础集可接受 */
        rich_append(h, L"\r\n");
        line[linelen] = save;
        if (!eol) break;
        line = eol + 1;
    }
    free(wbuf);
}
```

注意:斜体(`*...*`)与粗体(`**`)冲突,简化为不处理斜体(基础集可接受;若需,后续改进)。spec 列了斜体,但实现简化。执行时若需斜体,可加逻辑(避开 `**`)。本计划接受暂不处理斜体,提交信息注明。

- [ ] **Step 3: agent_thread content 收尾插入 md_render 替换**

定位 Task 2 Step 4 重构后的 content 收尾分支(`else { /* 无 tool_calls: content 收尾 */ ... }`),把:
```c
            /* 无 tool_calls: content 收尾。Task 3 在此处插入 md_render 替换。
             * 当前先只把完整 content 追加进 messages (纯文本已由流式 append 显示)。 */
            if (ctx.content_len > 0) {
                char *esc = json_escape_alloc(ctx.content_buf);
```
改为(在 `if (ctx.content_len > 0)` 内、追加 messages 之前,插入替换纯文本为 md_render):
```c
            /* 无 tool_calls: content 收尾。把流式显示的纯文本段替换为 Markdown 渲染。 */
            if (ctx.content_len > 0) {
                /* 选中并删除流式期间追加的纯文本 [content_start, 末尾) */
                LONG content_end = GetWindowTextLength(g_hHistory);
                SendMessage(g_hHistory, EM_SETSEL, ctx.content_start, content_end);
                SendMessage(g_hHistory, EM_REPLACESEL, FALSE, (LPARAM)L"");
                /* 渲染 Markdown 追加 */
                md_render(g_hHistory, ctx.content_buf);
                /* 追加 assistant content 到 messages */
                char *esc = json_escape_alloc(ctx.content_buf);
```

- [ ] **Step 4: 编译 + selftest 回归**

Run: `make && ./cagent_gui.exe --selftest; echo exit=$?`
Expected: 零警告;selftest OK。

- [ ] **Step 5: 手动验证 Markdown**

发问题让模型输出含标题/粗体/代码块的回复:流式显示纯文本 → 结束后变格式(标题大号粗体、代码块等宽灰底、粗体加粗)。

- [ ] **Step 6: 提交**

```bash
git add cagent_gui.c
git commit -m "feat: Markdown 基础集渲染 (标题/粗体/代码块/行内代码)"
```

---

### Task 4: README 补充

**Files:**
- Modify: `README.md`

- [ ] **Step 1: 技术特性表补 RichEdit/SSE/Markdown**

定位技术特性表"对话历史持久化"行之后:
```markdown
| 对话历史持久化 | 每轮 done 后保存 `cagent_history.json`,启动加载恢复 LLM 上下文 |
```
在其后追加:
```markdown
| 对话历史持久化 | 每轮 done 后保存 `cagent_history.json`,启动加载恢复 LLM 上下文 |
| RichEdit 显示 | 历史框用 RichEdit 2.0,支持格式化文本 |
| 流式 SSE | LLM 响应逐字流式显示(`stream:true`),工具调用 delta 累积 |
| Markdown 渲染 | 标题/粗体/代码块/行内代码基础集渲染(content 结束后替换) |
```

- [ ] **Step 2: 操作节补流式说明**

定位"### 操作"小节"运行中取消"行之后:
```markdown
- **运行中取消**:Agent 调用期间"发送"按钮变为"停止",点击即请求取消;取消在下一个安全点(下一轮迭代前或工具执行前)生效,正在进行的 HTTP 请求无法立即打断
```
在其后追加:
```markdown
- **流式显示**:LLM 回复逐字流式显示(纯文本),结束后自动渲染 Markdown 格式(标题/粗体/代码块)
```

- [ ] **Step 3: 提交**

```bash
git add README.md
git commit -m "docs: 补充 RichEdit/SSE/Markdown 显示特性说明"
```

---

### Task 5: 全量验证

**Files:** 无修改,仅验证

- [ ] **Step 1: 全量构建**

Run: `make clean && make`
Expected: 零警告(含 `-lriched20`)。

- [ ] **Step 2: selftest 回归**

Run: `./cagent_gui.exe --selftest; echo exit=$?`
Expected: `json_selftest: OK`,exit 0。

- [ ] **Step 3: 手动测试矩阵**

| # | 操作 | 预期 |
|---|---|---|
| 1 | 发普通问题 | content 流式逐字显示;结束后变 Markdown(标题/粗体) |
| 2 | 发需工具的问题 | `[Tool]` 显示;tool_calls 流式累积后执行;结果返回继续 |
| 3 | 让模型输出代码 | 代码块等宽 + 灰底 |
| 4 | 流式中点"停止" | 中止,回滚 |
| 5 | 网络错误 | 诊断显示 |
| 6 | RichEdit 滚动/选中/复制 | 正常 |
| 7 | 历史持久化/清空/watermark | 与 RichEdit 兼容,仍正常 |

- [ ] **Step 4: 完成确认**

矩阵 1–7 全通过 + 零警告 + selftest 全绿 → 显示重构子项目完成,阶段二全部完成。
