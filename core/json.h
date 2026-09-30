/*
 * core/json.h - 轻量 JSON 解析器 / 字符串转义 / 自测
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

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
