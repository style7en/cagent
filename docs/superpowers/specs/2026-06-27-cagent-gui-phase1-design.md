# cagent GUI 阶段一优化设计

- **日期**:2026-06-27
- **状态**:已批准,待实现
- **范围**:cagent_gui.c 与配套文件(Makefile / README.md / 删除 mini 版)
- **执行方案**:方案 B —— 解析器与并发分两步,各自可独立构建验证

---

## 1. 背景与目标

`cagent` 是 C 语言实现的极简 AI Agent,当前含两个程序:`cagent_mini.c`(教学 CLI)与 `cagent_gui.c`(Win32 GUI)。GUI 版工程化程度较高,但存在以下问题:

1. JSON 解析全部基于 `strstr` + 启发式抽取,脆弱:顶层 `id` 误取为 `tool_call.id`、并行 `tool_calls` 边界错乱、嵌套结构无法正确处理。
2. `start_task` 对旧线程 `CloseHandle` 而不 `WaitForSingleObject`,仅靠按钮禁用防重入,不健壮。
3. 无任务取消能力,长任务无法中断。
4. HTTP/SSL 错误诊断粗糙:`status<0` 仅 "WinHTTP request failed",无法区分 DNS / 连接 / 超时 / SSL 证书。
5. `MAX_ITERATIONS` 代码为 20、README FAQ 写 5,不一致。
6. mini 版功能与 GUI 版重叠且更脆弱(临时文件 `req.json`/`resp.json`/`tool_out.txt`、硬编码 Key),教学价值已被 GUI 版源码覆盖,维护成本多余。

**阶段一目标**:在不破坏"单文件 + 零第三方依赖"特色的前提下,把 GUI 版的解析与并发地基做扎实,并清理 mini 版。功能增强、UI/UX、Key 加密等留待阶段二。

---

## 2. 范围

### 2.1 纳入(scope)

- 删除 `cagent_mini.c` / `cagent_mini.exe`,同步 Makefile 与 README
- 手写递归下降 JSON 解析器,替换全部 `strstr` 解析逻辑
- 修复工具调用解析(并行调用、嵌套 `tool_calls`、`id`/`name`/`arguments` 边界)
- 并发重构:修复 `start_task` 线程管理 + 发送按钮变"停止"的协作式取消
- HTTP/SSL 错误诊断完善(WinHTTP 错误码翻译)
- 统一 `MAX_ITERATIONS` 为 20,文档对齐

### 2.2 不做(out of scope,留阶段二)

- 流式 SSE 响应显示
- 对话历史持久化到磁盘
- 更多工具(文件读写 / 搜索)
- Markdown 渲染 / 代码块高亮 / 复制按钮 / 深色模式
- API Key 加密存储(DPAPI)
- 命令执行沙箱 / 白名单
- SSL 证书校验跳过开关

### 2.3 不变性约束

- **单文件**:所有代码留在 `cagent_gui.c` 一个文件内。
- **零第三方依赖**:不引入 cJSON 等外部库,JSON 解析器自写。
- **编译命令不变**:`gcc -Wall -Wextra -Os -s -mwindows -lcomctl32 -lwinhttp`,零警告。

---

## 3. 执行节奏(方案 B)

| 步骤 | 内容 | 验证 |
|---|---|---|
| **步骤 1** | 删除 mini + Makefile/README 同步 + 手写 JSON 解析器替换全部 strstr + 工具调用解析修复 | `make` 零警告;手动测试矩阵 1–4、7、8 |
| **步骤 2** | 并发重构(线程管理修复 + 协作式取消) + HTTP/SSL 错误诊断 | `make` 零警告;手动测试矩阵 1–8 全量 |

两步各自保证可独立编译运行;步骤 1 完成后程序行为对外不变(解析更正确),步骤 2 在其基础上叠加取消与诊断。

---

## 4. JSON 解析器设计

单文件内手写递归下降解析器,约 250 行。彻底取代 `extract_string` 与所有 `strstr` 抽取。

### 4.1 数据结构

```c
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JValue JValue;
struct JValue {
    JType type;
    union {
        int b;                                          /* J_BOOL */
        double num;                                     /* J_NUM  */
        char *str;                                      /* J_STR,已解码 UTF-8,堆分配 */
        struct { JValue **items; size_t n, cap; } arr;  /* J_ARR */
        struct { char **keys; JValue **vals; size_t n, cap; } obj; /* J_OBJ */
    };
};
```

### 4.2 公共 API

```c
JValue *json_parse(const char *text);              /* 成功返回根,失败返回 NULL(不部分返回) */
const JValue *json_obj_get(const JValue *obj, const char *key);  /* 对象取字段,无则 NULL */
const JValue *json_arr_at(const JValue *arr, size_t i);          /* 数组取元素,越界 NULL */
const char   *json_as_str(const JValue *v);                      /* 字符串值,非字符串返回 NULL */
void         json_free(JValue *v);                               /* 递归释放 */
```

### 4.3 特性

- 完整转义解码:`\" \\ \/ \b \f \n \r \t \uXXXX`,含 UTF-16 代理对拼回 UTF-8。
- 容错:跳过 UTF-8 BOM、首尾空白;遇非法语法立即中止并返回 NULL。
- 严格 JSON:不支持注释、不支持 trailing comma。
- 内存:所有节点与字符串均 `malloc`,由 `json_free` 递归释放,无泄漏。
- 数字用 `double` 承载(本场景不涉及大整数精度问题)。

### 4.4 用途(解决现有 bug 的关键)

LLM 响应为 OpenAI 格式:

```json
{"choices":[{"message":{"role":"assistant","content":"...",
  "tool_calls":[{"id":"...","type":"function",
    "function":{"name":"...","arguments":"{\"command\":\"...\"}"}}]}}]}
```

- 一次 `json_parse(resp)` 后取 `choices[0].message.content` / `tool_calls[i].id` / `.function.name` / `.function.arguments`。
- `arguments` 是字符串化的 JSON → 二次 `json_parse(arguments)` 取 `command`。
- 从根本上修复"顶层 `id` 误取""并行调用边界错乱""arguments 内嵌引号截断"等问题。

### 4.5 错误处理策略

- 解析失败(HTTP 200 但响应体非法):视为业务异常,走 agent_thread 现有"响应解析失败 → 回滚"分支(见第 5 节)。
- 解析器内部不输出诊断,仅返回 NULL;调用方决定如何呈现。

---

## 5. 并发与取消设计

### 5.1 线程管理修复

- `start_task` 中,若 `g_hThread` 仍非 NULL(防御性,正常路径被按钮禁用挡住),先 `WaitForSingleObject(g_hThread, INFINITE)` 再 `CloseHandle`,杜绝裸 `CloseHandle` 导致的句柄泄漏与悬空。
- 启动新任务前 `InterlockedExchange(&g_cancel, 0)` 重置取消标志。

### 5.2 协作式取消

引入 `static volatile LONG g_cancel;`,用 `InterlockedExchange` / `InterlockedCompareExchange` 保证跨线程可见性。

**UX:发送按钮变"停止"**
- 任务运行时:按钮文本置"停止";点击 → `InterlockedExchange(&g_cancel, 1)` + 按钮立即禁用(防重复点)+ 历史框显示 `(正在停止...)`。
- `agent_thread` 在两处检查 `g_cancel`:
  1. 每轮 `for` 迭代顶部(LLM 调用前)
  2. 每个工具执行前
- 命中则 `append_text("(已取消)\r\n")` + `rolled_back = 1` + `goto done`,复用现有 `savepoint` 回滚机制(`messages[savepoint] = '\0'`)。

**取消语义(明确限制,须写入 README)**
- **协作式**,不使用 `TerminateThread`(后者不安全,会让 `messages` 处于非法中间态)。
- 已发出的 WinHTTP 同步请求无法中途打断;取消在"下一个安全点"生效:下一轮迭代前,或当前工具执行前。
- `WM_APP_DONE` 处理:恢复按钮文本为"发送"、启用输入框与按钮、`InterlockedExchange(&g_cancel, 0)`。

### 5.3 所有权边界(明确)

- **工作线程**:只通过 `PostMessage` 与 UI 通信;绝不 `SendMessage`(避免与主线程死锁);绝不直接读写控件。
- **主线程**:独占所有控件句柄访问;独占 `g_hThread` 的 `CloseHandle`。
- `AgentTask` 由工作线程消费后 `free`;控件句柄生命周期由主线程管理。
- 现有 `append_text` 已是 `PostMessage` + `strdup`,符合该边界,保留。

---

## 6. HTTP/SSL 错误诊断

### 6.1 现状

`http_post` 返回 `status<0` 时 `call_llm` 仅写入 `"WinHTTP request failed"`;非 200 仅加 `[HTTP xxx]` 前缀。无法区分 DNS / 连接 / 超时 / SSL 证书。

### 6.2 改造

- `http_post` 失败路径用 `GetLastError()` 取错误码,经映射表翻译为中文可读文本,写入 `out`(即 `resp`)。
- 映射表 `static const struct { DWORD code; const char *msg; } winhttp_errs[]`,至少覆盖:

| 错误码 | 文案 |
|---|---|
| `ERROR_WINHTTP_NAME_NOT_RESOLVED` | DNS 解析失败,请检查 Url-Base |
| `ERROR_WINHTTP_CANNOT_CONNECT` | 无法连接服务器 |
| `ERROR_WINHTTP_CONNECTION_ERROR` | 连接被重置 |
| `ERROR_WINHTTP_TIMEOUT` | 请求超时 |
| `ERROR_WINHTTP_SECURE_INVALID_CERT` | SSL 证书无效 |
| `ERROR_WINHTTP_SECURE_CERT_CN_INVALID` | 证书主机名不匹配 |
| `ERROR_WINHTTP_SECURE_CERT_DATE_INVALID` | 证书已过期或未生效(请检查系统时间) |
| `ERROR_WINHTTP_SECURE_CHANNEL_ERROR` | SSL/TLS 通道建立失败 |
| (未命中) | `WinHTTP 错误 %lu` |

- `call_llm` 中 `status<0` 时直接显示翻译后的文本;`status!=200` 时保留现有 `[HTTP xxx]` 前缀 + 响应体(便于看 401/429 等业务错误)。
- **SSL 校验保持默认严格**:不降级、不加跳过开关。可配置跳过留阶段二。

---

## 7. 删除 mini 与文档同步

### 7.1 删除文件

- `cagent_mini.c`
- `cagent_mini.exe`

### 7.2 Makefile

- 移除 `MINI_TARGET` / `MINI_SRC` 及其构建规则。
- `all` 目标只构建 `cagent_gui.exe`。
- `clean` 移除 mini 专有产物:`req.json`、`resp.json`、`tool_out.txt`(GUI 版纯内存,无这些产物)。

### 7.3 README.md

- 开篇定位改为"Win32 GUI 版极简 AI Agent(单文件 + 零第三方依赖)"。
- 目录结构移除 mini 两行。
- 删除"教学版 CLI: cagent_mini.exe"整节及其依赖/工具说明。
- "工作原理(Agent 循环)"节:移除 mini 的 `main` 引用,只保留 `cagent_gui.c` 的 `agent_thread`。
- "常见问题":移除 mini 相关条目(curl 依赖等);`MAX_ITERATIONS` 由 5 改为 20。
- "技术特性"表:实现完成后补充两条 —— 协作式取消(发送按钮变停止)、WinHTTP 错误码诊断。
- 新增"取消与限制"说明:协作式取消的生效时机与不可立即打断正在进行的 HTTP 请求的限制。

---

## 8. 验证策略

### 8.1 构建

`make clean && make`,要求 `-Wall -Wextra` 零警告。

### 8.2 手动测试矩阵

| # | 场景 | 预期 |
|---|---|---|
| 1 | 普通对话(无工具) | 单轮回复,history 正常追加 |
| 2 | 单工具调用 | `[Tool]`/`[Output]` 正确,最终回复出现 |
| 3 | 并行多工具(2–4 个) | 每个调用独立 id,assistant+tool 消息 1:1 配对,无错乱 |
| 4 | 多轮工具调用链 | 模型连续调用后正常收尾,不触发 max iterations |
| 5 | 取消:运行中点"停止" | 显示 `(已取消)`,messages 回滚到本轮前,按钮恢复"发送",可继续对话 |
| 6 | 错误诊断:错 URL(DNS)/ 错 Key(401)/ 错端口(连接)/ 断网(超时)/ 自签证书(SSL) | 各自显示对应中文诊断或 `[HTTP xxx]` |
| 7 | 历史 watermark 触发 | 长对话自动清空上下文,提示正常 |
| 8 | 配置持久化 | 改配置 → 关闭 → 重启 → 正确读取 |

### 8.3 解析器自测(可选,建议保留)

`WinMain` 增加隐藏 `--selftest` 参数:跑一批 JSON 用例(嵌套对象/数组、全部转义、代理对、非法输入),断言字段取值;正常双击启动不触发。作为后续回归手段。

### 8.4 完成准则

- 测试矩阵 1–8 全通过。
- `make` 零警告。
- 无内存泄漏(解析器 `json_free` 配对;`AgentTask` free;`strdup` 的 append 文本 free)。

---

## 9. 风险与限制

| 风险 | 缓解 |
|---|---|
| 手写解析器有边界 bug | `--selftest` 用例覆盖;测试矩阵 3 专测并行调用 |
| 协作式取消对正在进行的 HTTP 请求无效 | README 明示限制;UI 显示 `(正在停止...)` 设定预期 |
| 取消时 `messages` 回滚与正常失败回滚路径复用,需确保不冲突 | 复用现有 `savepoint`/`rolled_back` 机制,取消走同一 `goto done` 出口 |
| 删除 mini 后 README 引用残留 | 步骤 1 内统一校对 README 全文 |

---

## 10. 阶段二预留(简述,不在本 spec 实现)

流式 SSE、历史持久化、文件/搜索工具、Markdown 渲染、DPAPI Key 加密、命令沙箱、SSL 跳过开关。阶段一为之预留的接口面:JSON 解析器(便于解析 SSE 事件)、错误诊断(便于阶段二扩展证书选项)。
