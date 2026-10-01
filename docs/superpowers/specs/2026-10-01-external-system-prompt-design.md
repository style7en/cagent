# 外置系统提示词 设计文档

日期: 2026-10-01
状态: 已批准 (版本号不变, 保持 1.1.1)

## 目标

系统提示词外置: 程序启动后从 exe 同目录的 `SYSTEM_PROMPT` 文件读取提示词;
文件存在且有效则使用外置提示词, 否则静默回退到内置默认提示词。

## 需求

- 文件: exe 同目录, 文件名 `SYSTEM_PROMPT` (无扩展名), 与 cagent.ini 并列。
- 读取时机: 启动时读一次 (GUI WM_CREATE), 会话内不重读。
- 文件内容: 纯文本, 作为 system 消息的 content; 程序负责转义并包装成 JSON。
- 编码: 合法 UTF-8 原样使用; 非法 UTF-8 (常见: 中文 Windows 的 GBK/ANSI 文件)
  经 `oem_to_utf8` 转换。
- 无效文件 (不存在 / 空 / 超过 32KB / 转码后为空): 静默回退内置默认, 不提示。
- 版本号不变。

## 设计

### 核心数据结构 (core/agent.h)

```c
#define SYSTEM_PROMPT_DEFAULT "..."            /* 原 SYSTEM_PROMPT 宏内容不变 */
static const char *g_system_prompt = SYSTEM_PROMPT_DEFAULT;
```

- 原 `SYSTEM_PROMPT` 宏改名 `SYSTEM_PROMPT_DEFAULT`, 仅作兜底默认值。
- `g_system_prompt` 为运行时指针: 默认指向静态默认串; 外置文件加载成功后
  改指向堆上拼好的 JSON 串 (进程生命周期内最多替换一次, 无需释放)。
- 全项目 15 处 `SYSTEM_PROMPT` 引用 (agent.h / session.h / test.h /
  ui/task.h / ui/session_dlg.h) 全部改为 `g_system_prompt`,
  `reset_conversation()` 用它重建 messages 开头。
- 选型理由 (对比备选):
  - 拷入固定大缓冲: 多一块静态内存且需定上限 — 弃。
  - 每次 reset 重读文件: 违背"启动读一次", 失败时机难控 — 弃。
- 历史文件本就不存 system 消息 (每次加载都 reset 后拼接), 新旧会话兼容。

### 加载流程

```c
static void system_prompt_init(void);   /* 定义在 agent.h, 前向声明 get_app_path */
```

1. `get_app_path(path, cap, "SYSTEM_PROMPT")` 取 exe 同目录路径
   (前向声明, 同 `first_object_end` / `history_save` 的跨模块声明模式)。
2. 文件不存在 → 返回, 使用默认。
3. fseek/ftell 读全文件到堆缓冲, 多读 1 字节判定超限;
   空文件或 > 32KB → 释放返回 (静默回退)。
4. `is_valid_utf8` 校验; 非法则 `oem_to_utf8` 转换; 转后为空 → 回退。
5. `system_prompt_set_raw(text)`:
   - `json_escape_alloc` 转义 (引号/换行/CRLF 由它处理, CRLF 丢弃为 LF);
   - snprintf 拼 `{"role":"system","content":"<escaped>"}`, 校验容量;
   - 成功才提交 `g_system_prompt = buf` (先拼好再提交, 失败保持默认)。

拆出 `system_prompt_set_raw` 的目的: 文件 I/O 与"转义+包装+提交"分离,
测试可直接调 set_raw, 不碰真实 `SYSTEM_PROMPT` 文件。

### 调用点

`ui/wndproc.h` WM_CREATE: `config_load()` 之后、首次 `reset_conversation()`
之前 (该位置在历史框控件创建之后, 将来若要加界面提示可 `append_text`;
本次按需求不提示)。

### 影响面

- 偏移一致性: messages = `g_system_prompt + ",{...}"`, 所有
  `strlen(g_system_prompt)` 偏移、压缩/回放/水位逻辑自动适配, 无格式变化。
- 测试二进制不调用 `system_prompt_init`, `g_system_prompt` 保持默认指针,
  测试环境确定性不受用户真实文件影响。

## 测试 (core/test.h)

1. `system_prompt_set_raw("hi")` → `g_system_prompt` 含
   `{"role":"system","content":"hi"}`; 测完恢复默认指针。
2. 含引号/换行的原文 → 转义正确 (`\"` / `\n`), 仍是合法单对象 JSON
   (可复用 `json_parse` 断言)。
3. 超长输入 (拼出 > 缓冲容量) → 返回失败, 指针保持原值。
4. 既有回归: `make test` / `--selftest` 全绿。

## 手工验证

1. 无 `SYSTEM_PROMPT` 文件启动 → 对话行为与之前一致 (内置提示词)。
2. 写 UTF-8 内容的 `SYSTEM_PROMPT` → 新会话首条请求携带外置提示词
   (可用日志/抓包或改动可见行为验证)。
3. 写 GBK (ANSI) 内容的 `SYSTEM_PROMPT` → 中文正常, 无乱码。
4. 空文件 / 删除文件 → 静默回退默认。

## 非目标

- 不做界面提示 / 不做热重载 / 不改版本号 / 不改历史文件格式。
