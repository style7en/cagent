# cagent 显示重构子项目设计(阶段二 - 子项目 3)

- **日期**:2026-06-28
- **状态**:已批准,待实现
- **范围**:`cagent_gui.c`、`Makefile`、`README.md`
- **执行方案**:单子项目 spec,阶段二三个子项目之三(安全 ✓ → 功能 ✓ → 显示重构)。子项目内部分步:RichEdit 替换 → SSE 流式 → Markdown 渲染。

---

## 1. 背景与目标

阶段二子项目 1(安全)、2(功能)已完成并合并。本 spec 是子项目 3:显示重构。

当前显示局限:
1. 历史框是纯文本 `EDIT`,无法显示格式(粗体/标题/代码块),LLM 回复的 Markdown 全是原始符号。
2. `agent_thread` 一次性等完整 LLM 响应再显示,长回复时用户看不到生成过程。
3. `http_post` 同步读完整个响应体,无法处理 SSE 流。

**目标**:历史框换 RichEdit 2.0,LLM 响应改 SSE 流式增量显示,content 结束后渲染 Markdown 基础集。

---

## 2. 范围

### 2.1 纳入

- RichEdit 2.0(`riched20.dll`)替换历史框 `EDIT`
- 流式 SSE:请求加 `stream:true`,`WinHTTP` 增量读取,`content` delta 实时显示,`tool_calls` delta 累积
- Markdown 基础集渲染:标题/粗体/斜体/代码块/行内代码
- SSE 与 Markdown 交互:流式纯文本显示 + content 结束后整体替换为 Markdown 渲染

### 2.2 不做(排除)

- Markdown 列表/链接/引用/表格
- 代码块按语言语法高亮(统一代码块样式)
- RichEdit 图片/嵌入对象
- 增量 Markdown 渲染(每 delta 渲染,跨 delta 语法难处理)

### 2.3 不变性约束

- 单文件 `cagent_gui.c`(预计 ~2400 行)
- 零第三方依赖(RichEdit 是 Windows 内置)
- 编译命令:`gcc -Wall -Wextra -Os -s -mwindows -lcomctl32 -lwinhttp -lcrypt32 -lriched20`,零警告
- 现有 `--selftest` 保持全绿

---

## 3. RichEdit 替换

### 3.1 控件创建

- `#include <richedit.h>`
- `Makefile` `GUI_LDFLAGS` 追加 `-lriched20`
- 历史框 `CreateWindowExW` 类名由 `L"EDIT"` 改为 `RICHEDIT_CLASSW`(= `L"RichEdit20W"`)
- 样式:`WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL`(同原 EDIT)
- `LoadLibraryW(L"riched20.dll")` 确保控件类注册(MinGW 铙 `-lriched20` 通常已注册,但显式 LoadLibrary 更稳)

### 3.2 追加逻辑

- `append_text` / `do_append` 改用 RichEdit:`EM_SETSEL` 到末尾(`GetWindowTextLength`) + `EM_REPLACESEL` 写 UTF-16。RichEdit 支持与 EDIT 相同的这两条消息,`do_append` 的 LF→CRLF 转换保留。
- 滚动:`EM_SCROLLCARET` 保留。

### 3.3 默认字符格式

`WM_CREATE` 创建历史框后,用 `CHARFORMAT2` 设置默认格式:
- `cbSize = sizeof(CHARFORMAT2)`
- `dwMask = CFM_FACE|CFM_SIZE|CFM_CHARSET`
- `yHeight = 320`(16pt × 20)
- `szFaceName = "Microsoft YaHei UI"`
- `bCharSet = DEFAULT_CHARSET`
- `SendMessage(h, EM_SETCHARFORMAT, SCF_DEFAULT, &cf)`

### 3.4 子类化

`HistoryProc` 保留:处理 `WM_APP_APPEND`/`WM_APP_DONE` + 滚动重绘(`InvalidateRect`)。RichEdit 滚动消息兼容。

---

## 4. 流式 SSE

### 4.1 请求体

`agent_thread` 组装 `body` 时加 `"stream":true`:
```c
snprintf(body, BUFSZ,
    "{\"model\":\"%s\",\"messages\":[%s],\"tools\":%s,\"stream\":true}",
    g_model, messages, TOOLS_JSON);
```

### 4.2 http_post_stream

新增函数(替代 `http_post` 用于 agent_thread;`http_post` 保留供其他用途或合并):

```c
typedef void (*sse_cb)(void *ud, const char *content);   /* content delta 回调 */

static int http_post_stream(const char *url, const char *api_key,
                            const char *body, size_t body_len,
                            char *err_out, size_t err_cap,
                            sse_cb on_content, void *ud,
                            /* tool_calls 累积通过 ud 结构传递 */);
```

- URL 解析、句柄创建、头部、发送:同现有 `http_post`。
- 失败点(`WinHttpOpen`/`Connect`/`OpenRequest`/`Send`/`Receive`):同现有 `http_set_err` 诊断。
- **流式读取**:循环 `WinHttpReadData(tmp, sizeof(tmp), &nread)`,把 tmp 追加到行缓冲(`char linebuf[8192]`),遇 `\n` 处理一行:
  - 行前缀 `data: `:截取 JSON,`json_parse` 解析。
    - `choices[0].delta.content` 存在 → `on_content(ud, content)` 回调。
    - `choices[0].delta.tool_calls[i]`:按 `index` 累积到 ud 的 `ToolCall` 数组(拼 `id`/`function.name`/`function.arguments` 片段)。
  - 行 `data: [DONE]`:结束循环。
  - 其他行(空行/`event:`/`:` 注释):跳过。
- **取消**:每次 `WinHttpReadData` 返回后检查 `g_cancel`,命中则中止(返回特殊码或直接 return)。
- 返回 HTTP 状态码;非 200 读完整响应体到 `err_out` 供诊断。

### 4.3 agent_thread 改造

- `call_llm` 替换为 `call_llm_stream`:调 `http_post_stream`,`on_content` 回调内 `append_text(content)`(纯文本增量显示)。
- tool_calls 在 `http_post_stream` 内累积到 `AgentTask` 或专门结构的数组;`[DONE]` 后返回,agent_thread 检查:有 tool_calls → 执行(复用现有分发);无 → 收尾(走 Markdown 渲染)。
- **取消**:流式读取间隙检查 `g_cancel`,命中则中止读取,`rolled_back=1`,goto done。
- content delta 增量 `append_text` 时,记录起始位置(`GetWindowTextLength` 存入局部变量 `content_start`),供 Markdown 替换。

### 4.4 错误处理

- 网络错误(连接/超时/SSL):`http_set_err` 诊断,回滚。
- SSE 中途断开(连接断,未 `[DONE]`):视为失败,回滚本轮。
- 单行 JSON 解析失败:跳过该行(容错,不中止流)。

---

## 5. Markdown 渲染

### 5.1 SSE 与 Markdown 交互(方案 A)

流式时 `content` delta 实时 `append_text` 纯文本(用户看生成过程);`[DONE]` 且无 tool_calls 时,记录的 `content_start` 到当前末尾的纯文本段,选中删除,用 `md_render` 重新追加格式化文本。

### 5.2 md_render(HWND hRich, const char *utf8)

逐行扫描,每段追加前设置 `CHARFORMAT2`,追加后对行内格式(粗体/斜体/行内代码)选中子串设格式:

| 元素 | 识别 | 格式 |
|---|---|---|
| 标题 | `# `/`## `/`### ` 开头 | 字号 24/20/18pt(`yHeight=480/400/360`),`CFE_BOLD` |
| 代码块 | ``` ``` 包围 | 等宽 `Consolas`,背景浅灰(`crBackColor=RGB(245,245,245)`,`CFE_BACKCOLOR`) |
| 行内代码 | `` `...` `` | 等宽 `Consolas`,背景浅灰 |
| 粗体 | `**...**` | `CFE_BOLD` |
| 斜体 | `*...*` | `CFE_ITALIC` |
| 普通 | 其他 | 默认格式 |

- 代码块状态机:遇 ``` ``` 开始行进入代码块态(后续行用代码格式),遇 ``` ``` 结束行退出。
- 行内格式:在每行追加后,扫描 `**`/`*`/`` ` ``,选中子串 `EM_SETCHARFORMAT` 设 `CFE_BOLD`/`CFE_ITALIC`/等宽。

### 5.3 字符格式设置辅助

```c
static void rich_set_fmt(HWND h, LONG start, LONG end, DWORD mask, DWORD effects,
                         int yHeight, COLORREF back, const char *face);
```
选中 `[start,end)`,填 `CHARFORMAT2` 设 `EM_SETCHARFORMAT`(`SCF_SELECTION`)。

### 5.4 记录位置与替换

- content 流式开始前:`LONG content_start = GetWindowTextLength(g_hHistory);`
- `[DONE]` 无 tool_calls:`LONG content_end = GetWindowTextLength(g_hHistory);`
- 选中 `[content_start, content_end)`:`SendMessage(h, EM_SETSEL, content_start, content_end)` + `EM_REPLACESEL` 空(删除纯文本)。
- 调 `md_render(h, content_text)`:把 `content_text`(累积的完整 content)渲染追加。

agent_thread 需累积完整 content 字符串(供 md_render),同时增量 append 显示。即 `on_content` 回调既 `append_text(delta)` 又 `strcat(content_buf, delta)`。

---

## 6. 验证

### 6.1 构建

`make clean && make`,零警告(含 `-lriched20`)。

### 6.2 selftest

`--selftest` 全绿(现有 JSON/DPAPI 用例,RichEdit/SSE/Markdown 不纳入 selftest)。

### 6.3 手动矩阵

| # | 操作 | 预期 |
|---|---|---|
| 1 | 发普通问题 | content 流式逐字显示;结束后变 Markdown(标题/粗体等) |
| 2 | 发需工具的问题 | `[Tool]` 显示;tool_calls 流式累积后执行;结果返回继续 |
| 3 | 让模型输出代码 | 代码块等宽 + 灰底 |
| 4 | 流式中点"停止" | 中止,回滚 |
| 5 | 网络错误 | 诊断显示 |
| 6 | RichEdit 滚动/选中/复制 | 正常 |
| 7 | 历史持久化/清空/watermark | 与 RichEdit 兼容,仍正常 |

### 6.4 完成准则

矩阵 1–7 全通过 + 零警告 + selftest 全绿。

---

## 7. 风险与限制

| 风险 | 缓解 |
|---|---|
| RichEdit 与 EDIT 行为差异(换行/滚动) | `do_append` 的 LF→CRLF 保留;`HistoryProc` 滚动重绘保留 |
| SSE tool_calls delta 拼接错(分片乱序) | 按 `index` 累积,`[DONE]` 后校验完整性 |
| 流式 + Markdown 替换闪烁(纯文本先显示再替换) | 接受(实时感优先);替换瞬时 |
| Markdown 行内格式定位复杂(子串位置) | 追加后用 `EM_FINDTEXT` 或记录偏移定位 |
| 单行 JSON 解析失败 | 跳过该行,不中止流 |
| 流式读取阻塞与取消 | ReadData 返回间隙检查 `g_cancel` |
| `cagent_gui.c` ~2400 行可维护性 | 用户已确认单文件;注释分节清晰 |

---

## 8. 阶段二完成

本子项目完成并合并后,阶段二(安全 + 功能 + 显示重构)全部完成。
