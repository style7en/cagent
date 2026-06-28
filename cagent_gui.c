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
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

/* 解析器自测: 返回 0 通过, 非 0 失败。由 WinMain --selftest 触发。 */
/* ===== DPAPI Key 加密 =====
 * 加密: 明文 -> "dpapi:<base64>"。解密: "dpapi:<base64>" 或明文 -> 明文。
 * 失败返回 NULL。返回值 malloc, 调用者 free。 */
static char *dpapi_protect(const char *plain);
static char *dpapi_unprotect(const char *stored);

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
#define MAX_ITERATIONS  20

#define ID_HISTORY  1001
#define ID_INPUT    1002
#define ID_SEND     1003
#define ID_CLEAR    1004
#define ID_CFG_BASE 1005   /* +0 url, +1 key, +2 model */
#define ID_LBL_BASE 1008   /* +0 url, +1 key, +2 model */

#define WM_APP_APPEND  (WM_APP + 1)   /* lParam = UTF-8 char* (须 free) */
#define WM_APP_DONE    (WM_APP + 2)   /* Agent 任务完成,启用 UI */

#define CFG_URL 0
#define CFG_KEY 1
#define CFG_MDL 2

/* ===== 全局状态 ===== */
static HWND g_hHistory, g_hInput, g_hSend, g_hClear;
static HWND g_hCfg[3];                 /* [url, key, model] */
static HFONT g_hFont;
static HANDLE g_hThread = NULL;
static volatile LONG g_running = 0;   /* 1 = Agent 工作线程运行中 */
static volatile LONG g_cancel  = 0;   /* 1 = 请求取消 */

/* 运行时配置(在 start_task 时从 Edit 控件同步) */
static char g_api_url[1024] = "";      /* 例: https://token.sensenova.cn/v1 */
static char g_api_key[512]  = "";
static char g_model[128]    = "";
static int g_key_decrypt_failed = 0;   /* DPAPI 解密失败标志, 启动后提示 */
static int g_skip_cert_verify = 0;     /* 1=跳过 SSL 证书校验 (自签端点用) */

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
    "\"required\":[\"path\",\"content\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"list_dir\","
    "\"description\":\"List directory entries (name, size, type)\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"search\","
    "\"description\":\"Search pattern in files under a directory (non-recursive)\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"pattern\",\"path\"]}}}]";

/* ===== 工具函数: UTF-8 <-> UTF-16 ===== */

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

/* ===== JSON / HTTP / 工具 ===== */

/* JSON 字符串转义: 必须覆盖所有 0x00-0x1F 控制字符, 否则服务端解析失败.
 * \r 直接丢弃 (Windows 换行多余的部分, 显示和 LLM 都不需要).
 * 有界写入: 至多写 dst_cap-1 字节 + '\0'. 返回 1 成功, 0 容量不足.
 * 最坏膨胀比 6x (每控制字符 -> \u00XX). */
static int json_escape(const char *src, char *dst, size_t dst_cap) {
    if (dst_cap == 0) return 0;
    size_t j = 0;
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        /* 预估本字符最多写入 6 字节 (\u00XX), 再留 1 字节给 '\0' */
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

/* 分配一块刚好够 src 转义后存放的 heap 缓冲并执行 escape.
 * 返回 NULL 表示分配失败. 调用者负责 free. */
static char *json_escape_alloc(const char *src) {
    size_t cap = strlen(src) * 6 + 1;
    char *buf = (char*)malloc(cap);
    if (!buf) return NULL;
    json_escape(src, buf, cap);   /* cap 足够, 不会失败 */
    return buf;
}

/* 用 CreateProcess + 匿名管道静默运行命令(仅本地工具用),纯内存收发数据。
 * 子进程的 stdout+stderr 合并写入 output (含 \0)。
 * 返回实际读到的字节数,失败返回 -1。 */
static int run_pipe(const char *cmdline, char *output, size_t out_cap) {
    HANDLE outR = NULL, outW = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

    if (!CreatePipe(&outR, &outW, &sa, 0)) return -1;
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outW;
    si.hStdError  = outW;

    char buf[16384];
    snprintf(buf, sizeof(buf), "cmd /c %s", cmdline);

    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(outR); CloseHandle(outW);
        return -1;
    }
    CloseHandle(outW);

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

/* 把 WinHTTP/系统错误码翻译为中文可读文本 (静态字符串, 勿 free)。 */
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

/* 把当前 GetLastError() 翻译后写入 out。 */
static void http_set_err(char *out, size_t cap) {
    DWORD e = GetLastError();
    const char *m = winhttp_err_msg(e);
    if (m) snprintf(out, cap, "[网络错误] %s", m);
    else snprintf(out, cap, "[网络错误] WinHTTP 错误 %lu", e);
}

/* ===== WinHTTP POST =====
 * 解析 url 得到 host/port/path/是否 https,然后 WinHTTP 发起请求。
 * 响应正文写入 out (含 \0),返回 HTTP 状态码,失败返回 -1。 */
/* ===== SSE 流式 ===== */

/* 流式累积的 tool_call (按 index 累积 delta 片段) */
typedef struct {
    char id[256];
    char name[64];
    char args[8192];   /* arguments 片段累积 */
} StreamToolCall;

/* 流式上下文: 跨 http_post_stream 传递 */
typedef struct {
    char content_buf[BUFSZ];   /* 累积完整 content 供 messages */
    size_t content_len;
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

/* SSE 流式 POST: 增量读取, content delta 回调, tool_calls delta 累积到 ctx。
 * 返回 HTTP 状态码; -2=取消; -1=网络错误 (err_out 写诊断). */
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

/* 把 cmd.exe 的 OEM/ANSI 输出转成 UTF-8.
 * cmd.exe 的 stdout 用当前控制台的 OEM 代码页 (中文系统 = CP936/GBK),
 * 直接当 UTF-8 处理会得到 ????. 用 GetACP() 拿到系统代码页, GBK→UTF-16→UTF-8.
 * 转换失败则原样保留(降级而非崩溃). */
static void oem_to_utf8(char *buf, size_t cap) {
    UINT cp = GetACP();
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

/* 危险命令模式表 (子串匹配, 宁可误报不漏报) */
static const char *DANGEROUS[] = {
    "rm", "del", "erase", "rmdir", "rd", "format", "shutdown",
    "taskkill", "reg delete", "diskpart", "mklink", "takeown", "icacls"
};

/* 检查命令是否危险 (子串匹配 DANGEROUS 表) */
static int is_dangerous(const char *command) {
    for (size_t i = 0; i < sizeof(DANGEROUS)/sizeof(DANGEROUS[0]); i++) {
        if (strstr(command, DANGEROUS[i])) return 1;
    }
    return 0;
}

static void execute_bash(const char *command) {
    int n = run_pipe(command, tool_out, BUFSZ);
    if (n <= 0) { strcpy(tool_out, "(no output)"); return; }
    oem_to_utf8(tool_out, BUFSZ);
}

/* ===== 结构化工具 (写入全局 tool_out) ===== */

static void tool_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(tool_out, BUFSZ, "(读取失败: 无法打开 %s)", path); return; }
    size_t n = fread(tool_out, 1, BUFSZ - 64, f);
    fclose(f);
    tool_out[n] = '\0';
    if (n >= BUFSZ - 64) {
        strcat(tool_out, "\n(已截断, 文件过大)");
    }
    /* 文件内容直接用 (假设 UTF-8); 不调 oem_to_utf8 (那是给 cmd 的 GBK 输出用的) */
}

static void tool_write_file(const char *path, const char *content) {
    /* 覆盖确认: 文件已存在则弹窗 (工作线程, 同命令沙箱) */
    FILE *test = fopen(path, "rb");
    if (test) {
        fclose(test);
        int rc = MessageBoxW(NULL, L"文件已存在, 确认覆盖?", L"write_file 确认",
                             MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (rc != IDYES) { strcpy(tool_out, "(用户拒绝覆盖)"); return; }
    }
    FILE *f = fopen(path, "wb");
    if (!f) { strcpy(tool_out, "(写入失败)"); return; }
    size_t len = strlen(content);
    fwrite(content, 1, len, f);
    fclose(f);
    snprintf(tool_out, BUFSZ, "(已写入 %zu 字节)", len);
}

static void tool_list_dir(const char *path) {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { snprintf(tool_out, BUFSZ, "(列目录失败: %s)", path); return; }
    size_t pos = 0;
    int count = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        const char *type = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "<DIR>" : "file";
        char line[512];
        snprintf(line, sizeof(line), "%s\t%llu\t%s\n", fd.cFileName,
                 (unsigned long long)fd.nFileSizeLow, type);
        size_t ln = strlen(line);
        if (pos + ln + 32 >= BUFSZ) { strcat(tool_out, "(更多条目已截断)"); break; }
        memcpy(tool_out + pos, line, ln);
        pos += ln;
        if (++count >= 200) { strcat(tool_out, "(更多条目已截断)"); break; }
    } while (FindNextFileA(h, &fd));
    tool_out[pos] = '\0';
    FindClose(h);
}

static void tool_search(const char *pattern, const char *path) {
    char glob[MAX_PATH];
    snprintf(glob, sizeof(glob), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) { snprintf(tool_out, BUFSZ, "(搜索失败: %s)", path); return; }
    size_t pos = 0;
    int matches = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;  /* 非递归: 跳过子目录 */
        char fpath[MAX_PATH];
        snprintf(fpath, sizeof(fpath), "%s\\%s", path, fd.cFileName);
        FILE *f = fopen(fpath, "rb");
        if (!f) continue;
        char *buf = (char*)malloc(256 * 1024);
        if (!buf) { fclose(f); continue; }
        size_t n = fread(buf, 1, 256 * 1024 - 1, f);
        fclose(f);
        if (n == 0) { free(buf); continue; }
        buf[n] = '\0';
        char *line = buf;
        while (line < buf + n && matches < 50) {
            char *eol = strchr(line, '\n');
            int linelen = eol ? (int)(eol - line) : (int)(buf + n - line);
            char save = line[linelen];
            line[linelen] = '\0';
            if (strstr(line, pattern)) {
                char ml[1024];
                snprintf(ml, sizeof(ml), "%s: %s\n", fd.cFileName, line);
                size_t mlen = strlen(ml);
                if (pos + mlen + 32 >= BUFSZ) {
                    pos += snprintf(tool_out + pos, BUFSZ - pos, "(更多匹配已截断)");
                    line[linelen] = save;
                    free(buf);
                    goto done;
                }
                memcpy(tool_out + pos, ml, mlen);
                pos += mlen;
                matches++;
            }
            line[linelen] = save;
            if (!eol) break;
            line = eol + 1;
        }
        free(buf);
        if (matches >= 50) { pos += snprintf(tool_out + pos, BUFSZ - pos, "(更多匹配已截断)"); break; }
    } while (FindNextFileA(h, &fd));
done:
    tool_out[pos] = '\0';
    FindClose(h);
    if (matches == 0 && pos == 0) strcpy(tool_out, "(无匹配)");
}

/* ===== Agent 工作线程 ===== */

typedef struct { char user_msg[BUFSZ]; } AgentTask;

#define SYSTEM_PROMPT \
    "{\"role\":\"system\",\"content\":\"你是 cagent,一个由 C 语言实现的极简 AI Agent。" \
    "请始终使用中文回答。需要时调用工具。回答简洁。\"}"

/* messages 缓冲水位线: 接近上限时整轮对话重置, 防止越界. */
#define MESSAGES_WATERMARK  ((BUFSZ * 3) / 4)

static const char *ROLLBACK_HINT = "\r\n(本轮已回滚, 不影响后续对话)\r\n";

/* 初始化 messages 为只含 system prompt 的状态. 在程序启动和 "清空对话" 时调用. */
static void reset_conversation(void) {
    snprintf(messages, BUFSZ, "%s", SYSTEM_PROMPT);
}

/* 前向声明: agent_thread 调用历史函数, 其定义在 config 区之后 */
static void get_history_path(char *out, size_t cap);
static void history_save(void);
static int  history_load(void);

static DWORD WINAPI agent_thread(LPVOID arg) {
    AgentTask *task = (AgentTask*)arg;

    /* 显示用户消息 */
    {
        char line[BUFSZ + 16];
        snprintf(line, sizeof(line), "\r\n>>> %s\r\n\r\n", task->user_msg);
        append_text(line);
    }

    /* 1. 若历史接近溢出, 整体重置(并提示用户) */
    if (strlen(messages) > MESSAGES_WATERMARK) {
        append_text("(对话历史过长, 已自动清空上下文)\r\n");
        reset_conversation();
        char hpath[MAX_PATH];
        get_history_path(hpath, sizeof(hpath));
        DeleteFileA(hpath);
    }

    /* 2. 若首次发言, messages 还是空(没经过 config_load 之外的初始化) */
    if (messages[0] == '\0') reset_conversation();

    /* 3. 记录快照点: 出错时回滚到这里, 丢弃本轮 user message */
    size_t savepoint = strlen(messages);
    int rolled_back = 0;   /* 在 done 之前标记是否需要回滚 */

    /* 4. 追加本轮 user message */
    char *escaped = json_escape_alloc(task->user_msg);
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

    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
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
                char id[256]; char name[64]; char args[4096]; char *output;
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
                const char *ccmd = cmd ? cmd : "";
                if (is_dangerous(ccmd)) {
                    /* 高危命令确认: 只提示是否执行, 不显示具体命令 (避免信息不全/截断) */
                    if (MessageBoxW(NULL, L"模型请求执行高危命令, 是否执行?",
                                    L"高危命令确认",
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                        strcpy(tool_out, "(用户拒绝执行)");
                    } else {
                        execute_bash(ccmd);
                    }
                } else {
                    execute_bash(ccmd);
                }
            } else if (strcmp(c->name, "read_file") == 0) {
                const char *p = json_as_str(json_obj_get(argsj, "path"));
                tool_read_file(p ? p : "");
            } else if (strcmp(c->name, "write_file") == 0) {
                const char *p = json_as_str(json_obj_get(argsj, "path"));
                const char *ct = json_as_str(json_obj_get(argsj, "content"));
                tool_write_file(p ? p : "", ct ? ct : "");
            } else if (strcmp(c->name, "list_dir") == 0) {
                const char *p = json_as_str(json_obj_get(argsj, "path"));
                tool_list_dir(p ? p : "");
            } else if (strcmp(c->name, "search") == 0) {
                const char *pat = json_as_str(json_obj_get(argsj, "pattern"));
                const char *p = json_as_str(json_obj_get(argsj, "path"));
                tool_search(pat ? pat : "", p ? p : "");
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
    append_text("(max iterations reached)\r\n");
    /* 达到上限不算失败: 此前已多次成功调用, 保留历史. */

done:
    if (rolled_back) {
        messages[savepoint] = '\0';
    }
    history_save();   /* 每轮 done 后保存 (含回滚后状态) */
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

/* 取 exe 同目录下的 cagent_history.json 绝对路径 */
static void get_history_path(char *out, size_t cap) {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { snprintf(out, cap, "cagent_history.json"); return; }
    char *slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = '\0';
    else exe[0] = '\0';
    snprintf(out, cap, "%scagent_history.json", exe);
}

/* 把 messages 写入历史文件 (失败静默) */
static void history_save(void) {
    char path[MAX_PATH];
    get_history_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(messages, f);
    fclose(f);
}

/* 读历史文件到 messages, 成功返回 1, 失败/不存在返回 0 */
static int history_load(void) {
    char path[MAX_PATH];
    get_history_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(messages, 1, BUFSZ - 1, f);
    fclose(f);
    if (n == 0) return 0;
    messages[n] = '\0';
    return 1;
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
        else if (strcmp(key, "api_key") == 0) {
            char *dec = dpapi_unprotect(val);
            if (dec) {
                snprintf(g_api_key, sizeof(g_api_key), "%s", dec);
                free(dec);
            } else {
                /* 解密失败: 置空并标记, 启动后提示用户重填 */
                g_api_key[0] = '\0';
                g_key_decrypt_failed = 1;
            }
        }
        else if (strcmp(key, "model") == 0)
            snprintf(g_model, sizeof(g_model), "%s", val);
        else if (strcmp(key, "skip_cert_verify") == 0)
            g_skip_cert_verify = (atoi(val) != 0);
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
    {
        char *enc = dpapi_protect(g_api_key);
        if (enc) { fprintf(f, "api_key=%s\r\n", enc); free(enc); }
        else fprintf(f, "api_key=%s\r\n", g_api_key);   /* 加密失败降级明文 */
    }
    fprintf(f, "model=%s\r\n",    g_model);
    fprintf(f, "skip_cert_verify=%d\r\n", g_skip_cert_verify);
    fclose(f);
}

static void start_task(HWND hwnd) {
    /* 防御性: 若仍有旧线程未回收, 等待其结束再关闭 (正常路径不会走到, 因按钮状态已挡) */
    if (g_hThread) {
        WaitForSingleObject(g_hThread, INFINITE);
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
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, task->user_msg, BUFSZ, NULL, NULL);
    free(wbuf);

    SetWindowTextW(g_hInput, L"");

    InterlockedExchange(&g_cancel, 0);          /* 清除取消标志 */
    EnableWindow(g_hInput, FALSE);              /* 输入框禁用 */
    EnableWindow(g_hClear, FALSE);              /* 清空按钮禁用 */
    SetWindowTextW(g_hSend, L"停止");           /* 发送按钮变停止, 保持启用 (点击即取消) */

    InterlockedExchange(&g_running, 1);
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
    int clear_w = 80;
    int input_h = 72;           /* 多行输入框, 约 3 行高 (Enter 发送, Shift+Enter 换行) */
    int input_y = H - input_h - gap;
    int input_w = W - btn_w - clear_w - gap * 4;

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
    MoveWindow(g_hClear, gap * 2 + input_w, input_y, clear_w, input_h, TRUE);
    MoveWindow(g_hSend,  gap * 3 + input_w + clear_w, input_y, btn_w, input_h, TRUE);
}

/* 输入框子类化: Enter 发送, Shift+Enter 插入换行 */
static WNDPROC g_oldInputProc;
static LRESULT CALLBACK InputProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            /* Shift+Enter: 走默认处理插入换行 */
            return CallWindowProc(g_oldInputProc, h, msg, wp, lp);
        }
        SendMessage(GetParent(h), WM_COMMAND, MAKEWPARAM(ID_SEND, BN_CLICKED), 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) {
        /* 仅在没按 Shift 时抑制 (否则 Shift+Enter 会被吞,听不到 beep 也没换行) */
        if (!(GetKeyState(VK_SHIFT) & 0x8000)) return 0;
    }
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
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_INPUT, NULL, NULL);
        SendMessage(g_hInput, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        /* 子类化输入框,捕获 Enter */
        g_oldInputProc = (WNDPROC)SetWindowLongPtr(g_hInput, GWLP_WNDPROC, (LONG_PTR)InputProc);

        g_hSend = CreateWindowW(L"BUTTON", L"发送",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_SEND, NULL, NULL);
        SendMessage(g_hSend, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hClear = CreateWindowW(L"BUTTON", L"清空对话",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_CLEAR, NULL, NULL);
        SendMessage(g_hClear, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        /* 加载历史对话; 失败则初始化为只含 system prompt */
        if (!history_load()) reset_conversation();
        else append_text("(已恢复历史对话, 可继续)\r\n");

        /* DPAPI 解密失败提示 (config_load 在历史框创建前执行, 延迟到此处显示) */
        if (g_key_decrypt_failed) {
            append_text("API Key 解密失败 (可能换了用户/机器), 请重新填写 Key。\r\n");
            g_key_decrypt_failed = 0;
        }

        SetFocus(g_hInput);
        return 0;
    }

    case WM_SIZE:
        layout(hwnd);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_SEND && HIWORD(wp) == BN_CLICKED) {
            if (g_running) {
                /* 任务运行中: 点击=请求取消 */
                InterlockedExchange(&g_cancel, 1);
                EnableWindow(g_hSend, FALSE);   /* 防重复点, 等 DONE 恢复 */
                append_text("(正在停止...)\r\n");
            } else {
                start_task(hwnd);
            }
            return 0;
        }
        if (LOWORD(wp) == ID_CLEAR && HIWORD(wp) == BN_CLICKED) {
            if (!g_running) {  /* 任务运行中不允许清空 */
                reset_conversation();
                char hpath[MAX_PATH];
                get_history_path(hpath, sizeof(hpath));
                DeleteFileA(hpath);   /* 同步删除历史文件 */
                SetWindowTextW(g_hHistory, L"");
                SetFocus(g_hInput);
            }
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
        InterlockedExchange(&g_running, 0);
        InterlockedExchange(&g_cancel, 0);
        SetWindowTextW(g_hSend, L"发送");       /* 恢复按钮文本 */
        EnableWindow(g_hSend, TRUE);
        EnableWindow(g_hInput, TRUE);
        EnableWindow(g_hClear, TRUE);
        SetFocus(g_hInput);
        return 0;
    }
    /* 修复多行 EDIT 滚动后字形重叠 / 残留:
     * Win32 EDIT 滚动时仅重绘新出现的行, 用自定义字体 + 中文 / 高 DPI 时
     * 行高轻微错位会让旧字形未被擦除. 拦截所有可能引发滚动的消息,
     * 在默认处理之后强制整个控件区域重绘. */
    if (msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL ||
        msg == WM_KEYDOWN || msg == WM_KEYUP) {
        LRESULT r = CallWindowProc(g_oldHistoryProc, h, msg, wp, lp);
        InvalidateRect(h, NULL, TRUE);
        UpdateWindow(h);
        return r;
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
    (void)hPrev;
    /* 隐藏自测入口: 命令行含 --selftest 则跑解析器测试后退出。
     * GUI 子系统无控制台, printf 不可见; 把 stdout 重定向到 selftest.txt 便于读取。 */
    if (cmd && strstr(cmd, "--selftest")) {
        freopen("selftest.txt", "w", stdout);
        return json_selftest();
    }
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
