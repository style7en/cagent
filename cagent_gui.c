/*
 * cagent_gui.c - Win32 GUI 版极简 Agent
 *
 * 布局:
 *   +-----------------------------------+
 *   | API:   [ url-base       ]         |  (可编辑配置区)
 *   | Key:   [ ********       ]         |
 *   | Model: [ model-id       ]         |
 *   +-----------------------------------+
 *   |                                   |
 *   |  消息历史 (只读多行)              |
 *   |                                   |
 *   +-----------------------------------+
 *   | [输入框 单行]            [发送]   |
 *   +-----------------------------------+
 *
 * 设计:
 *   - 后台线程跑 Agent 循环, 通过 PostMessage(WM_APP_APPEND) 向 UI 追加文本
 *   - 工具调用过程也追加显示
 *   - LLM 调用期间禁用 输入框 + 发送按钮
 *
 * 编译: gcc -mwindows -o cagent_gui.exe cagent_gui.c -lcomctl32 -lwinhttp
 * 依赖: 无外部命令(WinHTTP 内置,工具调用走 cmd /c)
 */

#include <windows.h>
#include <commctrl.h>
#include <winhttp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define BUFSZ           (256 * 1024)
#define MAX_ITERATIONS  5

#define ID_HISTORY  1001
#define ID_INPUT    1002
#define ID_SEND     1003
#define ID_CFG_BASE 1004   /* +0 url, +1 key, +2 model */
#define ID_LBL_BASE 1007   /* +0 url, +1 key, +2 model */

#define WM_APP_APPEND  (WM_APP + 1)   /* lParam = UTF-8 char* (须 free) */
#define WM_APP_DONE    (WM_APP + 2)   /* Agent 任务完成,启用 UI */

#define CFG_URL 0
#define CFG_KEY 1
#define CFG_MDL 2

/* ===== 全局状态 ===== */
static HWND g_hHistory, g_hInput, g_hSend;
static HWND g_hCfg[3];                 /* [url, key, model] */
static HFONT g_hFont;
static HANDLE g_hThread = NULL;

/* 运行时配置(在 start_task 时从 Edit 控件同步) */
static char g_api_url[1024] = "";      /* 例: https://token.sensenova.cn/v1 */
static char g_api_key[512]  = "";
static char g_model[128]    = "";

/* Agent 工作缓冲(只在工作线程使用,主线程不碰) */
static char messages[BUFSZ];
static char body[BUFSZ];
static char resp[BUFSZ];
static char tool_out[BUFSZ];

static const char *TOOLS_JSON =
    "[{\"type\":\"function\",\"function\":{"
    "\"name\":\"execute_bash\","
    "\"description\":\"Execute a shell command via cmd /c\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"]}}}]";

/* ===== 工具函数: UTF-8 <-> UTF-16 ===== */
static void wide_to_utf8(const WCHAR *src, char *dst, int dst_bytes) {
    WideCharToMultiByte(CP_UTF8, 0, src, -1, dst, dst_bytes, NULL, NULL);
}

/* 向历史框追加一段文本(UTF-8)。线程安全:通过 PostMessage 投递 */
static void append_text(const char *utf8) {
    char *copy = strdup(utf8);
    if (copy) PostMessage(g_hHistory, WM_APP_APPEND, 0, (LPARAM)copy);
}

/* 主线程处理:把 UTF-8 -> UTF-16,LF 自动补成 CRLF,追加到 Edit 末尾 */
static void do_append(const char *utf8) {
    /* 先把 \n (非 \r\n 中的) 替换为 \r\n,避免 Edit 控件忽略 LF */
    size_t in_len = strlen(utf8);
    char *norm = (char*)malloc(in_len * 2 + 1);
    if (!norm) return;
    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = utf8[i];
        if (c == '\n' && (i == 0 || utf8[i-1] != '\r')) {
            norm[j++] = '\r';
            norm[j++] = '\n';
        } else {
            norm[j++] = c;
        }
    }
    norm[j] = '\0';

    int wlen = MultiByteToWideChar(CP_UTF8, 0, norm, -1, NULL, 0);
    if (wlen <= 0) { free(norm); return; }
    WCHAR *wbuf = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, norm, -1, wbuf, wlen);
    free(norm);

    int len = GetWindowTextLengthW(g_hHistory);
    SendMessageW(g_hHistory, EM_SETSEL, len, len);
    SendMessageW(g_hHistory, EM_REPLACESEL, FALSE, (LPARAM)wbuf);
    SendMessageW(g_hHistory, EM_SCROLLCARET, 0, 0);

    free(wbuf);
}

/* ===== JSON / HTTP / 工具 (从 cagent_mini.c 移植) ===== */

static void json_escape(const char *src, char *dst) {
    while (*src) {
        if (*src == '"' || *src == '\\') *dst++ = '\\';
        if (*src == '\n') { *dst++ = '\\'; *dst++ = 'n'; src++; continue; }
        if (*src == '\r') { src++; continue; }
        *dst++ = *src++;
    }
    *dst = '\0';
}

static int extract_string(const char *s, const char *key, char *out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(s, pattern);
    if (!p) return 0;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) {
            char c = *(p+1);
            if (c == 'n') { *out++ = '\n'; p += 2; }
            else if (c == 't') { *out++ = '\t'; p += 2; }
            else if (c == 'r') { p += 2; }
            else if (c == 'u' && p[2] && p[3] && p[4] && p[5]) {
                char hex[5] = { p[2], p[3], p[4], p[5], 0 };
                unsigned int v = (unsigned int)strtol(hex, NULL, 16);
                if (v < 0x80) *out++ = (char)v;
                else { *out++ = (char)(0xC0 | (v >> 6));
                       *out++ = (char)(0x80 | (v & 0x3F)); }
                p += 6;
            }
            else { *out++ = c; p += 2; }
        } else {
            *out++ = *p++;
        }
    }
    *out = '\0';
    return 1;
}

/* 用 CreateProcess + 匿名管道静默运行命令(仅本地工具用),纯内存收发数据。
 *   input/input_len: 通过 stdin 喂给子进程的数据(可为 NULL)
 *   output/out_cap : 接收子进程 stdout+stderr 的合并输出
 * 返回实际读到的字节数,失败返回 -1。 */
static int run_pipe(const char *cmdline,
                    const char *input, size_t input_len,
                    char *output, size_t out_cap) {
    HANDLE inR = NULL, inW = NULL, outR = NULL, outW = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

    if (!CreatePipe(&inR, &inW, &sa, 0)) return -1;
    if (!CreatePipe(&outR, &outW, &sa, 0)) {
        CloseHandle(inR); CloseHandle(inW); return -1;
    }
    SetHandleInformation(inW,  HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = inR;
    si.hStdOutput = outW;
    si.hStdError  = outW;

    char buf[16384];
    snprintf(buf, sizeof(buf), "cmd /c %s", cmdline);

    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(inR); CloseHandle(inW);
        CloseHandle(outR); CloseHandle(outW);
        return -1;
    }
    CloseHandle(inR);
    CloseHandle(outW);

    if (input && input_len > 0) {
        DWORD written;
        WriteFile(inW, input, (DWORD)input_len, &written, NULL);
    }
    CloseHandle(inW);

    size_t pos = 0;
    DWORD nread;
    char tmp[8192];
    while (pos + 1 < out_cap &&
           ReadFile(outR, tmp, sizeof(tmp), &nread, NULL) && nread > 0) {
        size_t copy = nread;
        if (pos + copy >= out_cap) copy = out_cap - 1 - pos;
        memcpy(output + pos, tmp, copy);
        pos += copy;
    }
    output[pos] = '\0';
    CloseHandle(outR);

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)pos;
}

/* ===== WinHTTP POST =====
 * 解析 url 得到 host/port/path/是否 https,然后 WinHTTP 发起请求。
 * 响应正文写入 out (含 \0),返回 HTTP 状态码,失败返回 -1。 */
static int http_post(const char *url, const char *api_key,
                     const char *body, size_t body_len,
                     char *out, size_t out_cap) {
    if (out_cap > 0) out[0] = '\0';

    const char *p = url;
    int https = 0;
    if (strncmp(p, "https://", 8) == 0) { https = 1; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; }
    else return -1;

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
    if (host_len >= sizeof(host)) return -1;
    memcpy(host, host_start, host_len);

    const char *path = path_start ? path_start : "/";

    WCHAR whost[256], wpath[1024];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, 256);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 1024);

    HINTERNET hSession = WinHttpOpen(L"cagent-gui/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return -1;

    HINTERNET hConnect = WinHttpConnect(hSession, whost, port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return -1; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return -1;
    }

    /* 头部: Content-Type + Authorization */
    char hdrs_a[1024];
    snprintf(hdrs_a, sizeof(hdrs_a),
        "Content-Type: application/json\r\nAuthorization: Bearer %s\r\n",
        api_key);
    WCHAR hdrs[1024];
    MultiByteToWideChar(CP_UTF8, 0, hdrs_a, -1, hdrs, 1024);
    WinHttpAddRequestHeaders(hReq, hdrs, (DWORD)wcslen(hdrs),
        WINHTTP_ADDREQ_FLAG_ADD);

    DWORD blen = (DWORD)body_len;
    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (LPVOID)body, blen, blen, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, NULL);

    int status = -1;
    if (ok) {
        DWORD st = 0, ss = sizeof(st);
        WinHttpQueryHeaders(hReq,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &st, &ss, WINHTTP_NO_HEADER_INDEX);
        status = (int)st;

        size_t pos = 0;
        char tmp[8192];
        DWORD nread;
        while (pos + 1 < out_cap &&
               WinHttpReadData(hReq, tmp, sizeof(tmp), &nread) && nread > 0) {
            size_t copy = nread;
            if (pos + copy >= out_cap) copy = out_cap - 1 - pos;
            memcpy(out + pos, tmp, copy);
            pos += copy;
        }
        out[pos] = '\0';
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return status;
}

static void call_llm(void) {
    /* 用户填的是 base url (例: https://x.com/v1),程序自动追加 /chat/completions */
    char full_url[1280];
    size_t n = strlen(g_api_url);
    int has_slash = (n > 0 && g_api_url[n-1] == '/');
    snprintf(full_url, sizeof(full_url), "%s%schat/completions",
             g_api_url, has_slash ? "" : "/");

    int status = http_post(full_url, g_api_key, body, strlen(body), resp, BUFSZ);
    if (status < 0) {
        snprintf(resp, BUFSZ, "{\"error\":\"WinHTTP request failed\"}");
    } else if (status != 200) {
        /* 保留响应体方便调试,但前面加状态码 */
        char prefix[64];
        int plen = snprintf(prefix, sizeof(prefix), "[HTTP %d] ", status);
        size_t blen = strlen(resp);
        if (plen + blen + 1 < BUFSZ) {
            memmove(resp + plen, resp, blen + 1);
            memcpy(resp, prefix, plen);
        }
    }
}

static void execute_bash(const char *command) {
    /* 本地命令仍走子进程 + 管道(无 HTTP) */
    int n = run_pipe(command, NULL, 0, tool_out, BUFSZ);
    if (n <= 0) strcpy(tool_out, "(no output)");
}

/* ===== Agent 工作线程 ===== */

typedef struct { char user_msg[BUFSZ]; } AgentTask;

static DWORD WINAPI agent_thread(LPVOID arg) {
    AgentTask *task = (AgentTask*)arg;

    /* 显示用户消息 */
    {
        char line[BUFSZ + 16];
        snprintf(line, sizeof(line), "\r\n>>> %s\r\n\r\n", task->user_msg);
        append_text(line);
    }

    /* 初始化 messages */
    char escaped[BUFSZ];
    json_escape(task->user_msg, escaped);
    snprintf(messages, BUFSZ,
        "{\"role\":\"system\",\"content\":\"你是 cagent,一个由 C 语言实现的极简 AI Agent。请始终使用中文回答。需要时调用工具。回答简洁。\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}", escaped);

    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        snprintf(body, BUFSZ,
            "{\"model\":\"%s\",\"messages\":[%s],\"tools\":%s}",
            g_model, messages, TOOLS_JSON);

        append_text("(thinking...)\r\n");
        call_llm();

        if (!strstr(resp, "\"tool_calls\"")) {
            char content[BUFSZ];
            if (extract_string(resp, "content", content)) {
                append_text(content);
                append_text("\r\n");
            } else {
                append_text("(no content)\r\n");
                append_text(resp);
                append_text("\r\n");
            }
            goto done;
        }

        char id[256] = "", name[64] = "", args[4096] = "";
        const char *tc = strstr(resp, "\"tool_calls\"");
        extract_string(tc, "id", id);
        extract_string(tc, "name", name);
        extract_string(tc, "arguments", args);

        {
            char line[8192];
            snprintf(line, sizeof(line), "[Tool] %s(%s)\r\n", name, args);
            append_text(line);
        }

        char command[4096] = "";
        extract_string(args, "command", command);
        execute_bash(command);

        {
            char line[BUFSZ + 32];
            snprintf(line, sizeof(line), "[Output]\r\n%s\r\n", tool_out);
            append_text(line);
        }

        char esc_args[8192], esc_out[BUFSZ];
        json_escape(args, esc_args);
        json_escape(tool_out, esc_out);

        size_t len = strlen(messages);
        snprintf(messages + len, BUFSZ - len,
            ",{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":["
            "{\"id\":\"%s\",\"type\":\"function\","
            "\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}]},"
            "{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"%s\"}",
            id, name, esc_args, id, esc_out);
    }
    append_text("(max iterations reached)\r\n");

done:
    free(task);
    PostMessage(g_hHistory, WM_APP_DONE, 0, 0);
    return 0;
}

/* ===== UI ===== */

/* 从一个 Edit 控件读 UTF-8 文本到指定缓冲(若空则保留原值) */
static void read_edit_utf8(HWND h, char *out, size_t cap) {
    int wlen = GetWindowTextLengthW(h);
    if (wlen <= 0) return;
    WCHAR *wbuf = (WCHAR*)malloc((wlen + 1) * sizeof(WCHAR));
    GetWindowTextW(h, wbuf, wlen + 1);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)cap, NULL, NULL);
    free(wbuf);
}

/* 把 UTF-8 字符串设到 Edit 控件 */
static void set_edit_utf8(HWND h, const char *utf8) {
    if (!utf8 || !*utf8) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return;
    WCHAR *wbuf = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wlen);
    SetWindowTextW(h, wbuf);
    free(wbuf);
}

/* ===== 配置文件 cagent.ini ===== */

/* 取 exe 同目录下的 cagent.ini 绝对路径 */
static void get_ini_path(char *out, size_t cap) {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { snprintf(out, cap, "cagent.ini"); return; }
    char *slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = '\0';
    else exe[0] = '\0';
    snprintf(out, cap, "%scagent.ini", exe);
}

/* 简单 key=value 解析器:遇到目标 key 把 value 拷入 out (去掉行尾 \r\n) */
static void config_load(void) {
    char path[MAX_PATH];
    get_ini_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        /* 去注释和空行 */
        if (line[0] == '#' || line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;

        /* 去掉 val 尾部的 \r \n */
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r')) {
            val[--vlen] = '\0';
        }

        if (strcmp(key, "url_base") == 0)
            snprintf(g_api_url, sizeof(g_api_url), "%s", val);
        else if (strcmp(key, "api_key") == 0)
            snprintf(g_api_key, sizeof(g_api_key), "%s", val);
        else if (strcmp(key, "model") == 0)
            snprintf(g_model, sizeof(g_model), "%s", val);
    }
    fclose(f);
}

/* 把当前 Edit 控件的值同步到全局并写回 ini */
static void config_save(void) {
    if (g_hCfg[CFG_URL]) read_edit_utf8(g_hCfg[CFG_URL], g_api_url, sizeof(g_api_url));
    if (g_hCfg[CFG_KEY]) read_edit_utf8(g_hCfg[CFG_KEY], g_api_key, sizeof(g_api_key));
    if (g_hCfg[CFG_MDL]) read_edit_utf8(g_hCfg[CFG_MDL], g_model,   sizeof(g_model));

    /* 三个全空就不写,避免覆盖出"空文件" */
    if (!g_api_url[0] && !g_api_key[0] && !g_model[0]) return;

    char path[MAX_PATH];
    get_ini_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "# cagent GUI 配置 (UTF-8, 退出时自动保存)\r\n");
    fprintf(f, "url_base=%s\r\n", g_api_url);
    fprintf(f, "api_key=%s\r\n",  g_api_key);
    fprintf(f, "model=%s\r\n",    g_model);
    fclose(f);
}

static void start_task(HWND hwnd) {
    if (g_hThread) {
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }

    int wlen = GetWindowTextLengthW(g_hInput);
    if (wlen <= 0) return;

    /* 同步配置 */
    read_edit_utf8(g_hCfg[CFG_URL], g_api_url, sizeof(g_api_url));
    read_edit_utf8(g_hCfg[CFG_KEY], g_api_key, sizeof(g_api_key));
    read_edit_utf8(g_hCfg[CFG_MDL], g_model,   sizeof(g_model));

    if (!g_api_url[0] || !g_api_key[0] || !g_model[0]) {
        MessageBoxW(hwnd, L"请填写 Url-Base、Key、Model 三项配置。",
                    L"配置不完整", MB_OK | MB_ICONWARNING);
        return;
    }

    WCHAR *wbuf = (WCHAR*)malloc((wlen + 1) * sizeof(WCHAR));
    GetWindowTextW(g_hInput, wbuf, wlen + 1);

    AgentTask *task = (AgentTask*)calloc(1, sizeof(AgentTask));
    wide_to_utf8(wbuf, task->user_msg, BUFSZ);
    free(wbuf);

    SetWindowTextW(g_hInput, L"");
    EnableWindow(g_hInput, FALSE);
    EnableWindow(g_hSend, FALSE);

    g_hThread = CreateThread(NULL, 0, agent_thread, task, 0, NULL);
}

static void layout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int gap = 8;
    int row_h = 26;             /* 配置区每行高度 */
    int lbl_w = 80;             /* 标签宽 (容纳 "Url-Base:") */
    int top_h = row_h * 3 + gap * 4;  /* 3 行 + 4 个间隙 */
    int btn_w = 80;
    int input_h = 28;
    int input_y = H - input_h - gap;
    int input_w = W - btn_w - gap * 3;

    /* 三行配置 */
    for (int i = 0; i < 3; i++) {
        int y = gap + i * (row_h + gap);
        HWND lbl = GetDlgItem(hwnd, ID_LBL_BASE + i);
        MoveWindow(lbl,        gap,         y + 4, lbl_w,               row_h, TRUE);
        MoveWindow(g_hCfg[i],  gap + lbl_w, y,     W - gap*2 - lbl_w,   row_h, TRUE);
    }

    MoveWindow(g_hHistory, gap, top_h, W - gap * 2,
               H - top_h - input_h - gap * 2, TRUE);
    MoveWindow(g_hInput, gap, input_y, input_w, input_h, TRUE);
    MoveWindow(g_hSend, gap * 2 + input_w, input_y, btn_w, input_h, TRUE);
}

/* 输入框子类化:Enter 触发发送 */
static WNDPROC g_oldInputProc;
static LRESULT CALLBACK InputProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        SendMessage(GetParent(h), WM_COMMAND, MAKEWPARAM(ID_SEND, BN_CLICKED), 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) return 0;  /* 抑制 beep */
    return CallWindowProc(g_oldInputProc, h, msg, wp, lp);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        /* 注意: 多行 EDIT + CLEARTYPE_QUALITY 在滚动时会出现字形 alpha 残留
         * (新字画在旧字上而不先擦除背景), 改用 ANTIALIASED_QUALITY 可消除重影. */
        g_hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, FF_DONTCARE, L"Microsoft YaHei UI");

        /* 顶部 3 行配置: Url-Base / Key / Model */
        static const WCHAR *labels[3] = { L"Url-Base:", L"Key:", L"Model:" };
        static const DWORD ed_styles[3] = { 0, ES_PASSWORD, 0 };

        for (int i = 0; i < 3; i++) {
            HWND lbl = CreateWindowW(L"STATIC", labels[i],
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)(ID_LBL_BASE + i), NULL, NULL);
            SendMessage(lbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            g_hCfg[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ed_styles[i],
                0, 0, 0, 0, hwnd,
                (HMENU)(LONG_PTR)(ID_CFG_BASE + i), NULL, NULL);
            SendMessage(g_hCfg[i], WM_SETFONT, (WPARAM)g_hFont, TRUE);
        }
        SendMessage(g_hCfg[CFG_KEY], EM_SETPASSWORDCHAR, (WPARAM)'*', 0);

        /* 读 cagent.ini 并填充三个 Edit */
        config_load();
        set_edit_utf8(g_hCfg[CFG_URL], g_api_url);
        set_edit_utf8(g_hCfg[CFG_KEY], g_api_key);
        set_edit_utf8(g_hCfg[CFG_MDL], g_model);

        g_hHistory = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_HISTORY, NULL, NULL);
        SendMessage(g_hHistory, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_INPUT, NULL, NULL);
        SendMessage(g_hInput, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        /* 子类化输入框,捕获 Enter */
        g_oldInputProc = (WNDPROC)SetWindowLongPtr(g_hInput, GWLP_WNDPROC, (LONG_PTR)InputProc);

        g_hSend = CreateWindowW(L"BUTTON", L"发送",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_SEND, NULL, NULL);
        SendMessage(g_hSend, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        SetFocus(g_hInput);
        return 0;
    }

    case WM_SIZE:
        layout(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_SEND && HIWORD(wp) == BN_CLICKED) {
            if (IsWindowEnabled(g_hSend)) start_task(hwnd);
            return 0;
        }
        break;

    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_DESTROY:
        config_save();   /* 退出时保存当前 Edit 值到 cagent.ini */
        if (g_hFont) DeleteObject(g_hFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 历史框子类化:接收 WM_APP_APPEND / WM_APP_DONE */
static WNDPROC g_oldHistoryProc;
static LRESULT CALLBACK HistoryProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP_APPEND) {
        char *txt = (char*)lp;
        do_append(txt);
        free(txt);
        return 0;
    }
    if (msg == WM_APP_DONE) {
        EnableWindow(g_hInput, TRUE);
        EnableWindow(g_hSend, TRUE);
        SetFocus(g_hInput);
        return 0;
    }
    return CallWindowProc(g_oldHistoryProc, h, msg, wp, lp);
}

/* ===== DPI 适配 =====
 * 优先级: GDI 缩放 (Win10 1809+) > System DPI Aware (Win8.1+) > 基础 DPI Aware (Vista+)
 * 全部通过 GetProcAddress 动态加载,旧系统上自动降级,不影响可执行文件运行
 */
static void enable_dpi_awareness(void) {
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (!hUser32) return;

    typedef DPI_AWARENESS_CONTEXT (WINAPI *PFN_SetCtx)(DPI_AWARENESS_CONTEXT);
    PFN_SetCtx pSetCtx = (PFN_SetCtx)(void*)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
    if (pSetCtx) {
        /* DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED = (HANDLE)-5
         * 让 Windows 用 GDI 缩放整个程序,字体清晰、布局正确,等同"系统增强" */
        if (pSetCtx((DPI_AWARENESS_CONTEXT)-5)) return;
    }

    typedef HRESULT (WINAPI *PFN_SetAwareness)(int);
    PFN_SetAwareness pSetAwareness = (PFN_SetAwareness)(void*)GetProcAddress(hUser32, "SetProcessDpiAwareness");
    if (pSetAwareness) {
        pSetAwareness(1); /* PROCESS_SYSTEM_DPI_AWARE */
        return;
    }

    typedef BOOL (WINAPI *PFN_SetAware)(void);
    PFN_SetAware pSetAware = (PFN_SetAware)(void*)GetProcAddress(hUser32, "SetProcessDPIAware");
    if (pSetAware) pSetAware();
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd;
    enable_dpi_awareness();   /* 必须在创建任何窗口之前调用 */
    InitCommonControls();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"CagentGuiWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(L"CagentGuiWnd", L"cagent GUI",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 720, 560,
        NULL, NULL, hInst, NULL);

    /* 历史框创建完后再子类化(此时 g_hHistory 已设) */
    g_oldHistoryProc = (WNDPROC)SetWindowLongPtr(g_hHistory, GWLP_WNDPROC, (LONG_PTR)HistoryProc);

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return 0;
}
