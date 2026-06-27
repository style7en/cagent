# cagent 安全增强 实现计划(阶段二 - 子项目 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 cagent_gui 加 DPAPI Key 加密、危险命令确认沙箱、SSL 证书跳过开关。

**Architecture:** 仍单文件 `cagent_gui.c`。DPAPI 用 `CryptProtectData` + base64(`CryptBinaryToStringA`),ini 中以 `dpapi:` 前缀区分密文/明文(无感迁移)。沙箱用危险命令模式表 + `MessageBoxW` 确认。SSL 用 ini `skip_cert_verify` 开关 + `WinHttpSetOption`。DPAPI 走 `--selftest` TDD,沙箱/SSL 靠手动测试。

**Tech Stack:** C(MinGW-w64)、Win32、DPAPI(crypt32)、WinHTTP、单文件。

**对应 spec:** `docs/superpowers/specs/2026-06-28-cagent-security-design.md`

---

## 文件结构

- **Modify** `cagent_gui.c` —— DPAPI 函数、config_load/save 集成、execute_bash 沙箱、http_post SSL 开关、selftest 用例
- **Modify** `Makefile` —— `GUI_LDFLAGS` 追加 `-lcrypt32`
- **Modify** `README.md` —— 配置说明补 `skip_cert_verify`、技术特性补 DPAPI/沙箱

---

### Task 1: Makefile 加 crypt32 + DPAPI 函数 + selftest(TDD)

**Files:**
- Modify: `Makefile`
- Modify: `cagent_gui.c`(include 区、解析器区后加 DPAPI 函数、selftest 加用例)

- [ ] **Step 1: Makefile 加 -lcrypt32**

定位 `Makefile` 中:
```makefile
GUI_LDFLAGS = -mwindows -lcomctl32 -lwinhttp
```
改为:
```makefile
GUI_LDFLAGS = -mwindows -lcomctl32 -lwinhttp -lcrypt32
```

- [ ] **Step 2: cagent_gui.c 加 #include <wincrypt.h>**

定位 include 区(`#include <winhttp.h>` 之后):
```c
#include <winhttp.h>
#include <stdio.h>
```
改为:
```c
#include <winhttp.h>
#include <wincrypt.h>
#include <stdio.h>
```

- [ ] **Step 3: 加 dpapi_protect / dpapi_unprotect 声明与 selftest 用例**

在 `json_selftest` 函数之前(解析器实现之后、selftest 之前)插入 DPAPI 函数声明 + 桩:

```c
/* ===== DPAPI Key 加密 =====
 * 加密: 明文 -> "dpapi:<base64>"。解密: "dpapi:<base64>" 或明文 -> 明文。
 * 失败返回 NULL。返回值 malloc, 调用者 free。 */
static char *dpapi_protect(const char *plain);
static char *dpapi_unprotect(const char *stored);
```

然后在 `json_selftest` 函数内、`return` 语句之前,插入 DPAPI round-trip 用例:

```c
    /* DPAPI round-trip + 明文兼容 */
    {
        const char *plain = "sk-test-key-123";
        char *enc = dpapi_protect(plain);
        CHK(enc != NULL && strncmp(enc, "dpapi:", 6) == 0);
        char *dec = dpapi_unprotect(enc);
        CHK(dec != NULL && strcmp(dec, plain) == 0);
        free(enc); free(dec);
        /* 明文兼容: 无 dpapi: 前缀原样返回 */
        char *dec2 = dpapi_unprotect("sk-plain-key");
        CHK(dec2 != NULL && strcmp(dec2, "sk-plain-key") == 0);
        free(dec2);
    }
```

- [ ] **Step 4: 加桩实现(selftest 必然失败)**

在 DPAPI 声明之后、`json_selftest` 之前插入桩:

```c
/* —— 桩 (Task 1 Step 6 替换) —— */
static char *dpapi_protect(const char *plain) { (void)plain; return NULL; }
static char *dpapi_unprotect(const char *stored) { (void)stored; return NULL; }
```

- [ ] **Step 5: 编译 + selftest 验证失败(红)**

Run: `make && ./cagent_gui.exe --selftest; echo exit=$?`
Expected: 编译零警告;selftest 输出 DPAPI 相关 `FAIL:` 行,exit 非 0。

- [ ] **Step 6: 用真实实现替换桩**

删除 Step 4 的桩,替换为:

```c
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
```

- [ ] **Step 7: 编译 + selftest 验证通过(绿)**

Run: `make && ./cagent_gui.exe --selftest; echo exit=$?`
Expected: 编译零警告;selftest 输出 `json_selftest: OK`,exit 0。

- [ ] **Step 8: 提交**

```bash
git add Makefile cagent_gui.c
git commit -m "feat: DPAPI Key 加密 (CryptProtectData) + selftest round-trip"
```

---

### Task 2: config_load/config_save 集成 DPAPI + 解密失败提示

**Files:**
- Modify: `cagent_gui.c`(全局区、`config_load`、`config_save`、`WM_CREATE`)

- [ ] **Step 1: 加 g_key_decrypt_failed 全局**

定位全局配置区(`static char g_model[128] = "";` 之后):
```c
static char g_model[128]    = "";
```
改为:
```c
static char g_model[128]    = "";
static int g_key_decrypt_failed = 0;   /* DPAPI 解密失败标志, 启动后提示 */
```

- [ ] **Step 2: config_load 的 api_key 改用 dpapi_unprotect**

定位 `config_load` 中:
```c
        if (strcmp(key, "url_base") == 0)
            snprintf(g_api_url, sizeof(g_api_url), "%s", val);
        else if (strcmp(key, "api_key") == 0)
            snprintf(g_api_key, sizeof(g_api_key), "%s", val);
        else if (strcmp(key, "model") == 0)
```
改为:
```c
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
```

- [ ] **Step 3: config_save 的 api_key 改用 dpapi_protect**

定位 `config_save` 中:
```c
    fprintf(f, "api_key=%s\r\n",  g_api_key);
```
改为:
```c
    {
        char *enc = dpapi_protect(g_api_key);
        if (enc) { fprintf(f, "api_key=%s\r\n", enc); free(enc); }
        else fprintf(f, "api_key=%s\r\n", g_api_key);   /* 加密失败降级明文 */
    }
```

- [ ] **Step 4: WM_CREATE 历史框创建后加解密失败提示**

定位 `WM_CREATE` 中 `reset_conversation()` 调用(在 `SetFocus(g_hInput)` 之前):
```c
        /* 初始化对话历史(只有 system prompt) */
        reset_conversation();

        SetFocus(g_hInput);
        return 0;
```
改为:
```c
        /* 初始化对话历史(只有 system prompt) */
        reset_conversation();

        /* DPAPI 解密失败提示 (config_load 在历史框创建前执行, 延迟到此处显示) */
        if (g_key_decrypt_failed) {
            append_text("API Key 解密失败 (可能换了用户/机器), 请重新填写 Key。\r\n");
            g_key_decrypt_failed = 0;
        }

        SetFocus(g_hInput);
        return 0;
```

- [ ] **Step 5: 编译 + selftest 回归**

Run: `make && ./cagent_gui.exe --selftest; echo exit=$?`
Expected: 零警告;selftest OK。

- [ ] **Step 6: 提交**

```bash
git add cagent_gui.c
git commit -m "feat: config_load/save 集成 DPAPI 加密 + 解密失败提示"
```

---

### Task 3: 命令沙箱(危险命令确认)

**Files:**
- Modify: `cagent_gui.c`(`execute_bash`)

- [ ] **Step 1: execute_bash 加危险命令拦截**

定位 `execute_bash` 函数(整函数):
```c
static void execute_bash(const char *command) {
    int n = run_pipe(command, tool_out, BUFSZ);
    if (n <= 0) { strcpy(tool_out, "(no output)"); return; }
    oem_to_utf8(tool_out, BUFSZ);
}
```
替换为:
```c
/* 危险命令模式表 (子串匹配, 宁可误报不漏报) */
static const char *DANGEROUS[] = {
    "rm", "del", "erase", "rmdir", "rd", "format", "shutdown",
    "taskkill", "reg delete", "diskpart", "mklink", "takeown", "icacls"
};

static void execute_bash(const char *command) {
    /* 危险命令确认: 命中模式表则弹 MessageBox 让用户决定 */
    for (size_t i = 0; i < sizeof(DANGEROUS)/sizeof(DANGEROUS[0]); i++) {
        if (strstr(command, DANGEROUS[i])) {
            int wlen = MultiByteToWideChar(CP_UTF8, 0, command, -1, NULL, 0);
            WCHAR *wcmd = (WCHAR*)malloc(wlen * sizeof(WCHAR));
            WCHAR msg[8192];
            if (wcmd) {
                MultiByteToWideChar(CP_UTF8, 0, command, -1, wcmd, wlen);
                swprintf(msg, sizeof(msg)/sizeof(msg[0]),
                         L"模型请求执行以下命令:\n\n%s\n\n确认执行?", wcmd);
                free(wcmd);
            } else {
                swprintf(msg, sizeof(msg)/sizeof(msg[0]),
                         L"模型请求执行一条危险命令, 确认执行?");
            }
            int rc = MessageBoxW(NULL, msg, L"危险命令确认",
                                 MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
            if (rc != IDYES) {
                strcpy(tool_out, "(用户拒绝执行)");
                return;   /* 拒绝: 把结果交给模型, 不执行 */
            }
            break;   /* 确认后不再重复弹 */
        }
    }

    int n = run_pipe(command, tool_out, BUFSZ);
    if (n <= 0) { strcpy(tool_out, "(no output)"); return; }
    oem_to_utf8(tool_out, BUFSZ);
}
```

说明:`MessageBoxW` 从工作线程调用,阻塞工作线程;主线程窗口仍可拖动(发送按钮已在禁用态)。`MB_DEFBUTTON2` 让"否"为默认,防误点。

- [ ] **Step 2: 编译**

Run: `make`
Expected: 零警告。

- [ ] **Step 3: selftest 回归**

Run: `./cagent_gui.exe --selftest; echo exit=$?`
Expected: exit 0。

- [ ] **Step 4: 提交**

```bash
git add cagent_gui.c
git commit -m "feat: 危险命令确认沙箱 (rm/del/format 等弹窗确认)"
```

---

### Task 4: SSL 证书跳过开关

**Files:**
- Modify: `cagent_gui.c`(全局区、`config_load`、`config_save`、`http_post`)

- [ ] **Step 1: 加 g_skip_cert_verify 全局**

定位 `g_key_decrypt_failed` 全局(Step 1 of Task 2 加的),在其后:
```c
static int g_key_decrypt_failed = 0;   /* DPAPI 解密失败标志, 启动后提示 */
```
改为:
```c
static int g_key_decrypt_failed = 0;   /* DPAPI 解密失败标志, 启动后提示 */
static int g_skip_cert_verify = 0;     /* 1=跳过 SSL 证书校验 (自签端点用) */
```

- [ ] **Step 2: config_load 读 skip_cert_verify**

定位 `config_load` 中 `model` 分支之后:
```c
        else if (strcmp(key, "model") == 0)
            snprintf(g_model, sizeof(g_model), "%s", val);
    }
```
改为:
```c
        else if (strcmp(key, "model") == 0)
            snprintf(g_model, sizeof(g_model), "%s", val);
        else if (strcmp(key, "skip_cert_verify") == 0)
            g_skip_cert_verify = (atoi(val) != 0);
    }
```

- [ ] **Step 3: config_save 写 skip_cert_verify**

定位 `config_save` 中写 `model` 那行之后:
```c
    fprintf(f, "model=%s\r\n",    g_model);
    fclose(f);
```
改为:
```c
    fprintf(f, "model=%s\r\n",    g_model);
    fprintf(f, "skip_cert_verify=%d\r\n", g_skip_cert_verify);
    fclose(f);
```

- [ ] **Step 4: http_post 加 SSL 跳过**

定位 `http_post` 中 `WinHttpOpenRequest` 之后、`/* 头部: Content-Type + Authorization */` 之前:
```c
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        http_set_err(out, out_cap);
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return -1;
    }

    /* 头部: Content-Type + Authorization */
```
改为:
```c
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        http_set_err(out, out_cap);
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return -1;
    }

    /* 可选: 跳过 SSL 证书校验 (自签端点, 默认仍严格校验) */
    if (g_skip_cert_verify && https) {
        DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA
                  | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
                  | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                  | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
    }

    /* 头部: Content-Type + Authorization */
```

- [ ] **Step 5: 编译 + selftest 回归**

Run: `make && ./cagent_gui.exe --selftest; echo exit=$?`
Expected: 零警告;selftest OK。

- [ ] **Step 6: 提交**

```bash
git add cagent_gui.c
git commit -m "feat: SSL 证书跳过开关 (skip_cert_verify, 默认严格)"
```

---

### Task 5: README 补充

**Files:**
- Modify: `README.md`

- [ ] **Step 1: 配置说明表补 skip_cert_verify**

定位 README "配置说明" 表(在 `| **Model** |` 行之后):
```markdown
| **Model** | 模型 ID,例: `deepseek-v4-flash` |
```
在其后追加一行:
```markdown
| **Model** | 模型 ID,例: `deepseek-v4-flash` |
| **skip_cert_verify** | 写在 `cagent.ini` 中,`1`=跳过 SSL 证书校验(自签端点用,默认 `0` 严格) |
```

- [ ] **Step 2: 技术特性表补 DPAPI 与沙箱**

定位技术特性表(在"网络错误诊断"行之后):
```markdown
| 网络错误诊断 | WinHTTP 错误码翻译为中文(DNS/连接/超时/SSL 证书),便于排错 |
```
在其后追加两行:
```markdown
| 网络错误诊断 | WinHTTP 错误码翻译为中文(DNS/连接/超时/SSL 证书),便于排错 |
| API Key 加密 | DPAPI (`CryptProtectData`) 加密存储 `cagent.ini` 中的 Key,明文不入盘 |
| 命令沙箱 | 危险命令(rm/del/format 等)执行前弹窗确认,拒绝则把结果交回模型 |
```

- [ ] **Step 3: 配置文件格式示例补 skip_cert_verify**

定位 ini 格式示例:
```ini
  # cagent GUI 配置 (UTF-8, 退出时自动保存)
  url_base=https://token.sensenova.cn/v1
  api_key=sk-xxxxxxxx
  model=deepseek-v4-flash
```
改为(注意 api_key 示例改为 dpapi 形式 + 新增 skip_cert_verify):
```ini
  # cagent GUI 配置 (UTF-8, 退出时自动保存)
  url_base=https://token.sensenova.cn/v1
  api_key=dpapi:<DPAPI 加密的 base64>
  model=deepseek-v4-flash
  skip_cert_verify=0
```

- [ ] **Step 4: 常见问题补 DPAPI 限制**

定位常见问题末尾(在"点了停止但还在转"Q 之后、`---` 之前):
```markdown
**Q: 点了"停止"但还在转?**
A: 取消是协作式的:已发出的 HTTP 请求无法中途打断,会在下一个迭代间隙生效。若长时间无响应(如服务端不返回),等待超时后才会退出。

---
```
改为:
```markdown
**Q: 点了"停止"但还在转?**
A: 取消是协作式的:已发出的 HTTP 请求无法中途打断,会在下一个迭代间隙生效。若长时间无响应(如服务端不返回),等待超时后才会退出。

**Q: 换了电脑/用户后 API Key 解密失败?**
A: Key 用 DPAPI 加密,绑定当前 Windows 用户。换机/换用户无法解密,程序会提示并清空 Key,重新填写即可。

**Q: 模型要删文件时弹了确认框?**
A: 命令沙箱拦截了危险命令(rm/del/format 等)。选"否"会把"(用户拒绝执行)"返回模型,模型可改用其他方案。

---
```

- [ ] **Step 5: 提交**

```bash
git add README.md
git commit -m "docs: 补充 DPAPI/沙箱/SSL 跳过说明"
```

---

### Task 6: 全量验证

**Files:** 无修改,仅验证

- [ ] **Step 1: 全量构建**

Run: `make clean && make`
Expected: 零警告,生成 `cagent_gui.exe`。

- [ ] **Step 2: selftest 回归**

Run: `./cagent_gui.exe --selftest; echo exit=$?`
Expected: `json_selftest: OK`,exit 0(含 DPAPI round-trip)。

- [ ] **Step 3: 手动测试矩阵**

| # | 操作 | 预期 |
|---|---|---|
| 1 | 旧明文 ini 启动 → 发对话 → 关闭 → 重启 | 首次正常;关闭后 ini `api_key=dpapi:...`;重启后仍可用 |
| 2 | 清空 Key 重填 → 保存 → 重启 | ini 为 `dpapi:...`,Key 正常 |
| 3 | 复制 ini 到另一用户启动 | 提示"API Key 解密失败",不崩溃 |
| 4 | 发"删除某文件"任务 | 弹确认;否→模型收到"(用户拒绝执行)";是→执行 |
| 5 | 发 `echo rm xxx` | 弹确认(误报);确认后正常 echo |
| 6 | ini `skip_cert_verify=1` 连自签端点 | 不再 SSL 报错;`=0`→SSL 错误诊断 |
| 7 | skip_cert_verify 改动 → 重启 | 配置保留 |

- [ ] **Step 4: 完成确认**

矩阵 1–7 全通过 + 零警告 + selftest 全绿 → 安全子项目完成。
