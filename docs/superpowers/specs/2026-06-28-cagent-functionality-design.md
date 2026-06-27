# cagent 功能增强子项目设计(阶段二 - 子项目 2)

- **日期**:2026-06-28
- **状态**:已批准,待实现
- **范围**:`cagent_gui.c`、`README.md`、`cagent_history.json`(运行时新增)
- **执行方案**:单子项目 spec,阶段二三个子项目之二(安全已完成 → 功能 → 显示重构)

---

## 1. 背景与目标

阶段二子项目 1(安全:DPAPI/沙箱/SSL)已完成并合并。本 spec 是子项目 2:功能增强。

当前功能缺口:
1. 对话历史仅在内存 `messages[BUFSZ]`,关闭即失,无法跨会话延续上下文。
2. 仅 `execute_bash` 一个工具,模型读写文件/列目录/搜内容都要走 shell,既不安全也不结构化。

**目标**:加对话历史持久化(每轮保存/启动加载),新增 read_file/write_file/list_dir/search 四个结构化工具,并把 agent_thread 的工具调用从硬编码改为按名分发。

---

## 2. 范围

### 2.1 纳入

- 对话历史持久化(`cagent_history.json`,每轮 done 后保存,启动加载)
- 新增 4 工具:`read_file` / `write_file` / `list_dir` / `search`(`execute_bash` 保留)
- agent_thread 工具分发改造(按 `c->name` 分发)
- write_file 覆盖确认、search 结果上限

### 2.2 不做(排除或留后续)

- 历史框逐条恢复显示(只恢复 LLM 上下文 `messages`,历史框仅提示"已恢复")
- 多会话/多历史文件(单文件覆盖式)
- search 递归(只搜 path 目录直接子文件)
- 文件路径越界校验(单用户本地工具,YAGNI)
- 工具/历史的 selftest(涉及文件系统,靠手动测试;现有 JSON/DPAPI selftest 保持)

### 2.3 不变性约束

- 单文件 `cagent_gui.c`
- 零第三方依赖
- 编译命令不变:`gcc -Wall -Wextra -Os -s -mwindows -lcomctl32 -lwinhttp -lcrypt32`,零警告
- 现有 `--selftest` 保持全绿

---

## 3. 对话历史持久化

### 3.1 文件与格式

- 文件:`cagent_history.json`,与 exe 同目录(用 `get_ini_path` 同款逻辑取路径,替换文件名)。
- 格式:直接存 `messages` 字符串(JSON 数组内容,含 system + 所有轮,不含外层 `[]`)。
- 编码:UTF-8。

### 3.2 函数

```c
static void get_history_path(char *out, size_t cap);   /* exe 目录 + cagent_history.json */
static void history_save(void);                         /* 把 messages 写文件, 失败静默 */
static int  history_load(void);                         /* 读文件到 messages, 成功 1 失败 0 */
```

### 3.3 保存时机

`agent_thread` 的 `done:` 标签**之前**调用 `history_save()`。此时 savepoint 回滚已生效(`messages[savepoint]='\0'` 若 rolled_back),存的是一致状态。

### 3.4 加载时机

`WM_CREATE` 中 `config_load()` 之后调用 `history_load()`:
- 成功:`messages` 已填,历史框 `append_text("(已恢复历史对话, 可继续)\r\n")`。
- 失败/文件不存在:走 `reset_conversation()`(只 system)。
- **不逐条恢复显示**(历史框只显示提示,不重建过往对话文本)。

### 3.5 清空与 watermark

- **ID_CLEAR**(清空对话):`reset_conversation()` + 删除 `cagent_history.json` + 清历史框。
- **watermark 触发 reset**(`agent_thread` 开头检测 `messages` 过长):`reset_conversation()` 后同步删除历史文件(避免下次启动加载已清空的旧历史)。

### 3.6 错误处理

- 保存失败(磁盘满/只读):静默,不阻塞对话。
- 加载失败(文件损坏/非法 JSON 不检测 —— 直接当字符串读入 `messages`,因为 `messages` 本就是 JSON 数组内容字符串,非完整 JSON;若文件被截断导致后续请求体非法,LLM 会返回错误,走回滚):`reset_conversation()` 兜底。

---

## 4. 工具分发改造 + 5 工具

### 4.1 TOOLS_JSON 扩展

在现有 `execute_bash` 定义后追加 4 个(均为 `type:"function"`,`parameters.type:"object"`,字段 `type:"string"`):

| 工具 | 参数 | 描述 |
|---|---|---|
| `read_file` | `path` | 读取文件文本内容 |
| `write_file` | `path`, `content` | 写入文件(覆盖) |
| `list_dir` | `path` | 列出目录条目(名/大小/类型) |
| `search` | `pattern`, `path` | 在目录下文件内容中搜索 pattern |

### 4.2 agent_thread 工具分发

替换"执行所有工具"循环内硬编码的 `execute_bash(command)`,改为按 `c->name` 分发(解析 `c->args` JSON 取各工具参数,调对应 `tool_*` 函数,结果均写入全局 `tool_out`):

```c
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
```

### 4.3 工具实现(均写入全局 `tool_out`,失败写错误信息)

- `tool_read_file(const char *path)`:`fopen` 二进制读,上限 64KB(超长截断 + 末尾提示 `(已截断, 文件过大)`),`oem_to_utf8` 转码。失败 → `(读取失败: <原因>)`。
- `tool_write_file(const char *path, const char *content)`:见节 5 安全。`fopen` 二进制写,返回 `(已写入 N 字节)`。失败 → `(写入失败)`。
- `tool_list_dir(const char *path)`:`FindFirstFileW`/`FindNextFileW`,格式 `name\tsize\t<DIR>|file`,上限 200 条,超限提示 `(更多条目已截断)`。
- `tool_search(const char *pattern, const char *path)`:见节 5 限制。

---

## 5. write_file 安全与 search 限制

### 5.1 write_file 覆盖确认

写入前若目标文件已存在(`fopen` 读测试),弹 `MessageBoxW`(是/否,`MB_DEFBUTTON2` 默认否):
- 是 → 继续写
- 否 → `tool_out = "(用户拒绝覆盖)"`,不写
- 文件不存在 → 直接写,不弹

线程模型同命令沙箱:`MessageBox` 从工作线程调用,阻塞工作线程。

### 5.2 search 限制

- **非递归**:只搜 `path` 目录直接子文件(不进子目录)。
- **可读文本文件**:跳过 `fopen` 失败的文件;单文件读上限 256KB(超大跳过)。
- **匹配输出**:`文件名:行内容`,上限 50 个匹配,超限追加 `(更多匹配已截断)`。
- **大小写**:区分大小写(`strstr`,简单)。

---

## 6. 验证

### 6.1 构建

`make clean && make`,零警告。

### 6.2 selftest

`--selftest` 仍全绿(现有 JSON/DPAPI 用例,本子项目不加 selftest)。

### 6.3 手动矩阵

| # | 操作 | 预期 |
|---|---|---|
| 1 | 对话几轮 → 关闭 → 重启 | 历史框"已恢复历史对话";继续对话 LLM 有上下文 |
| 2 | 每轮后查 `cagent_history.json` | 内容随轮增长;回滚后存回滚后状态 |
| 3 | 清空对话 → 重启 | 文件删除,无恢复 |
| 4 | watermark 触发自动清空 → 重启 | 不加载已清空旧历史 |
| 5 | read_file 读小文件 | 返回内容;读超大 → 截断提示 |
| 6 | write_file 新文件 | 直接写,返回字节数 |
| 7 | write_file 已存在文件 | 弹确认;否→拒绝;是→覆盖 |
| 8 | list_dir 列目录 | 名/大小/类型,超 200 截断 |
| 9 | search 搜关键词 | 匹配 `文件:行`,超 50 截断 |
| 10 | 模型并行调用多工具 | 各工具独立结果,1:1 配对 |

### 6.4 完成准则

矩阵 1–10 全通过 + 零警告 + selftest 全绿。

---

## 7. 风险与限制

| 风险 | 缓解 |
|---|---|
| 历史文件被手改导致 messages 非法 → LLM 报错 | 走现有回滚机制;reset_conversation 兜底 |
| 历史框不恢复逐条显示,用户可能困惑 | 提示"已恢复历史对话";用户继续对话即见 LLM 有上下文 |
| write_file 覆盖误删 | 文件存在时弹确认,默认否 |
| search 在大目录慢 | 非递归 + 单文件 256KB 上限 + 50 匹配截断 |
| 工具路径无越界校验 | 单用户本地工具,文档注明;留后续 |

---

## 8. 后续

- 子项目 3(显示重构):流式 SSE + RichEdit + Markdown/代码高亮。本子项目的工具分发与历史持久化为之预留:工具结果仍写 `tool_out` 经 `append_text` 显示,RichEdit 替换 EDIT 时显示路径不变。
