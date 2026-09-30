/*
 * cagent_core.h - cagent 平台无关核心 (零依赖, Windows 平台)
 *
 * 包含: 轻量 JSON 解析器 / DPAPI Key 加密 / UTF 转码 / WinHTTP SSE 流式 /
 *       工具系统 / Agent 循环 / 配置与历史持久化。
 *
 * 与界面解耦: 通过一组宿主钩子 (cagent_emit / cagent_on_done / cagent_read_config_ui)
 * 与前端交互, 因此 Win32 GUI 复用同一段 Agent 循环。
 *
 * 设计取向 (对齐 pi.dev 的极简理念): 仅 3 个工具 (bash/read/write), 无权限弹窗,
 * 无轮次上限, 由模型自行决定何时停止, 用户随时可用停止按钮取消。
 *
 * 使用方式: 前端 .c 文件 #include 本头文件, 并在调用 agent_thread / agent_turn
 * 之前设置钩子; 钩子未设置时采用安全默认 (不读 UI 配置)。
 */

#ifndef CAGENT_CORE_H
#define CAGENT_CORE_H

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ===== 宿主钩子 (由 GUI 前端实现) ===== */
#define CAGENT_ROLE_SYS   0   /* 系统/过程提示 (thinking/回滚/工具结果/载入提示) */
#define CAGENT_ROLE_USER  1   /* 用户输入 */
#define CAGENT_ROLE_AI    2   /* AI 输出 */
static void (*cagent_emit)(const char *utf8, int role) = NULL;  /* 输出一段文本 (role 区分角色) */
static void (*cagent_on_done)(void) = NULL;                   /* 一轮 Agent 结束 */
static void (*cagent_read_config_ui)(void) = NULL;            /* 从 UI 同步配置到全局 */

/* 安全的输出封装: 前端只需提供 cagent_emit。默认按 SYS 角色输出。 */
static void append_text(const char *utf8) {
    if (cagent_emit) cagent_emit(utf8, CAGENT_ROLE_SYS);
}

/* ===== 轻量 JSON 解析器 (零依赖, 递归下降) =====
 * 仅用于解析 LLM 响应与 tool_call.arguments。严格 JSON, 不支持注释/trailing comma。
 * 解码完整转义含 UTF-16 代理对。失败返回 NULL, 不部分返回。 */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JValue JValue;
struct JValue {
    JType type;
    union {
        int b;                                          /* J_BOOL */
        double num;                                     /* J_NUM  */
        char *str;                                      /* J_STR, 已解码 UTF-8, 堆分配 */
        struct { JValue **items; size_t n, cap; } arr;  /* J_ARR */
        struct { char **keys; JValue **vals; size_t n, cap; } obj; /* J_OBJ */
    };
};

/* 前向声明: selftest 在桩实现之前定义, 需先声明全部公共 API */
void         json_free(JValue *v);
JValue       *json_parse(const char *text);
const JValue *json_obj_get(const JValue *obj, const char *key);
const JValue *json_arr_at(const JValue *arr, size_t i);
const char   *json_as_str(const JValue *v);

/* ===== DPAPI Key 加密 =====
 * 加密: 明文 -> "dpapi:<base64>"。解密: "dpapi:<base64>" 或明文 -> 明文。
 * 失败返回 NULL。返回值 malloc, 调用者 free。 */
static char *dpapi_protect(const char *plain) {
    if (!plain) return NULL;
    DATA_BLOB in = { (DWORD)strlen(plain), (BYTE*)plain };
    DATA_BLOB out = {0};
    if (!CryptProtectData(&in, NULL, NULL, NULL, NULL, 0, &out)) return NULL;
    DWORD b64len = 0;
    CryptBinaryToStringA(out.pbData, out.cbData,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &b64len);
    char *b64 = (char*)malloc(b64len);
    if (!b64) { LocalFree(out.pbData); return NULL; }
    CryptBinaryToStringA(out.pbData, out.cbData,
        CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &b64len);
    LocalFree(out.pbData);
    char *res = (char*)malloc(6 + b64len);
    if (!res) { free(b64); return NULL; }
    memcpy(res, "dpapi:", 6);
    memcpy(res + 6, b64, b64len);   /* b64len 含 '\0' */
    free(b64);
    return res;
}

static char *dpapi_unprotect(const char *stored) {
    if (!stored) return NULL;
    if (strncmp(stored, "dpapi:", 6) != 0) {
        return strdup(stored);   /* 明文兼容 */
    }
    const char *b64 = stored + 6;
    DWORD binlen = 0;
    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, NULL, &binlen, NULL, NULL))
        return NULL;
    BYTE *bin = (BYTE*)malloc(binlen);
    if (!bin) return NULL;
    if (!CryptStringToBinaryA(b64, 0, CRYPT_STRING_BASE64, bin, &binlen, NULL, NULL)) {
        free(bin); return NULL;
    }
    DATA_BLOB in = { binlen, bin };
    DATA_BLOB out = {0};
    BOOL ok = CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &out);
    free(bin);
    if (!ok) return NULL;
    char *res = (char*)malloc(out.cbData + 1);
    if (!res) { LocalFree(out.pbData); return NULL; }
    memcpy(res, out.pbData, out.cbData);
    res[out.cbData] = '\0';
    LocalFree(out.pbData);
    return res;
}

static int json_selftest(void) {
    int fails = 0;
    #define CHK(cond) do { if(!(cond)) { printf("FAIL: %s\n", #cond); fails++; } } while(0)

    /* 基本对象 + 各类型 */
    {
        JValue *r = json_parse("{\"name\":\"abc\",\"n\":3,\"b\":true,\"x\":null}");
        CHK(r != NULL && r->type == J_OBJ);
        CHK(json_as_str(json_obj_get(r,"name")) && strcmp(json_as_str(json_obj_get(r,"name")),"abc")==0);
        CHK(json_obj_get(r,"n") && json_obj_get(r,"n")->num == 3.0);
        CHK(json_obj_get(r,"b") && json_obj_get(r,"b")->b == 1);
        CHK(json_obj_get(r,"x") && json_obj_get(r,"x")->type == J_NULL);
        json_free(r);
    }
    /* 数组 + 嵌套对象 */
    {
        JValue *r = json_parse("{\"arr\":[1,2,{\"k\":\"v\"}]}");
        CHK(r != NULL);
        const JValue *a = json_obj_get(r,"arr");
        CHK(a && a->type == J_ARR && a->arr.n == 3);
        CHK(json_arr_at(a,1) && json_arr_at(a,1)->num == 2.0);
        CHK(json_as_str(json_obj_get(json_arr_at(a,2),"k")) && strcmp(json_as_str(json_obj_get(json_arr_at(a,2),"k")),"v")==0);
        json_free(r);
    }
    /* 转义 + 中文 + 代理对 */
    {
        JValue *r = json_parse("\"a\\nb\\tc\\\\d\\\"e\\/\\u4e2d\\uD83D\\uDE00\"");
        CHK(r != NULL && r->type == J_STR);
        CHK(r && strcmp(r->str, "a\nb\tc\\d\"e/中😀") == 0);
        json_free(r);
    }
    /* OpenAI 风格 tool_calls (arguments 是字符串化 JSON, 需二次解析) */
    {
        JValue *r = json_parse("{\"choices\":[{\"message\":{\"content\":null,\"tool_calls\":[{\"id\":\"call_1\",\"function\":{\"name\":\"execute_bash\",\"arguments\":\"{\\\"command\\\":\\\"ls\\\"}\"}}]}}]}");
        CHK(r != NULL);
        const JValue *msg = json_obj_get(json_arr_at(json_obj_get(r,"choices"),0),"message");
        CHK(msg != NULL);
        const JValue *tcs = json_obj_get(msg,"tool_calls");
        CHK(tcs && tcs->type==J_ARR && tcs->arr.n==1);
        const JValue *tc0 = json_arr_at(tcs,0);
        CHK(json_as_str(json_obj_get(tc0,"id")) && strcmp(json_as_str(json_obj_get(tc0,"id")),"call_1")==0);
        const char *args = json_as_str(json_obj_get(json_obj_get(tc0,"function"),"arguments"));
        CHK(args != NULL);
        JValue *argsj = json_parse(args);
        CHK(argsj != NULL);
        CHK(json_as_str(json_obj_get(argsj,"command")) && strcmp(json_as_str(json_obj_get(argsj,"command")),"ls")==0);
        json_free(argsj);
        json_free(r);
    }
    /* 非法输入 */
    CHK(json_parse("{") == NULL);
    CHK(json_parse("[1,]") == NULL);
    CHK(json_parse("\"unterminated") == NULL);
    CHK(json_parse("") == NULL);
    /* BOM + 前后空白 */
    {
        JValue *r = json_parse("\xEF\xBB\xBF  {\"k\":\"v\"}  ");
        CHK(r != NULL && strcmp(json_as_str(json_obj_get(r,"k")),"v")==0);
        json_free(r);
    }

    /* DPAPI round-trip + 明文兼容 */
    {
        const char *plain = "sk-test-key-123";
        char *enc = dpapi_protect(plain);
        CHK(enc != NULL && strncmp(enc, "dpapi:", 6) == 0);
        char *dec = dpapi_unprotect(enc);
        CHK(dec != NULL && strcmp(dec, plain) == 0);
        free(enc); free(dec);
        char *dec2 = dpapi_unprotect("sk-plain-key");
        CHK(dec2 != NULL && strcmp(dec2, "sk-plain-key") == 0);
        free(dec2);
    }

    if (fails == 0) printf("json_selftest: OK\n");
    else printf("json_selftest: %d FAIL(s)\n", fails);
    return fails ? 1 : 0;
    #undef CHK
}

/* —— 真实实现 —— */

typedef struct {
    const char *p;
    int ok;
} JParser;

static void json_skip_ws(JParser *ps) {
    while (*ps->p==' '||*ps->p=='\t'||*ps->p=='\n'||*ps->p=='\r') ps->p++;
}

static JValue *json_parse_value(JParser *ps);

static JValue *json_new(JType t) {
    JValue *v = (JValue*)calloc(1, sizeof(JValue));
    if (v) v->type = t;
    return v;
}

/* 解析 "..." 字面量, 返回 malloc 的解码 UTF-8。调用时 ps->p 指向开头的 "。 */
static char *json_parse_str_raw(JParser *ps) {
    if (*ps->p != '"') { ps->ok = 0; return NULL; }
    ps->p++;
    size_t cap = 16, len = 0;
    char *out = (char*)malloc(cap);
    if (!out) { ps->ok = 0; return NULL; }
    while (*ps->p && *ps->p != '"') {
        char c = *ps->p;
        if (c == '\\') {
            ps->p++;
            char e = *ps->p;
            switch (e) {
            case '"':  c = '"';  ps->p++; break;
            case '\\': c = '\\'; ps->p++; break;
            case '/':  c = '/';  ps->p++; break;
            case 'b':  c = '\b'; ps->p++; break;
            case 'f':  c = '\f'; ps->p++; break;
            case 'n':  c = '\n'; ps->p++; break;
            case 'r':  c = '\r'; ps->p++; break;
            case 't':  c = '\t'; ps->p++; break;
            case 'u': {
                if (!ps->p[1]||!ps->p[2]||!ps->p[3]||!ps->p[4]) { ps->ok=0; free(out); return NULL; }
                char hex[5] = { ps->p[1], ps->p[2], ps->p[3], ps->p[4], 0 };
                ps->p += 5;
                unsigned int cp = (unsigned int)strtoul(hex, NULL, 16);
                /* 代理对 */
                if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0]=='\\' && ps->p[1]=='u'
                    && ps->p[2] && ps->p[3] && ps->p[4] && ps->p[5]) {
                    char lo[5] = { ps->p[2], ps->p[3], ps->p[4], ps->p[5], 0 };
                    unsigned int lcp = (unsigned int)strtoul(lo, NULL, 16);
                    if (lcp >= 0xDC00 && lcp <= 0xDFFF) {
                        ps->p += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lcp - 0xDC00);
                    }
                }
                if (cp < 0x80) {
                    c = (char)cp;
                } else {
                    char tmp[4]; int tn;
                    if (cp < 0x800) { tmp[0]=(char)(0xC0|(cp>>6)); tmp[1]=(char)(0x80|(cp&0x3F)); tn=2; }
                    else if (cp < 0x10000) { tmp[0]=(char)(0xE0|(cp>>12)); tmp[1]=(char)(0x80|((cp>>6)&0x3F)); tmp[2]=(char)(0x80|(cp&0x3F)); tn=3; }
                    else { tmp[0]=(char)(0xF0|(cp>>18)); tmp[1]=(char)(0x80|((cp>>12)&0x3F)); tmp[2]=(char)(0x80|((cp>>6)&0x3F)); tmp[3]=(char)(0x80|(cp&0x3F)); tn=4; }
                    if (len + tn + 1 > cap) {
                        while (len + tn + 1 > cap) cap *= 2;
                        char *nw = (char*)realloc(out, cap);
                        if (!nw) { ps->ok=0; free(out); return NULL; }
                        out = nw;
                    }
                    memcpy(out + len, tmp, tn);
                    len += tn;
                    continue;
                }
                break;
            }
            default: ps->ok = 0; free(out); return NULL;
            }
        } else {
            ps->p++;
        }
        if (len + 2 > cap) {
            cap *= 2;
            char *nw = (char*)realloc(out, cap);
            if (!nw) { ps->ok=0; free(out); return NULL; }
            out = nw;
        }
        out[len++] = c;
    }
    if (*ps->p != '"') { ps->ok = 0; free(out); return NULL; }
    ps->p++;
    out[len] = '\0';
    return out;
}

static JValue *json_parse_array(JParser *ps) {
    ps->p++; /* [ */
    JValue *v = json_new(J_ARR);
    if (!v) { ps->ok = 0; return NULL; }
    json_skip_ws(ps);
    if (*ps->p == ']') { ps->p++; return v; }
    for (;;) {
        JValue *item = json_parse_value(ps);
        if (!ps->ok) { json_free(item); json_free(v); return NULL; }
        if (v->arr.n == v->arr.cap) {
            size_t nc = v->arr.cap ? v->arr.cap*2 : 4;
            JValue **ni = (JValue**)realloc(v->arr.items, nc*sizeof(JValue*));
            if (!ni) { json_free(item); json_free(v); ps->ok=0; return NULL; }
            v->arr.items = ni; v->arr.cap = nc;
        }
        v->arr.items[v->arr.n++] = item;
        json_skip_ws(ps);
        if (*ps->p == ',') { ps->p++; json_skip_ws(ps); continue; }
        if (*ps->p == ']') { ps->p++; break; }
        ps->ok = 0; json_free(v); return NULL;
    }
    return v;
}

static JValue *json_parse_object(JParser *ps) {
    ps->p++; /* { */
    JValue *v = json_new(J_OBJ);
    if (!v) { ps->ok = 0; return NULL; }
    json_skip_ws(ps);
    if (*ps->p == '}') { ps->p++; return v; }
    for (;;) {
        json_skip_ws(ps);
        char *key = json_parse_str_raw(ps);
        if (!ps->ok) { json_free(v); return NULL; }
        json_skip_ws(ps);
        if (*ps->p != ':') { ps->ok=0; free(key); json_free(v); return NULL; }
        ps->p++;
        JValue *val = json_parse_value(ps);
        if (!ps->ok) { free(key); json_free(val); json_free(v); return NULL; }
        if (v->obj.n == v->obj.cap) {
            size_t nc = v->obj.cap ? v->obj.cap*2 : 4;
            char **nk = (char**)realloc(v->obj.keys, nc*sizeof(char*));
            if (!nk) { free(key); json_free(val); json_free(v); ps->ok=0; return NULL; }
            v->obj.keys = nk;
            JValue **nv = (JValue**)realloc(v->obj.vals, nc*sizeof(JValue*));
            if (!nv) { free(key); json_free(val); json_free(v); ps->ok=0; return NULL; }
            v->obj.vals = nv; v->obj.cap = nc;
        }
        v->obj.keys[v->obj.n] = key;
        v->obj.vals[v->obj.n] = val;
        v->obj.n++;
        json_skip_ws(ps);
        if (*ps->p == ',') { ps->p++; continue; }
        if (*ps->p == '}') { ps->p++; break; }
        ps->ok = 0; json_free(v); return NULL;
    }
    return v;
}

static JValue *json_parse_value(JParser *ps) {
    json_skip_ws(ps);
    char c = *ps->p;
    if (c == '"') {
        char *s = json_parse_str_raw(ps);
        if (!ps->ok) return NULL;
        JValue *v = json_new(J_STR);
        if (!v) { free(s); ps->ok=0; return NULL; }
        v->str = s;
        return v;
    }
    if (c == '{') return json_parse_object(ps);
    if (c == '[') return json_parse_array(ps);
    if (c == 't') {
        if (strncmp(ps->p,"true",4)==0) { ps->p+=4; JValue *v=json_new(J_BOOL); if(v)v->b=1; return v; }
        ps->ok=0; return NULL;
    }
    if (c == 'f') {
        if (strncmp(ps->p,"false",5)==0) { ps->p+=5; JValue *v=json_new(J_BOOL); if(v)v->b=0; return v; }
        ps->ok=0; return NULL;
    }
    if (c == 'n') {
        if (strncmp(ps->p,"null",4)==0) { ps->p+=4; return json_new(J_NULL); }
        ps->ok=0; return NULL;
    }
    if (c=='-' || (c>='0' && c<='9')) {
        char *end;
        double d = strtod(ps->p, &end);
        if (end == ps->p) { ps->ok=0; return NULL; }
        ps->p = end;
        JValue *v = json_new(J_NUM);
        if (v) v->num = d;
        return v;
    }
    ps->ok = 0;
    return NULL;
}

JValue *json_parse(const char *text) {
    if (!text) return NULL;
    JParser ps = { text, 1 };
    /* 跳过 UTF-8 BOM */
    if ((unsigned char)text[0]==0xEF && (unsigned char)text[1]==0xBB && (unsigned char)text[2]==0xBF)
        ps.p = text + 3;
    JValue *v = json_parse_value(&ps);
    if (!ps.ok) { json_free(v); return NULL; }
    json_skip_ws(&ps);
    if (*ps.p != '\0') { json_free(v); return NULL; }
    return v;
}

const JValue *json_obj_get(const JValue *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (size_t i = 0; i < obj->obj.n; i++)
        if (strcmp(obj->obj.keys[i], key) == 0) return obj->obj.vals[i];
    return NULL;
}

const JValue *json_arr_at(const JValue *arr, size_t i) {
    if (!arr || arr->type != J_ARR || i >= arr->arr.n) return NULL;
    return arr->arr.items[i];
}

const char *json_as_str(const JValue *v) {
    if (!v || v->type != J_STR) return NULL;
    return v->str;
}

void json_free(JValue *v) {
    if (!v) return;
    switch (v->type) {
    case J_STR: free(v->str); break;
    case J_ARR:
        for (size_t i=0;i<v->arr.n;i++) json_free(v->arr.items[i]);
        free(v->arr.items);
        break;
    case J_OBJ:
        for (size_t i=0;i<v->obj.n;i++) { free(v->obj.keys[i]); json_free(v->obj.vals[i]); }
        free(v->obj.keys); free(v->obj.vals);
        break;
    default: break;
    }
    free(v);
}

#define BUFSZ           (256 * 1024)
#define ARGS_MAX        8192   /* tool_calls.arguments 累积上限 (流式与执行期统一) */

/* ===== 全局状态 ===== */
static char g_api_url[1024] = "";      /* 例: https://token.sensenova.cn/v1 */
static char g_api_key[512]  = "";
static char g_model[128]    = "";
static int g_key_decrypt_failed = 0;   /* DPAPI 解密失败标志, 启动后提示 */
static int g_skip_cert_verify = 0;     /* 1=跳过 SSL 证书校验 (自签端点用) */
static char g_workspace[MAX_PATH] = "";/* 工作目录 (文件工具限制在此目录内, 默认 exe 目录) */
static char g_active_ws[MAX_PATH] = "";/* 当前已加载历史所对应的工作目录 */
static char g_last_session[MAX_PATH] = ""; /* 最近使用的会话文件 (持久化到 ini) */
static char g_history_file[MAX_PATH] = ""; /* 当前对话绑定的会话文件 (懒生成) */

/* Agent 工作缓冲(只在工作线程使用,主线程不碰) */
static char messages[BUFSZ];
static char body[BUFSZ];
static char resp[BUFSZ];
static char tool_out[BUFSZ];

static volatile LONG g_running = 0;   /* 1 = Agent 工作线程运行中 */
static volatile LONG g_cancel  = 0;   /* 1 = 请求取消 */

static const char *TOOLS_JSON =
    "[{\"type\":\"function\",\"function\":{"
    "\"name\":\"execute_bash\","
    "\"description\":\"Execute a shell command via cmd /c\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"read_file\","
    "\"description\":\"Read text content of a file\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"write_file\","
    "\"description\":\"Write content to a file (overwrite)\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"content\"]}}}]";

/* ===== JSON 转义 ===== */

/* JSON 字符串转义: 覆盖所有 0x00-0x1F 控制字符, 否则服务端解析失败.
 * \r 直接丢弃。有界写入。返回 1 成功, 0 容量不足。最坏膨胀比 6x。 */
static int json_escape(const char *src, char *dst, size_t dst_cap) {
    if (dst_cap == 0) return 0;
    size_t j = 0;
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        if (j + 6 >= dst_cap) { dst[dst_cap - 1] = '\0'; return 0; }
        switch (c) {
        case '"':  dst[j++] = '\\'; dst[j++] = '"';  break;
        case '\\': dst[j++] = '\\'; dst[j++] = '\\'; break;
        case '\n': dst[j++] = '\\'; dst[j++] = 'n';  break;
        case '\t': dst[j++] = '\\'; dst[j++] = 't';  break;
        case '\b': dst[j++] = '\\'; dst[j++] = 'b';  break;
        case '\f': dst[j++] = '\\'; dst[j++] = 'f';  break;
        case '\r': break;   /* 丢弃 */
        default:
            if (c < 0x20) {
                j += sprintf(dst + j, "\\u%04X", c);
            } else {
                dst[j++] = (char)c;
            }
        }
    }
    dst[j] = '\0';
    return 1;
}

static char *json_escape_alloc(const char *src) {
    size_t cap = strlen(src) * 6 + 1;
    char *buf = (char*)malloc(cap);
    if (!buf) return NULL;
    json_escape(src, buf, cap);
    return buf;
}

/* 命令执行超时 (毫秒): 默认 60s, 可用环境变量 CAGENT_CMD_TIMEOUT (秒) 覆盖。 */
static int cmd_timeout_ms(void) {
    char *e = getenv("CAGENT_CMD_TIMEOUT");
    if (e && atoi(e) > 0) return atoi(e) * 1000;
    return 60000;
}

/* 用 CreateProcess + 匿名管道静默运行命令(仅本地工具用),纯内存收发数据。
 * 子进程的 stdout+stderr 合并写入 output (含 \0)。
 * 返回实际读到的字节数,失败返回 -1。超过 timeout 则终止整个进程树。 */
static size_t utf8_trim_len(const char *s, size_t len);   /* 前向声明 */

static int run_pipe(const char *cmdline, char *output, size_t out_cap) {
    if (out_cap) memset(output, 0, out_cap);   /* 清空, 避免上一轮残留泄漏 */
    HANDLE outR = NULL, outW = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

    if (!CreatePipe(&outR, &outW, &sa, 0)) return -1;
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outW;
    si.hStdError  = outW;

    /* 命令行必须转宽字符交给 CreateProcessW, 否则中文参数会被按 ANSI 解释 */
    char buf[16384];
    snprintf(buf, sizeof(buf), "cmd /c %s", cmdline);
    wchar_t wbuf[16384];
    if (MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, 16384) <= 0) {
        CloseHandle(outR); CloseHandle(outW);
        return -1;
    }

    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(NULL, wbuf, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(outR); CloseHandle(outW);
        return -1;
    }
    CloseHandle(outW);

    /* 作业对象: 超时统一杀死进程树 (含 cmd 派生的子进程) */
    HANDLE hJob = NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    memset(&jeli, 0, sizeof(jeli));
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    hJob = CreateJobObjectA(NULL, NULL);
    if (hJob && SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                                        &jeli, sizeof(jeli))) {
        AssignProcessToJobObject(hJob, pi.hProcess); /* 失败则退化为仅杀主进程 */
    }

    size_t pos = 0;
    DWORD nread;
    char tmp[8192];
    int timedout = 0;
    int timeout = cmd_timeout_ms();
    DWORD deadline = GetTickCount() + (DWORD)timeout;

    while (pos + 1 < out_cap) {
        DWORD avail = 0;
        if (!PeekNamedPipe(outR, NULL, 0, NULL, &avail, NULL)) break; /* 管道关闭/出错 */
        if (avail == 0) {
            if (GetTickCount() >= deadline) { timedout = 1; break; }
            Sleep(15);
            continue;
        }
        DWORD toread = (avail > sizeof(tmp)) ? (DWORD)sizeof(tmp) : avail;
        if (!ReadFile(outR, tmp, toread, &nread, NULL) || nread == 0) break;
        size_t copy = nread;
        if (pos + copy >= out_cap) copy = out_cap - 1 - pos;
        memcpy(output + pos, tmp, copy);
        pos += copy;
    }
    /* 缓冲可能在多字节字符中间被截断: 回退到字符边界, 保证输出是合法 UTF-8 */
    pos = utf8_trim_len(output, pos);
    output[pos] = '\0';

    if (timedout) {
        if (hJob) TerminateJobObject(hJob, 1);
        else TerminateProcess(pi.hProcess, 1);
        char note[] = "\n(命令执行超时, 已终止)";
        size_t nl = sizeof(note) - 1;
        if (pos + nl < out_cap) memcpy(output + pos, note, nl);
        pos = strlen(output);   /* 让返回值 > 0, 超时提示不被当作"无输出" */
    }

    WaitForSingleObject(pi.hProcess, timedout ? 2000 : INFINITE);
    if (hJob) CloseHandle(hJob);
    CloseHandle(outR);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)pos;
}

/* 把 WinHTTP/系统错误码翻译为中文可读文本。 */
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

/* SSE 流式 POST: 增量读取, content delta 回调, tool_calls delta 累积到 ctx。
 * 返回 HTTP 状态码; -2=取消; -1=网络错误 (err_out 写诊断)。 */
static int http_post_stream(const char *url, const char *api_key,
                            const char *body, size_t body_len,
                            char *err_out, size_t err_cap,
                            StreamCtx *ctx) {
    if (err_cap > 0) err_out[0] = '\0';

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

/* 把 cmd.exe 的 OEM/ANSI 输出转成 UTF-8。 */
static void oem_to_utf8(char *buf, size_t cap) {
    UINT cp = GetOEMCP();
    if (cp == CP_UTF8 || buf[0] == '\0') return;

    int wlen = MultiByteToWideChar(cp, 0, buf, -1, NULL, 0);
    if (wlen <= 0) return;
    WCHAR *w = (WCHAR*)malloc(wlen * sizeof(WCHAR));
    if (!w) return;
    MultiByteToWideChar(cp, 0, buf, -1, w, wlen);

    int u8len = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (u8len > 0 && (size_t)u8len <= cap) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)cap, NULL, NULL);
    }
    free(w);
}

/* 严格 UTF-8 校验: 合法则原样保留, 否则按 OEM(GBK) 转换。
   命令输出可能来自本地 cmd (GBK) 或网络 (curl 拿到的 UTF-8), 无法先验, 只能检测。 */
static int is_valid_utf8(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int len; unsigned int cp;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else return 0;
        if (i + (size_t)len > n) return 0;
        for (int k = 1; k < len; k++) {
            if ((s[i + k] & 0xC0) != 0x80) return 0;
            cp = (cp << 6) | (unsigned)(s[i + k] & 0x3F);
        }
        if (len == 2 && cp < 0x80) return 0;          /* 超长编码 */
        if (len == 3 && cp < 0x800) return 0;
        if (len == 4 && cp < 0x10000) return 0;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        i += (size_t)len;
    }
    return 1;
}

static void execute_bash(const char *command) {
    /* 极简理念: 不拦截命令, 由用户自己承担运行环境的风险 (建议跑在容器中)。 */
    int n = run_pipe(command, tool_out, BUFSZ);
    if (n <= 0) { strcpy(tool_out, "(no output)"); return; }
    /* 输出已是合法 UTF-8 则原样保留, 否则才做 OEM(GBK) -> UTF-8 转换 */
    if (!is_valid_utf8((const unsigned char *)tool_out, (size_t)n))
        oem_to_utf8(tool_out, BUFSZ);
}

/* ===== 工作目录限制 ===== */

/* UTF-8 -> UTF-16, 写入 out (cap 为 wchar 数)。成功返回非 0。 */
static int utf8_to_wide(const char *u8, wchar_t *out, int cap) {
    return MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, cap) > 0;
}

/* 把截断长度回退到 UTF-8 字符边界: 避免切出半个字符 (非法 UTF-8 会让整段文本被丢弃) */
static size_t utf8_trim_len(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if (c >= 0xF0)      need = 4;
        else if (c >= 0xE0) need = 3;
        else if (c >= 0xC0) need = 2;
        else                need = 1;
        if (need > 1 && i + need > len) break;      /* 该字符被截断 -> 停 */
        int ok = 1;
        for (size_t k = 1; k < need; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) { ok = 0; break; }
        if (!ok) break;                             /* 非法序列 -> 停 */
        i += need;
    }
    return i;
}

/* UTF-8 路径打开文件: 必须走宽字符, 否则非 ASCII 路径 (中文目录) 会打不开 */
static FILE *fopen_utf8(const char *path, const char *mode) {
    wchar_t wp[MAX_PATH], wm[16];
    if (!utf8_to_wide(path, wp, MAX_PATH)) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (n <= 0 || n > 16) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, n);
    return _wfopen(wp, wm);
}

/* 取 exe 所在目录 (UTF-8, 无结尾反斜杠); 失败回退当前目录 */
static void get_exe_dir_utf8(char *out, size_t cap) {
    wchar_t wexe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, wexe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        wchar_t wcd[MAX_PATH];
        if (GetCurrentDirectoryW(MAX_PATH, wcd) == 0) { out[0] = '\0'; return; }
        WideCharToMultiByte(CP_UTF8, 0, wcd, -1, out, (int)cap, NULL, NULL);
        return;
    }
    wchar_t *slash = wcsrchr(wexe, L'\\');
    if (slash) *slash = L'\0';
    WideCharToMultiByte(CP_UTF8, 0, wexe, -1, out, (int)cap, NULL, NULL);
}

/* 确保 g_workspace 已初始化 (默认 exe 目录) */
static void ensure_workspace(void) {
    if (g_workspace[0]) return;
    get_exe_dir_utf8(g_workspace, sizeof(g_workspace));
}

/* 检查 path 规范化后是否在 g_workspace 内。返回 1 合法, 0 非法。 */
static int path_in_workspace(const char *path) {
    ensure_workspace();
    wchar_t wbase[MAX_PATH], wabs[MAX_PATH], wws[MAX_PATH], wwsfull[MAX_PATH];
    if (path[0] == '\\' || path[0] == '/' || (path[0] && path[1] == ':')) {
        if (!utf8_to_wide(path, wbase, MAX_PATH)) return 0;
    } else {
        char base[MAX_PATH];
        int m = snprintf(base, sizeof(base), "%s\\%s", g_workspace, path);
        if (m < 0 || (size_t)m >= sizeof(base)) return 0;
        if (!utf8_to_wide(base, wbase, MAX_PATH)) return 0;
    }
    DWORD n = GetFullPathNameW(wbase, MAX_PATH, wabs, NULL);
    if (n == 0 || n >= MAX_PATH) return 0;
    if (!utf8_to_wide(g_workspace, wws, MAX_PATH)) return 0;
    n = GetFullPathNameW(wws, MAX_PATH, wwsfull, NULL);
    if (n == 0 || n >= MAX_PATH) return 0;
    size_t wl = wcslen(wwsfull);
    if (wcsncmp(wabs, wwsfull, wl) != 0) return 0;
    if (wabs[wl] != L'\\' && wabs[wl] != L'\0') return 0;
    return 1;
}

/* 把 path 解析为工作目录内的实际路径: 相对路径在 g_workspace 下拼接,
 * 绝对路径 (盘符或 \ 开头) 原样使用。写入 out (cap 为 wchar 数)。返回 1 成功。
 * 这样文件工具的枚举/打开目标与沙箱校验一致, 避免 CWD≠工作目录时越界。 */
static int resolve_in_workspace(const char *path, wchar_t *out, int cap) {
    ensure_workspace();
    if (path[0] == '\\' || path[0] == '/' || (path[0] && path[1] == ':')) {
        return utf8_to_wide(path, out, cap);
    }
    char full[MAX_PATH];
    int m = snprintf(full, sizeof(full), "%s\\%s", g_workspace, path);
    if (m < 0 || (size_t)m >= sizeof(full)) return 0;
    return utf8_to_wide(full, out, cap);
}

/* ===== 结构化工具 (写入全局 tool_out) ===== */

static void tool_read_file(const char *path) {
    if (!path_in_workspace(path)) { snprintf(tool_out, BUFSZ, "(拒绝: 路径在工作目录外)"); return; }
    wchar_t wpath[MAX_PATH];
    if (!resolve_in_workspace(path, wpath, MAX_PATH)) { snprintf(tool_out, BUFSZ, "(读取失败: 路径过长)"); return; }
    FILE *f = _wfopen(wpath, L"rb");
    if (!f) { snprintf(tool_out, BUFSZ, "(读取失败: 无法打开 %s)", path); return; }
    size_t n = fread(tool_out, 1, BUFSZ - 64, f);
    n = utf8_trim_len(tool_out, n);          /* 截断对齐到字符边界, 避免半个字符 */
    fclose(f);
    tool_out[n] = '\0';
    if (n >= BUFSZ - 64) {
        strcat(tool_out, "\n(已截断, 文件过大)");
    }
}

static void tool_write_file(const char *path, const char *content) {
    if (!path_in_workspace(path)) { strcpy(tool_out, "(拒绝: 路径在工作目录外)"); return; }
    wchar_t wpath[MAX_PATH];
    if (!resolve_in_workspace(path, wpath, MAX_PATH)) { strcpy(tool_out, "(写入失败: 路径过长)"); return; }
    FILE *f = _wfopen(wpath, L"wb");
    if (!f) { strcpy(tool_out, "(写入失败)"); return; }
    size_t len = strlen(content);
    fwrite(content, 1, len, f);
    fclose(f);
    snprintf(tool_out, BUFSZ, "(已写入 %zu 字节)", len);
}

/* ===== Agent 循环 ===== */

typedef struct { char user_msg[BUFSZ]; } AgentTask;

#define SYSTEM_PROMPT \
    "{\"role\":\"system\",\"content\":\"你是 cagent,一个极简的编程 Agent。" \
    "你有三个工具: execute_bash(执行命令)、read_file(读文件)、write_file(写文件)。" \
    "写入或覆盖文件前,先 read_file 读取现有内容。" \
    "修改已有文件时,先读取再用 write_file 写入完整新内容(本 Agent 没有增量编辑工具)。" \
    "列目录用 execute_bash 跑 dir,递归搜索内容用 findstr /s /i 关键词 *.* 。" \
    "命令通过 cmd /c 执行,用 Windows 命令风格:不要 mkdir -p(直接 mkdir),运行当前程序不要 ./ 前缀。" \
    "文件工具仅限工作目录内,用相对路径。" \
    "任务不明确时,先向用户澄清。" \
    "任务完成后,停止并简要总结你做了什么。" \
    "始终用中文回答。回答简洁。\"}"

/* messages 缓冲水位线: 接近上限时整轮对话重置, 防止越界. */
#define MESSAGES_WATERMARK  ((BUFSZ * 3) / 4)

static const char *ROLLBACK_HINT = "\r\n(本轮已回滚, 不影响后续对话)\r\n";

/* 初始化 messages 为只含 system prompt 的状态。 */
static void reset_conversation(void) {
    snprintf(messages, BUFSZ, "%s", SYSTEM_PROMPT);
}

/* 前向声明: agent_turn 调用历史函数, 其定义在 config 区之后 */
static void history_save(void);

/* 执行一轮对话 (user_msg -> 直至最终回复或回滚)。线程无关, 可在主线程直接调用。
 * 用户消息的界面回显由前端负责 (核心不管渲染)。 */
static void agent_turn(const char *user_msg) {
    InterlockedExchange(&g_cancel, 0);          /* 清除取消标志 */

    /* 1. 若历史接近溢出, 重开一个新会话 (旧会话文件保留, 不删除) */
    if (strlen(messages) > MESSAGES_WATERMARK) {
        append_text("(对话历史过长, 已自动另起新会话)\r\n");
        reset_conversation();
        g_history_file[0] = '\0';
    }

    /* 2. 若首次发言, messages 还是空(没经过 config_load 之外的初始化) */
    if (messages[0] == '\0') reset_conversation();

    /* 3. 记录快照点: 出错时回滚到这里, 丢弃本轮 user message */
    size_t savepoint = strlen(messages);
    int rolled_back = 0;   /* 在 done 之前标记是否需要回滚 */

    /* 4. 追加本轮 user message */
    char *escaped = json_escape_alloc(user_msg);
    if (!escaped) {
        append_text("(内存不足, 已忽略本轮)\r\n");
        goto done;
    }
    int n = snprintf(messages + savepoint, BUFSZ - savepoint,
                     ",{\"role\":\"user\",\"content\":\"%s\"}", escaped);
    free(escaped);
    if (n <= 0 || (size_t)n >= BUFSZ - savepoint) {
        append_text("(输入过长, 已忽略本轮)\r\n");
        messages[savepoint] = '\0';
        goto done;
    }

    for (;;) {
        /* 取消检查点 1: 每轮迭代顶部 (LLM 调用前) */
        if (InterlockedCompareExchange(&g_cancel, 0, 0)) {
            append_text("(已取消)\r\n");
            rolled_back = 1;
            goto done;
        }
        snprintf(body, BUFSZ,
            "{\"model\":\"%s\",\"messages\":[%s],\"tools\":%s,\"stream\":true}",
            g_model, messages, TOOLS_JSON);

        append_text("(thinking...)\r\n");

        /* 流式 SSE 请求 */
        StreamCtx ctx;
        memset(&ctx, 0, sizeof(ctx));

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

        if (ctx.n_calls > 0) {
            /* 有 tool_calls: 从 ctx.calls 转 ToolCall 并执行 */
            typedef struct {
                char id[256]; char name[64]; char args[ARGS_MAX]; char *output;
            } ToolCall;
            ToolCall calls[8];
            int n_calls = 0;
            for (int i = 0; i < ctx.n_calls && n_calls < 8; i++) {
                if (!ctx.calls[i].name[0]) continue;
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

        /* 执行所有工具: arguments 是字符串化 JSON, 二次解析取 command */
        for (int i = 0; i < n_calls; i++) {
            /* 取消检查点 2: 每个工具执行前 */
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
                const char *p = json_as_str(json_obj_get(argsj, "path"));
                tool_read_file(p ? p : "");
            } else if (strcmp(c->name, "write_file") == 0) {
                const char *p = json_as_str(json_obj_get(argsj, "path"));
                const char *ct = json_as_str(json_obj_get(argsj, "content"));
                tool_write_file(p ? p : "", ct ? ct : "");
            } else {
                strcpy(tool_out, "(未知工具)");
            }
            json_free(argsj);
            c->output = strdup(tool_out);
            {
                size_t tl = strlen(tool_out);
                char line[BUFSZ + 32];
                if (tl > 0 && (tool_out[tl-1] == '\n' || tool_out[tl-1] == '\r'))
                    snprintf(line, sizeof(line), "[Output]\r\n%s", tool_out);
                else
                    snprintf(line, sizeof(line), "[Output]\r\n%s\r\n", tool_out);
                append_text(line);
            }
        }

        /* 第二遍: 写 assistant 消息 (含所有 tool_calls 数组) */
        size_t mstart = strlen(messages);
        int an = snprintf(messages + mstart, BUFSZ - mstart,
            ",{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[");
        int ok = (an > 0 && (size_t)an < BUFSZ - mstart);

        for (int i = 0; i < n_calls && ok; i++) {
            ToolCall *c = &calls[i];
            char *esc_id   = json_escape_alloc(c->id);
            char *esc_name = json_escape_alloc(c->name);
            char *esc_args = json_escape_alloc(c->args);
            if (!esc_id || !esc_name || !esc_args) { ok = 0; free(esc_id); free(esc_name); free(esc_args); break; }
            size_t len = strlen(messages);
            int n2 = snprintf(messages + len, BUFSZ - len,
                "%s{\"id\":\"%s\",\"type\":\"function\","
                "\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}",
                (i == 0 ? "" : ","), esc_id, esc_name, esc_args);
            free(esc_id); free(esc_name); free(esc_args);
            if (n2 <= 0 || (size_t)n2 >= BUFSZ - len) { ok = 0; }
        }

        if (ok) {
            size_t len = strlen(messages);
            int n2 = snprintf(messages + len, BUFSZ - len, "]}");
            if (n2 <= 0 || (size_t)n2 >= BUFSZ - len) ok = 0;
        }

        /* 第三遍: 为每个 tool_call 写 tool 消息 */
        for (int i = 0; i < n_calls && ok; i++) {
            ToolCall *c = &calls[i];
            char *esc_id  = json_escape_alloc(c->id);
            char *esc_out = json_escape_alloc(c->output ? c->output : "");
            if (!esc_id || !esc_out) { ok = 0; free(esc_id); free(esc_out); break; }
            size_t len = strlen(messages);
            int n2 = snprintf(messages + len, BUFSZ - len,
                ",{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"%s\"}",
                esc_id, esc_out);
            free(esc_id); free(esc_out);
            if (n2 <= 0 || (size_t)n2 >= BUFSZ - len) ok = 0;
        }

        /* 释放工具输出 */
        for (int i = 0; i < n_calls; i++) free(calls[i].output);

        if (!ok) {
            /* 任一步失败: 把本次拼接的 assistant+tool 段全部截掉, 回滚整轮 */
            messages[mstart] = '\0';
            append_text("(历史空间或内存不足, 本轮回滚)\r\n");
            rolled_back = 1;
            goto done;
        }
        } else {
            /* 无 tool_calls: content 收尾。流式已逐字显示, 这里只追加换行 + messages。 */
            if (ctx.content_len > 0) {
                /* 内容末尾已带换行则不再补, 避免双倍空行 */
                if (ctx.content_buf[ctx.content_len - 1] != '\n' &&
                    ctx.content_buf[ctx.content_len - 1] != '\r')
                    append_text("\r\n");
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
    }
done:
    if (rolled_back) {
        messages[savepoint] = '\0';
    }
    history_save();   /* 每轮 done 后保存 (含回滚后状态) */
}

/* 后台线程入口: 跑一轮后通过钩子通知前端。 */
static DWORD WINAPI agent_thread(LPVOID arg) {
    AgentTask *task = (AgentTask*)arg;
    agent_turn(task->user_msg);
    free(task);
    if (cagent_on_done) cagent_on_done();
    return 0;
}

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

/* 简单 key=value 解析器 */
static void config_load(void) {
    char path[MAX_PATH];
    get_ini_path(path, sizeof(path));
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == ';' || line[0] == '\n' || line[0] == '\r') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line, *val = eq + 1;

        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == '\n' || val[vlen-1] == '\r')) {
            val[--vlen] = '\0';
        }

        if (strcmp(key, "url_base") == 0)
            snprintf(g_api_url, sizeof(g_api_url), "%s", val);
        else if (strcmp(key, "api_key") == 0) {
            char *dec = dpapi_unprotect(val);
            if (dec) {
                snprintf(g_api_key, sizeof(g_api_key), "%s", dec);
                free(dec);
            } else {
                g_api_key[0] = '\0';
                g_key_decrypt_failed = 1;
            }
        }
        else if (strcmp(key, "model") == 0)
            snprintf(g_model, sizeof(g_model), "%s", val);
        else if (strcmp(key, "skip_cert_verify") == 0)
            g_skip_cert_verify = (atoi(val) != 0);
        else if (strcmp(key, "workspace") == 0)
            snprintf(g_workspace, sizeof(g_workspace), "%s", val);
        else if (strcmp(key, "last_session") == 0)
            snprintf(g_last_session, sizeof(g_last_session), "%s", val);
    }
    fclose(f);
}

/* 把当前全局配置写回 ini (若 cagent_read_config_ui 已设置, 先从中同步)。 */
static void config_save(void) {
    if (cagent_read_config_ui) cagent_read_config_ui();

    /* 三个全空就不写,避免覆盖出"空文件" */
    if (!g_api_url[0] && !g_api_key[0] && !g_model[0]) return;

    char path[MAX_PATH];
    get_ini_path(path, sizeof(path));
    FILE *f = fopen_utf8(path, "wb");
    if (!f) return;
    fprintf(f, "# cagent 配置 (UTF-8, 退出时自动保存)\r\n");
    fprintf(f, "url_base=%s\r\n", g_api_url);
    {
        char *enc = dpapi_protect(g_api_key);
        if (enc) { fprintf(f, "api_key=%s\r\n", enc); free(enc); }
    }
    fprintf(f, "model=%s\r\n",    g_model);
    fprintf(f, "skip_cert_verify=%d\r\n", g_skip_cert_verify);
    fprintf(f, "workspace=%s\r\n", g_workspace);
    fprintf(f, "last_session=%s\r\n", g_last_session);
    fclose(f);
}

#endif /* CAGENT_CORE_H */
