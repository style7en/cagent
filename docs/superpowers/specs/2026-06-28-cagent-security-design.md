# cagent 安全增强子项目设计(阶段二 - 子项目 1)

- **日期**:2026-06-28
- **状态**:已批准,待实现
- **范围**:`cagent_gui.c`、`Makefile`、`README.md`、`cagent.ini`(运行时)
- **执行方案**:单子项目 spec,作为阶段二三个子项目之首(安全 → 功能 → 显示重构)

---

## 1. 背景与目标

阶段一已完成健壮性地基(JSON 解析器、协作式取消、WinHTTP 诊断)。阶段二拆为三个子项目,本 spec 是第一个:安全增强。

当前安全问题:
1. `cagent.ini` 中 `api_key` 明文存储,任何能读该文件的进程/用户都可获取。
2. `execute_bash` 执行模型返回的任意命令,无任何拦截,模型误删/格式化等危险操作直接生效。
3. 连接自签证书端点时,WinHTTP 默认严格校验导致无法使用,且无可配置降级。

**目标**:在不破坏单文件 + 零第三方依赖前提下,完成 Key 加密存储、危险命令确认、SSL 校验可配置。

---

## 2. 范围

### 2.1 纳入

- DPAPI 加密 `api_key`(`CryptProtectData`/`CryptUnprotectData`)
- 危险命令确认沙箱(`MessageBox` 人工确认)
- SSL 证书跳过 ini 开关(`skip_cert_verify`)

### 2.2 不做(留后续或排除)

- 真正进程隔离沙箱(job object / 降权 token)—— 复杂度与收益不匹配
- 命令白名单—— 限制过大,模型能力受损
- SSL 开关的 UI checkbox—— 走 ini 配置,保持配置区三框简洁
- 文件读写工具的路径限制—— 留给子项目 2(更多工具)一并考虑

### 2.3 不变性约束

- 单文件:`cagent_gui.c` 不拆分
- 零第三方依赖:`CryptProtectData` 等 Win32 API 经 MinGW 自带 `crypt32` 链接
- 编译命令:`gcc -Wall -Wextra -Os -s -mwindows -lcomctl32 -lwinhttp -lcrypt32`,零警告
- 兼容旧 ini:明文 `api_key` 仍可读,无感迁移到加密

---

## 3. DPAPI Key 加密

### 3.1 API 与依赖

- `CryptProtectData` / `CryptUnprotectData`(DPAPI,绑定当前 Windows 用户)
- `CryptBinaryToStringA` / `CryptStringToBinaryA`(base64 编解码)
- `#include <wincrypt.h>`(dpapi.h 经其引入)
- `Makefile` 的 `GUI_LDFLAGS` 追加 `-lcrypt32`

### 3.2 存储格式

`cagent.ini` 中:
```
api_key=dpapi:<base64 密文>
```
无 `dpapi:` 前缀视为明文(兼容旧 ini)。

### 3.3 读取(config_load)

1. 读到 `api_key` 值后,检测 `dpapi:` 前缀。
2. 有前缀:截掉前缀 → `CryptStringToBinaryA`(BASE64)解码 → `CryptUnprotectData` 解密 → 明文写入 `g_api_key`。
3. 无前缀:直接作明文(旧 ini 兼容)。
4. 解密失败(如换用户/换机):`g_api_key` 置空,不崩溃;启动后历史框提示 "API Key 解密失败,请重新填写"。

### 3.4 保存(config_save)

1. 从 Key 输入框读到明文。
2. 明文非空:`CryptProtectData` 加密 → `CryptBinaryToStringA`(BASE64|NOCRLF)编码 → 写 `api_key=dpapi:<base64>`。
3. 明文为空:写 `api_key=`(空)。
4. 加密失败:回退写明文(降级,保证可启动),历史框不提示(保存时无 UI)。

### 3.5 UI 行为

- Key 输入框仍 `ES_PASSWORD` 显示 `*`(运行时内存明文,仅磁盘加密)。
- 不新增 UI 控件。

---

## 4. 命令沙箱(危险命令确认)

### 4.1 危险命令模式表

`static const char *DANGEROUS[]`,前缀/子串匹配:
```
"rm", "del", "erase", "rmdir", "rd", "format", "shutdown",
"taskkill", "reg delete", "diskpart", "mklink", "takeown", "icacls"
```
覆盖:删除、格式化、关机、杀进程、注册表删除、磁盘操作、符号链接、权限修改。

### 4.2 拦截逻辑(execute_bash 内,run_pipe 前)

```c
for (size_t i = 0; i < sizeof(DANGEROUS)/sizeof(DANGEROUS[0]); i++) {
    if (strstr(command, DANGEROUS[i])) {
        /* 弹确认 */
        int rc = MessageBoxW(NULL, <显示完整 command 的 UTF-16>, 
                             L"危险命令确认", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (rc != IDYES) {
            strcpy(tool_out, "(用户拒绝执行)");
            return;   /* 不执行,把拒绝结果交给模型 */
        }
        break;   /* 确认后不再重复弹 */
    }
}
```

### 4.3 线程模型

- `MessageBoxW` 从工作线程调用,阻塞工作线程。
- 主线程窗口仍可拖动(发送按钮已在禁用态)。
- 取消按钮(停止)在确认弹窗期间不响应(弹窗需先处理);可接受,因弹窗是瞬时操作。

### 4.4 拒绝语义

- 拒绝时 `tool_out = "(用户拒绝执行)"`,作为工具结果追加进 messages 返回模型。
- 模型据此调整方案(如改用非破坏命令),符合 Agent 协作语义。

### 4.5 误报容忍

- `strstr` 子串匹配会误报(如 `echo rm`),但误报只多一次确认,不影响安全。
- 优于漏报。如后续需精确,可在子项目外改进。

---

## 5. SSL 证书跳过开关

### 5.1 配置

- ini 加 `skip_cert_verify=0`(默认 0,严格校验)。
- `config_load` 读入 `static int g_skip_cert_verify = 0;`。
- `config_save` 写回。

### 5.2 http_post 改造

在 `WinHttpOpenRequest` 之后、`WinHttpSendRequest` 之前,若 `g_skip_cert_verify`:
```c
if (g_skip_cert_verify && https) {
    DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA
              | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID
              | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
              | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));
}
```
默认(=0)不动,保持 WinHTTP 严格校验。

### 5.3 文档

README 配置说明补一条 `skip_cert_verify`(用途、默认、风险提示)。

---

## 6. 验证

### 6.1 构建

`make clean && make`,零警告(含新 `-lcrypt32`)。

### 6.2 selftest

`--selftest` 仍全绿(安全改动不影响 JSON 解析器)。

### 6.3 手动矩阵

| # | 操作 | 预期 |
|---|---|---|
| 1 | 旧明文 ini 启动 → 发对话 → 关闭 → 重启 | 首次正常;关闭后 ini 变 `api_key=dpapi:...`;重启后仍可用 |
| 2 | 清空 Key 重填 → 保存 → 重启 | ini 为 `dpapi:...`,Key 正常 |
| 3 | 复制 ini 到另一用户/机器启动 | 提示 "API Key 解密失败,请重新填写",不崩溃 |
| 4 | 发"删除某文件"类任务 | 弹确认框;选"否"→ 模型收到 "(用户拒绝执行)";选"是"→ 执行 |
| 5 | 发 `echo rm xxx` | 弹确认(误报可接受);确认后正常 echo |
| 6 | ini `skip_cert_verify=1` 连自签端点 | 不再 SSL 报错;`=0` → 显示 SSL 证书错误诊断 |
| 7 | 配置持久化 | skip_cert_verify 改动关闭重启后保留 |

### 6.4 完成准则

矩阵 1–7 全通过 + 零警告 + selftest 全绿。

---

## 7. 风险与限制

| 风险 | 缓解 |
|---|---|
| DPAPI 绑定用户,换机/换用户 Key 不可用 | 文档说明;解密失败不崩溃,提示重填 |
| 危险命令表不全(漏报) | 表可扩展;宁可误报不漏报;文档列出当前覆盖范围 |
| `strstr` 误报 | 误报仅多一次确认,不影响安全 |
| 工作线程 MessageBox 与取消按钮竞态 | 弹窗期间停止按钮不响应(可接受,弹窗瞬时) |
| `skip_cert_verify=1` 降低安全 | 默认 0;README 注明风险 |

---

## 8. 后续子项目(不在本 spec)

- 子项目 2(功能):对话历史持久化 + 更多工具(文件/搜索)
- 子项目 3(显示重构):流式 SSE + RichEdit + Markdown/代码高亮
