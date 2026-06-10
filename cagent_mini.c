/*
 * cagent_mini.c - 教学版 Agent (约 150 行)
 *
 * 核心思想:
 *   loop:
 *     resp = call_llm(messages + tools)
 *     if resp has tool_calls:
 *         execute each, append result to messages, continue
 *     else:
 *         print content, exit
 *
 * 工程简化:
 *   - HTTP 用 system("curl ...") 替代 WinHTTP
 *   - JSON 用 strstr 简单抽取,不写完整解析器
 *   - 只保留一个工具 execute_bash
 *   - 所有缓冲区都是 static 全局,不做溢出检查
 *
 * 编译: gcc -o cagent_mini.exe cagent_mini.c
 * 依赖: 系统中有 curl (Win10+ 自带)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define API_URL "https://token.sensenova.cn/v1/chat/completions"
#define API_KEY "sk-YOUR-API-KEY-HERE"
#define MODEL   "deepseek-v4-flash"

#define BUFSZ (256 * 1024)

static char messages[BUFSZ];   /* JSON 数组内容,含多条 message,不含外层 [] */
static char body[BUFSZ];       /* 完整请求体 */
static char resp[BUFSZ];       /* API 响应 */
static char tool_out[BUFSZ];   /* 工具输出 */

/* 工具定义 (硬编码 JSON schema) */
static const char *TOOLS_JSON =
    "[{\"type\":\"function\",\"function\":{"
    "\"name\":\"execute_bash\","
    "\"description\":\"Execute a shell command via cmd /c\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"]}}}]";

/* 把字符串里的 " 和 \ 简单转义,够用即可 */
static void json_escape(const char *src, char *dst) {
    while (*src) {
        if (*src == '"' || *src == '\\') *dst++ = '\\';
        if (*src == '\n') { *dst++ = '\\'; *dst++ = 'n'; src++; continue; }
        if (*src == '\r') { src++; continue; }
        *dst++ = *src++;
    }
    *dst = '\0';
}

/* 在 s 中找 "key": "value" 后的字符串值,写入 out。返回是否找到。*/
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
            else if (c == 'r') { p += 2; /* skip */ }
            else if (c == 'u' && p[2] && p[3] && p[4] && p[5]) {
                /* 解析 \uXXXX (仅 ASCII 范围 0x00-0xFF) */
                char hex[5] = { p[2], p[3], p[4], p[5], 0 };
                unsigned int v = (unsigned int)strtol(hex, NULL, 16);
                if (v < 0x80) *out++ = (char)v;
                else { *out++ = (char)0xC0 | (v >> 6);
                       *out++ = (char)0x80 | (v & 0x3F); }
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

/* 调用 LLM: 把 body 写文件,curl 发请求,响应读回 resp */
static void call_llm(void) {
    FILE *f = fopen("req.json", "wb");
    fputs(body, f);
    fclose(f);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "curl -s -X POST \"%s\" "
        "-H \"Content-Type: application/json\" "
        "-H \"Authorization: Bearer %s\" "
        "-d @req.json -o resp.json",
        API_URL, API_KEY);
    system(cmd);

    f = fopen("resp.json", "rb");
    size_t n = fread(resp, 1, BUFSZ - 1, f);
    resp[n] = '\0';
    fclose(f);
}

/* 执行 bash 命令,输出写入 tool_out */
static void execute_bash(const char *command) {
    char cmd[8192];
    /* 用 () 把命令括起来再 redirect,确保整体输出都被捕获 */
    snprintf(cmd, sizeof(cmd), "(%s) > tool_out.txt 2>&1", command);
    system(cmd);

    FILE *f = fopen("tool_out.txt", "rb");
    if (!f) { strcpy(tool_out, "(no output)"); return; }
    size_t n = fread(tool_out, 1, BUFSZ - 1, f);
    tool_out[n] = '\0';
    fclose(f);
    if (n == 0) strcpy(tool_out, "(no output)");
}

int main(int argc, char *argv[]) {
    const char *user_msg = (argc > 1) ? argv[1] : "Hello";
    printf("API:   %s\n", API_URL);
    printf("Model: %s\n", MODEL);
    printf("Task:  %s\n\n", user_msg);

    /* 初始化 messages: system + user */
    char escaped[BUFSZ];
    json_escape(user_msg, escaped);
    snprintf(messages, BUFSZ,
        "{\"role\":\"system\",\"content\":\"你是 cagent-mini,一个由 C 语言实现的极简教学版 AI Agent。请始终使用中文回答。需要时调用工具。回答简洁。\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}", escaped);

    for (int iter = 0; iter < 5; iter++) {
        /* 1. 组装 body */
        snprintf(body, BUFSZ,
            "{\"model\":\"%s\",\"messages\":[%s],\"tools\":%s}",
            MODEL, messages, TOOLS_JSON);

        /* 2. 调用 LLM */
        call_llm();

        /* 3. 检查是否有工具调用 */
        if (!strstr(resp, "\"tool_calls\"")) {
            /* 没有工具调用 → 抽 content 打印退出 */
            char content[BUFSZ];
            if (extract_string(resp, "content", content))
                printf("%s\n", content);
            else
                printf("(no content)\n[raw] %s\n", resp);
            return 0;
        }

        /* 4. 有工具调用 → 抽 id, name, arguments */
        char id[256] = "", name[64] = "", args[4096] = "";
        extract_string(resp, "id", id);  /* 注意: 这里抽到的是 response 顶层 id, 不是 tool_call id */
        /* 简单做法: 找 tool_calls 那段再抽 */
        const char *tc = strstr(resp, "\"tool_calls\"");
        extract_string(tc, "id", id);
        extract_string(tc, "name", name);
        extract_string(tc, "arguments", args);

        printf("[Tool] %s(%s)\n", name, args);

        /* 5. 执行工具 (arguments 里挑 command 字段) */
        char command[4096] = "";
        extract_string(args, "command", command);
        execute_bash(command);

        /* 6. 追加 assistant 消息 + tool 消息到 messages */
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

    printf("(max iterations reached)\n");
    return 1;
}
