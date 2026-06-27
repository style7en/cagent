# cagent GUI 阶段一优化 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 cagent_gui 的解析与并发地基做扎实(手写 JSON 解析器、协作式取消、WinHTTP 错误诊断),并删除 mini 版。

**Architecture:** 保持单文件 + 零第三方依赖。引入自写递归下降 JSON 解析器替换全部 strstr;并发用 `volatile LONG g_running/g_cancel` + `Interlocked*` 做协作式取消;HTTP 失败用 `GetLastError` + 映射表翻译。解析器以 `--selftest` 做 TDD,其余部分靠手动测试矩阵。

**Tech Stack:** C(MinGW-w64, GCC 13+)、Win32 API、WinHTTP、单文件 `cagent_gui.c`。

**对应 spec:** `docs/superpowers/specs/2026-06-27-cagent-gui-phase1-design.md`

---

## 文件结构

- **Modify** `cagent_gui.c` —— 主体:新增 JSON 解析器、重写 agent_thread 响应解析、并发取消、HTTP 诊断、--selftest 入口
- **Modify** `Makefile` —— 移除 mini 构建规则与 clean 产物
- **Modify** `README.md` —— 移除 mini 章节、统一 MAX_ITERATIONS=20、补充取消/诊断特性
- **Delete** `cagent_mini.c`、`cagent_mini.exe`

---

## 步骤 1:删除 mini + 解析器替换

### Task 1: 删除 mini 版与构建/文档同步

**Files:**
- Delete: `cagent_mini.c`、`cagent_mini.exe`
- Modify: `Makefile`
- Modify: `README.md`

- [ ] **Step 1: 删除 mini 文件**

```bash
rm -f cagent_mini.c cagent_mini.exe
```

- [ ] **Step 2: 改 Makefile —— 移除 mini 规则**

把 `Makefile` 整体替换为:

```makefile
CC = gcc
# -Os : 优化代码体积 (相对 -O2 牺牲极小性能, I/O bound 程序无感)
# -s  : 链接后 strip 全部符号
CFLAGS  = -Wall -Wextra -Os
LDFLAGS = -s

GUI_TARGET = cagent_gui.exe
GUI_SRC = cagent_gui.c
GUI_LDFLAGS = -mwindows -lcomctl32 -lwinhttp

.PHONY: all clean

all: $(GUI_TARGET)

$(GUI_TARGET): $(GUI_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(GUI_LDFLAGS)

clean:
	@rm -rf $(GUI_TARGET)
```

- [ ] **Step 3: 改 README.md —— 移除 mini 章节、统一 MAX_ITERATIONS、更新定位**

对 `README.md` 做以下修改(用 Edit 工具逐处替换):

1. 开篇定位:把
   ```
   用 C 语言实现的极简 AI Agent,**单文件 + 零第三方依赖**,运行在 Windows 平台。包含一个教学版 CLI 和一个 Win32 GUI 版,核心思想都是同一段 Agent 循环。
   ```
   改为:
   ```
   用 C 语言实现的极简 AI Agent,**单文件 + 零第三方依赖**,运行在 Windows 平台。Win32 GUI 版,核心是同一段 Agent 循环。
   ```

2. 目录结构:把整块
   ```
   cagent/
   ├── Makefile            # 构建脚本
   ├── cagent.ini          # GUI 版配置文件 (首次启动后自动生成)
   ├── cagent_mini.c       # 教学版 CLI (~200 行)
   ├── cagent_mini.exe     # 编译产物
   ├── cagent_gui.c        # Win32 GUI 版 (~750 行)
   └── cagent_gui.exe      # 编译产物
   ```
   改为:
   ```
   cagent/
   ├── Makefile            # 构建脚本
   ├── cagent.ini          # 配置文件 (首次启动后自动生成)
   ├── cagent_gui.c        # Win32 GUI 版 (~1300 行)
   └── cagent_gui.exe      # 编译产物
   ```

3. 删除"单独构建"中 `make cagent_mini.exe` 一行,只保留 `make cagent_gui.exe`。

4. 删除整个 `## 教学版 CLI: cagent_mini.exe` 章节(从该标题到下一个 `---` 之前,含使用方式、依赖、工具小节)。

5. "工作原理"节:把
   ```
   GUI 版 `cagent_gui.c` 的 `agent_thread` 和 mini 版 `cagent_mini.c` 的 `main` 都是这个循环的实现。
   ```
   改为:
   ```
   `cagent_gui.c` 的 `agent_thread` 就是这个循环的实现。
   ```

6. 常见问题 "工具调用反复执行不停止" 答案中 `MAX_ITERATIONS=5` 改为 `MAX_ITERATIONS=20`。

- [ ] **Step 4: 验证构建**

Run: `make clean && make`
Expected: 编译成功,无 `-Wall -Wextra` 警告,生成 `cagent_gui.exe`。

- [ ] **Step 5: 提交**

```bash
git add Makefile README.md cagent_mini.c cagent_mini.exe
git commit -m "chore: 删除 mini 版, 同步 Makefile 与 README"
```

---

### Task 2: 新增 JSON 解析器数据结构与 selftest 测试(TDD 红)

**Files:**
- Modify: `cagent_gui.c`(在 `#include` 区之后、`#define BUFSZ` 之前插入解析器代码;WinMain 加 `--selftest` 入口)

- [ ] **Step 1: 插入解析器数据结构、前向声明与 API 声明**

在 `cagent_gui.c` 的 `#include <stdlib.h>` 之后、`#define BUFSZ` 之前插入:

```c
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
static void   json_free(JValue *v);
JValue       *json_parse(const char *text);
const JValue *json_obj_get(const JValue *obj, const char *key);
const JValue *json_arr_at(const JValue *arr, size_t i);
const char   *json_as_str(const JValue *v);
```

- [ ] **Step 2: 插入 selftest 测试函数**

在 Task 2 Step 1 插入的代码块之后继续插入(数据结构之后):

```c
/* 解析器自测: 返回 0 通过, 非 0 失败。由 WinMain --selftest 触发。 */
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

    if (fails == 0) printf("json_selftest: OK\n");
    else printf("json_selftest: %d FAIL(s)\n", fails);
    return fails ? 1 : 0;
    #undef CHK
}
```

- [ ] **Step 3: 插入解析器桩实现(让 selftest 可编译并失败)**

在 selftest 函数之后插入(桩:全部返回 NULL/空,使 selftest 必然失败):

```c
/* —— 桩实现 (Task 3 替换为真实实现) —— */
JValue *json_parse(const char *text) { (void)text; return NULL; }
const JValue *json_obj_get(const JValue *obj, const char *key) { (void)obj; (void)key; return NULL; }
const JValue *json_arr_at(const JValue *arr, size_t i) { (void)arr; (void)i; return NULL; }
const char *json_as_str(const JValue *v) { (void)v; return NULL; }
void json_free(JValue *v) { (void)v; }
```

注意:桩与 selftest 里调用的签名必须完全一致。`json_obj_get`/`json_arr_at`/`json_as_str` 返回 `const JValue*`/`const char*`;`json_parse`/`json_free` 接受 `const char*`/`JValue*`。

- [ ] **Step 4: WinMain 加 --selftest 入口**

定位 `WinMain`(文件末尾),把
```c
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev; (void)cmd;
    enable_dpi_awareness();   /* 必须在创建任何窗口之前调用 */
```
改为:
```c
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show) {
    (void)hPrev;
    /* 隐藏自测入口: 命令行含 --selftest 则跑解析器测试后退出 */
    if (cmd && strstr(cmd, "--selftest")) {
        return json_selftest();
    }
    enable_dpi_awareness();   /* 必须在创建任何窗口之前调用 */
```

- [ ] **Step 5: 验证编译**

Run: `make`
Expected: 编译成功,零警告。

- [ ] **Step 6: 运行 selftest 验证失败(TDD 红)**

Run: `./cagent_gui.exe --selftest`
Expected: 输出多个 `FAIL: ...` 行,最后一行 `json_selftest: N FAIL(s)`,退出码非 0。证明测试有效(桩返回 NULL 导致全部断言失败)。

- [ ] **Step 7: 暂不提交**(下一步实现真实解析器)

---

### Task 3: 实现 JSON 解析器(TDD 绿)

**Files:**
- Modify: `cagent_gui.c`(替换 Task 2 Step 3 的桩实现)

- [ ] **Step 1: 用真实实现替换桩**

删除 Task 2 Step 3 插入的整段桩实现,替换为下面的完整实现:

```c
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
```

注意:`memcpy` 需要 `<string.h>`(已 include);`strtod`/`strtoul` 需要 `<stdlib.h>`(已 include);`printf` 需要 `<stdio.h>`(已 include)。

- [ ] **Step 2: 验证编译**

Run: `make`
Expected: 零警告。

- [ ] **Step 3: 运行 selftest 验证通过(TDD 绿)**

Run: `./cagent_gui.exe --selftest`
Expected: 输出 `json_selftest: OK`,退出码 0。

- [ ] **Step 4: 提交**

```bash
git add cagent_gui.c
git commit -m "feat: 新增零依赖 JSON 解析器与 --selftest 自测"
```

---

### Task 4: 用解析器重写 agent_thread 响应解析,移除 extract_string

**Files:**
- Modify: `cagent_gui.c`(`agent_thread` 的 content 分支与 tool_calls 分支;删除 `extract_string` 函数)

- [ ] **Step 1: 重写 content 分支(无工具调用时)**

定位 `agent_thread` 中如下代码块(content 分支):
```c
        if (!strstr(resp, "\"tool_calls\"")) {
            char content[BUFSZ];
            if (extract_string(resp, "content", content, sizeof(content))) {
                append_text(content);
                append_text("\r\n");

                /* 成功: 把 assistant 最终回复也追加进历史
                 * (若拼接溢出会得到截断的非法 JSON, 必须回滚) */
                char *esc_content = json_escape_alloc(content);
                if (esc_content) {
                    size_t len = strlen(messages);
                    int an = snprintf(messages + len, BUFSZ - len,
                        ",{\"role\":\"assistant\",\"content\":\"%s\"}", esc_content);
                    free(esc_content);
                    if (an <= 0 || (size_t)an >= BUFSZ - len) {
                        append_text("(历史空间不足, 本轮回滚)\r\n");
                        rolled_back = 1;
                    }
                }
            } else {
                /* 响应不含 content 也不含 tool_calls: 视为解析失败, 回滚 */
                append_text("(响应解析失败)\r\n");
                append_text(resp);
                append_text(ROLLBACK_HINT);
                rolled_back = 1;
            }
            goto done;
        }
```

替换为:
```c
        /* 无工具调用: 用解析器取 choices[0].message.content */
        {
            JValue *root = json_parse(resp);
            const char *content = NULL;
            if (root) {
                const JValue *choices = json_obj_get(root, "choices");
                const JValue *msg = json_obj_get(json_arr_at(choices, 0), "message");
                content = json_as_str(json_obj_get(msg, "content"));
            }
            if (content) {
                append_text(content);
                append_text("\r\n");

                /* 成功: 把 assistant 最终回复追加进历史 (溢出则回滚) */
                char *esc_content = json_escape_alloc(content);
                if (esc_content) {
                    size_t len = strlen(messages);
                    int an = snprintf(messages + len, BUFSZ - len,
                        ",{\"role\":\"assistant\",\"content\":\"%s\"}", esc_content);
                    free(esc_content);
                    if (an <= 0 || (size_t)an >= BUFSZ - len) {
                        append_text("(历史空间不足, 本轮回滚)\r\n");
                        rolled_back = 1;
                    }
                }
                json_free(root);
            } else {
                /* 响应解析失败 (root==NULL 或 content 缺失/为 null): 回滚 */
                append_text("(响应解析失败)\r\n");
                append_text(resp);
                append_text(ROLLBACK_HINT);
                rolled_back = 1;
                json_free(root);
                goto done;
            }
            goto done;
        }
```

说明:`content` 为 JSON `null` 时 `json_as_str` 返回 NULL → 走失败分支,符合预期(content 不应为 null 时收尾)。

- [ ] **Step 2: 重写 tool_calls 分支(解析 + 执行)**

定位 `agent_thread` 中如下代码块(从 `/* === 工具调用分支 ===` 到 `执行所有工具` 循环结束 `}` 之前,即解析与执行部分;**不包含**其后的"第二遍写 assistant 消息"与"第三遍写 tool 消息"):

```c
        /* === 工具调用分支 ===
         * 解析所有 tool_calls (OpenAI 协议支持并行调用), 全部执行, 然后:
         *   1) 写一条 assistant 消息, 其 tool_calls 数组包含全部调用
         *   2) 为每个调用写一条 tool 消息
         * 必须 1:1 配对, 否则下一轮服务端会以 invalid_arguments 报错. */
        const char *tc_section = strstr(resp, "\"tool_calls\"");

        /* 第一遍: 解析 + 执行, 收集结果 */
        typedef struct {
            char id[256];
            char name[64];
            char args[4096];
            char *output;   /* heap, 末尾 free */
        } ToolCall;
        ToolCall calls[8];   /* 单轮最多 8 个并行调用, 足够使用 */
        int n_calls = 0;

        /* 收集所有 tool_calls. 每个 tool_call 以 "id": 起头, 顺序扫描.
         * extract_string 取第一次匹配, 在 tool_call 子串起点上抽 id/name/arguments
         * 自然能命中本调用的字段. */
        const char *cursor = tc_section;
        while (n_calls < (int)(sizeof(calls)/sizeof(calls[0]))) {
            const char *p = strstr(cursor, "\"id\":");
            if (!p) break;
            ToolCall *c = &calls[n_calls++];
            c->id[0] = c->name[0] = c->args[0] = '\0';
            c->output = NULL;
            extract_string(p, "id", c->id, sizeof(c->id));
            extract_string(p, "name", c->name, sizeof(c->name));
            extract_string(p, "arguments", c->args, sizeof(c->args));
            cursor = p + 5;   /* 跳过本次 "id": 防止死循环 */
        }

        if (n_calls == 0) {
            append_text("(tool_calls 解析失败)\r\n");
            append_text(resp);
            append_text(ROLLBACK_HINT);
            rolled_back = 1;
            goto done;
        }

        /* 执行所有工具 */
        for (int i = 0; i < n_calls; i++) {
            ToolCall *c = &calls[i];
            {
                char line[8192];
                snprintf(line, sizeof(line), "[Tool] %s(%s)\r\n", c->name, c->args);
                append_text(line);
            }
            char command[4096] = "";
            extract_string(c->args, "command", command, sizeof(command));
            execute_bash(command);
            c->output = strdup(tool_out);
            {
                char line[BUFSZ + 32];
                snprintf(line, sizeof(line), "[Output]\r\n%s\r\n", tool_out);
                append_text(line);
            }
        }
```

替换为:
```c
        /* === 工具调用分支 ===
         * 用解析器取 choices[0].message.tool_calls[] (支持并行调用), 全部执行,
         * 然后: 1) 写一条 assistant 消息含全部 tool_calls; 2) 每个调用写一条 tool 消息.
         * 必须 1:1 配对, 否则服务端报 invalid_arguments. */
        JValue *root = json_parse(resp);
        if (!root) {
            append_text("(响应解析失败)\r\n");
            append_text(resp);
            append_text(ROLLBACK_HINT);
            rolled_back = 1;
            goto done;
        }
        const JValue *choices = json_obj_get(root, "choices");
        const JValue *msg = json_obj_get(json_arr_at(choices, 0), "message");
        const JValue *tcs = json_obj_get(msg, "tool_calls");
        if (!tcs || tcs->type != J_ARR || tcs->arr.n == 0) {
            append_text("(tool_calls 解析失败)\r\n");
            append_text(resp);
            append_text(ROLLBACK_HINT);
            rolled_back = 1;
            json_free(root);
            goto done;
        }

        typedef struct {
            char id[256];
            char name[64];
            char args[4096];
            char *output;   /* heap, 末尾 free */
        } ToolCall;
        ToolCall calls[8];   /* 单轮最多 8 个并行调用 */
        int n_calls = 0;
        for (size_t i = 0; i < tcs->arr.n && n_calls < (int)(sizeof(calls)/sizeof(calls[0])); i++) {
            const JValue *tc = tcs->arr.items[i];
            const char *id = json_as_str(json_obj_get(tc, "id"));
            const JValue *fn = json_obj_get(tc, "function");
            const char *name = json_as_str(json_obj_get(fn, "name"));
            const char *args = json_as_str(json_obj_get(fn, "arguments"));
            ToolCall *c = &calls[n_calls++];
            c->id[0] = c->name[0] = c->args[0] = '\0';
            c->output = NULL;
            if (id)   snprintf(c->id,   sizeof(c->id),   "%s", id);
            if (name) snprintf(c->name, sizeof(c->name), "%s", name);
            if (args) snprintf(c->args, sizeof(c->args), "%s", args);
        }
        /* 解析结果已拷贝进 calls, root 可释放 */
        json_free(root);

        if (n_calls == 0) {
            append_text("(tool_calls 解析失败)\r\n");
            append_text(resp);
            append_text(ROLLBACK_HINT);
            rolled_back = 1;
            goto done;
        }

        /* 执行所有工具: arguments 是字符串化 JSON, 二次解析取 command */
        for (int i = 0; i < n_calls; i++) {
            ToolCall *c = &calls[i];
            {
                char line[8192];
                snprintf(line, sizeof(line), "[Tool] %s(%s)\r\n", c->name, c->args);
                append_text(line);
            }
            char command[4096] = "";
            JValue *argsj = json_parse(c->args);
            if (argsj) {
                const char *cmd = json_as_str(json_obj_get(argsj, "command"));
                if (cmd) snprintf(command, sizeof(command), "%s", cmd);
                json_free(argsj);
            }
            execute_bash(command);
            c->output = strdup(tool_out);
            {
                char line[BUFSZ + 32];
                snprintf(line, sizeof(line), "[Output]\r\n%s\r\n", tool_out);
                append_text(line);
            }
        }
```

注意:替换范围到"执行所有工具"循环结束为止。其后紧接着的"第二遍:写 assistant 消息"与"第三遍:写 tool 消息"代码(原文件中 `/* 第二遍: 写 assistant 消息 (含所有 tool_calls 数组) */` 起始的那段)**保持不变**,继续使用 `calls` 数组。

- [ ] **Step 3: 删除已无引用的 extract_string 函数**

删除 `cagent_gui.c` 中整个 `extract_string` 函数(原文件 161-198 行的 `/* 从 JSON 文本里抽出 "key":"value" 的 value...` 注释起,到该函数闭合 `}` 止)。

删除前先确认无残留引用:Run `grep -n "extract_string" cagent_gui.c`,Expected: 仅剩函数定义本身那一处(或完全无匹配)。若仍有调用点,回 Step 1/2 检查遗漏。

- [ ] **Step 4: 验证编译**

Run: `make`
Expected: 零警告。

- [ ] **Step 5: 运行 selftest 回归**

Run: `./cagent_gui.exe --selftest`
Expected: `json_selftest: OK`,退出码 0。

- [ ] **Step 6: 手动测试矩阵 1–4、7、8**

启动 `./cagent_gui.exe`(填入可用 API 配置),逐项验证:

| # | 操作 | 预期 |
|---|---|---|
| 1 | 发送"你好"(无工具) | 正常单轮回复 |
| 2 | 发送"用 execute_bash 执行 echo hi" | 显示 `[Tool]`/`[Output]`,最终回复 |
| 3 | 发送需要并行工具的任务(如"同时执行 echo a 和 echo b") | 两个 `[Tool]` 各自 id,1:1 配对无错乱 |
| 4 | 发送需多轮工具的任务 | 连续调用后正常收尾 |
| 7 | 长对话反复发送 | 触发"对话历史过长, 已自动清空上下文" |
| 8 | 改配置→关闭→重启 | 配置正确读取 |

任一项不符:回对应 Step 排查,不要进入下一步。

- [ ] **Step 7: 提交**

```bash
git add cagent_gui.c
git commit -m "refactor: agent_thread 改用 JSON 解析器, 移除 extract_string"
```

---

## 步骤 2:并发取消 + HTTP/SSL 诊断

### Task 5: 引入运行/取消状态与线程管理修复

**Files:**
- Modify: `cagent_gui.c`(全局状态、`start_task`、`WM_APP_DONE`、`WM_COMMAND`)

- [ ] **Step 1: 新增 g_running / g_cancel 全局**

定位全局状态区(`static HANDLE g_hThread = NULL;` 一行),在其后新增:
```c
static volatile LONG g_running = 0;   /* 1 = Agent 工作线程运行中 */
static volatile LONG g_cancel  = 0;   /* 1 = 请求取消 */
```

- [ ] **Step 2: 改 start_task —— 修复线程管理 + 置运行态 + 按钮变停止**

定位 `start_task`,把
```c
static void start_task(HWND hwnd) {
    if (g_hThread) {
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }

    int wlen = GetWindowTextLengthW(g_hInput);
    if (wlen <= 0) return;
```
改为:
```c
static void start_task(HWND hwnd) {
    /* 防御性: 若仍有旧线程未回收, 等待其结束再关闭 (正常路径不会走到, 因按钮状态已挡) */
    if (g_hThread) {
        WaitForSingleObject(g_hThread, INFINITE);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }

    int wlen = GetWindowTextLengthW(g_hInput);
    if (wlen <= 0) return;
```

再定位 `start_task` 末尾(创建线程前后的禁用控件逻辑):
```c
    SetWindowTextW(g_hInput, L"");
    EnableWindow(g_hInput, FALSE);
    EnableWindow(g_hSend, FALSE);
    EnableWindow(g_hClear, FALSE);

    g_hThread = CreateThread(NULL, 0, agent_thread, task, 0, NULL);
}
```
改为:
```c
    SetWindowTextW(g_hInput, L"");

    InterlockedExchange(&g_cancel, 0);          /* 清除取消标志 */
    EnableWindow(g_hInput, FALSE);              /* 输入框禁用 */
    EnableWindow(g_hClear, FALSE);              /* 清空按钮禁用 */
    SetWindowTextW(g_hSend, L"停止");           /* 发送按钮变停止, 保持启用 (点击即取消) */

    InterlockedExchange(&g_running, 1);
    g_hThread = CreateThread(NULL, 0, agent_thread, task, 0, NULL);
}
```

- [ ] **Step 3: 改 WM_COMMAND 的 ID_SEND 处理 —— 区分发送/取消**

定位 `WndProc` 的 `WM_COMMAND`:
```c
        if (LOWORD(wp) == ID_SEND && HIWORD(wp) == BN_CLICKED) {
            if (IsWindowEnabled(g_hSend)) start_task(hwnd);
            return 0;
        }
```
改为:
```c
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
```

注意:`append_text` 内部是 `strdup` + `PostMessage`,主线程调用安全。

- [ ] **Step 4: 改 WM_APP_DONE —— 恢复按钮文本 + 清运行态**

定位 `HistoryProc` 的 `WM_APP_DONE`:
```c
    if (msg == WM_APP_DONE) {
        EnableWindow(g_hInput, TRUE);
        EnableWindow(g_hSend, TRUE);
        EnableWindow(g_hClear, TRUE);
        SetFocus(g_hInput);
        return 0;
    }
```
改为:
```c
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
```

- [ ] **Step 5: 验证编译**

Run: `make`
Expected: 零警告。

- [ ] **Step 6: 暂不提交**(下一步加入取消检查点后一起测)

---

### Task 6: agent_thread 加入取消检查点

**Files:**
- Modify: `cagent_gui.c`(`agent_thread` 循环顶部与工具执行前)

- [ ] **Step 1: 循环顶部加取消检查**

定位 `agent_thread` 的 for 循环起始:
```c
    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        snprintf(body, BUFSZ,
```
改为:
```c
    for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
        /* 取消检查点 1: 每轮迭代顶部 (LLM 调用前) */
        if (InterlockedCompareExchange(&g_cancel, 0, 0)) {
            append_text("(已取消)\r\n");
            rolled_back = 1;
            goto done;
        }
        snprintf(body, BUFSZ,
```

说明:`InterlockedCompareExchange(&g_cancel, 0, 0)` 原子读取 g_cancel(期望 0、欲写 0,故值不变),返回当前值。

- [ ] **Step 2: 工具执行循环内加取消检查**

定位 Task 4 Step 2 重写后的"执行所有工具"循环:
```c
        /* 执行所有工具: arguments 是字符串化 JSON, 二次解析取 command */
        for (int i = 0; i < n_calls; i++) {
            ToolCall *c = &calls[i];
            {
                char line[8192];
                snprintf(line, sizeof(line), "[Tool] %s(%s)\r\n", c->name, c->args);
                append_text(line);
            }
```
改为(在循环体开头插入取消检查):
```c
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
```

注意:取消时需释放已分配的 `calls[j].output`(若有)。`root` 在 Task 4 Step 2 中执行循环前已 `json_free`,此处无需再释放。

- [ ] **Step 3: 验证编译**

Run: `make`
Expected: 零警告。

- [ ] **Step 4: 手动测试矩阵 5(取消)**

启动 `./cagent_gui.exe`,发送一个会触发多轮工具调用的任务(如"列出当前目录,再列出上级目录,再列出根目录"),在第一个 `[Tool]`/`[Output]` 出现后、下一次 `(thinking...)` 期间点击"停止"按钮:
- 预期:显示 `(正在停止...)` → `(已取消)`
- 按钮恢复为"发送",输入框/清空按钮恢复可用
- 再次发送新消息,对话正常(本轮 user message 已回滚,不含在历史中)

若取消后历史损坏或下次对话报错:回 Step 1/2 检查 savepoint/rolled_back 路径。

- [ ] **Step 5: 提交(Task 5 + Task 6 合并)**

```bash
git add cagent_gui.c
git commit -m "feat: 协作式取消 (发送按钮变停止) 与线程管理修复"
```

---

### Task 7: HTTP/SSL 错误诊断

**Files:**
- Modify: `cagent_gui.c`(`http_post` 失败路径、新增映射表、`call_llm`)

- [ ] **Step 1: 新增 WinHTTP 错误码映射函数**

在 `http_post` 函数之前插入:
```c
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
```

- [ ] **Step 2: http_post 各失败点写诊断**

定位 `http_post` 中三处句柄创建失败:
```c
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
```
改为(每个 `return -1` 前调 `http_set_err`):
```c
    HINTERNET hSession = WinHttpOpen(L"cagent-gui/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { http_set_err(out, out_cap); return -1; }

    HINTERNET hConnect = WinHttpConnect(hSession, whost, port, 0);
    if (!hConnect) { http_set_err(out, out_cap); WinHttpCloseHandle(hSession); return -1; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        http_set_err(out, out_cap);
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return -1;
    }
```

再定位 send/receive 失败处理:
```c
    DWORD blen = (DWORD)body_len;
    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (LPVOID)body, blen, blen, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, NULL);

    int status = -1;
    if (ok) {
```
改为(send/receive 失败时写诊断):
```c
    DWORD blen = (DWORD)body_len;
    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        (LPVOID)body, blen, blen, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, NULL);
    if (!ok) http_set_err(out, out_cap);

    int status = -1;
    if (ok) {
```

- [ ] **Step 3: call_llm 不再覆盖诊断文本**

定位 `call_llm`:
```c
    int status = http_post(full_url, g_api_key, body, strlen(body), resp, BUFSZ);
    if (status < 0) {
        snprintf(resp, BUFSZ, "{\"error\":\"WinHTTP request failed\"}");
    } else if (status != 200) {
```
改为(status<0 时 resp 已含诊断,不再覆盖):
```c
    int status = http_post(full_url, g_api_key, body, strlen(body), resp, BUFSZ);
    if (status < 0) {
        /* resp 已由 http_post 写入诊断文本; 兜底防空 */
        if (!resp[0]) snprintf(resp, BUFSZ, "[网络错误] 未知失败");
    } else if (status != 200) {
```

- [ ] **Step 4: 验证编译**

Run: `make`
Expected: 零警告。

- [ ] **Step 5: 手动测试矩阵 6(错误诊断)**

启动 `./cagent_gui.exe`,逐项触发:

| 操作 | 预期显示 |
|---|---|
| Url-Base 填 `https://no-such-host.invalid/v1` 发送 | `[网络错误] DNS 解析失败, 请检查 Url-Base` |
| Key 填错(有效 URL)发送 | `[HTTP 401]` + 响应体 |
| Url-Base 填 `http://127.0.0.1:1/v1`(端口不通)发送 | `[网络错误] 无法连接服务器` |
| 断网后发送 | `[网络错误] 请求超时` 或 `无法连接服务器` |
| (若有自签证书端点)填入发送 | `[网络错误] SSL 证书...` 类提示 |

任一项显示为旧的 `{"error":"WinHTTP request failed"}`:回 Step 2/3 检查。

- [ ] **Step 6: 提交**

```bash
git add cagent_gui.c
git commit -m "feat: WinHTTP 错误码诊断 (DNS/连接/超时/SSL 证书)"
```

---

### Task 8: README 补充取消/诊断特性与限制说明

**Files:**
- Modify: `README.md`

- [ ] **Step 1: 技术特性表补充两行**

定位 README "技术特性" 表,在表格末尾(`| 配置持久化 |...|` 一行之后)追加:
```
| 协作式取消 | Agent 运行时"发送"按钮变"停止",点击后在迭代间隙优雅退出并回滚本轮历史 |
| 网络错误诊断 | WinHTTP 错误码翻译为中文(DNS/连接/超时/SSL 证书),便于排错 |
```

- [ ] **Step 2: 操作节补充取消说明**

定位 README "### 操作" 小节,在"回车发送"那条之后追加:
```
- **运行中取消**:Agent 调用期间"发送"按钮变为"停止",点击即请求取消;取消在下一个安全点(下一轮迭代前或工具执行前)生效,正在进行的 HTTP 请求无法立即打断
```

- [ ] **Step 3: 常见问题补充取消限制**

在"常见问题"末尾追加:
```
**Q: 点了"停止"但还在转?**
A: 取消是协作式的:已发出的 HTTP 请求无法中途打断,会在下一个迭代间隙生效。若长时间无响应(如服务端不返回),等待超时后才会退出。
```

- [ ] **Step 4: 提交**

```bash
git add README.md
git commit -m "docs: 补充取消与错误诊断特性说明"
```

---

### Task 9: 全量验证

**Files:** 无修改,仅验证

- [ ] **Step 1: 全量构建**

Run: `make clean && make`
Expected: 零警告,生成 `cagent_gui.exe`。

- [ ] **Step 2: selftest 回归**

Run: `./cagent_gui.exe --selftest`
Expected: `json_selftest: OK`,退出码 0。

- [ ] **Step 3: 手动测试矩阵 1–8 全量**

逐项复跑 Task 4 Step 6(1–4、7、8)、Task 6 Step 4(5)、Task 7 Step 5(6)的测试,全部通过。

- [ ] **Step 4: 内存泄漏目检**

确认代码中所有 `malloc`/`strdup`/`calloc`/`realloc` 均有对应 `free`:
- `json_parse` 返回值 → 每条路径都 `json_free`
- `calls[i].output`(strdup)→ 取消路径(Task 6)与正常路径(原"释放工具输出"循环)都 free
- `append_text` 的 `strdup` → `HistoryProc` 的 `WM_APP_APPEND` free
- `task`(calloc)→ `agent_thread` done 处 free

- [ ] **Step 5: 完成确认**

所有矩阵通过 + 零警告 + selftest OK → 阶段一完成。
