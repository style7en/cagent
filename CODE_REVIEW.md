# cagent 代码审查报告

- 审查对象：`master` @ `dd06e09`（已 `git pull`，github / gitee 均为最新，无待合并提交）
- 审查范围：`src/core/*`（16 文件）、`src/ui/*`（9 文件）、`test/*`、`Makefile`、`res/*`，共约 5700 行
- 审查维度：**代码架构 / 代码逻辑 / 代码简洁**
- 验证：`make` 与 `make test` 均**零警告通过**，`json_selftest: OK` / `run_all_tests: OK`

---

## 一、总体结论

这是一份**工程质量显著高于平均水准**的 C 项目。它把"零第三方依赖、单编译单元、全 static"这条约束贯彻得非常彻底，模块拆分清晰、注释几乎都写"为什么"而非"是什么"，错误处理与降级路径考虑得比大多数同类项目周全（原子写会话、UTF-8 自我毒化的预防与自愈、400 超限自愈、进程树超时终止、DPAPI、跨屏 DPI 四步重算）。

但正因为它已经很成熟，剩下的问题多为**边界与一致性**问题，而非结构性缺陷。以下按严重度分级。总体评价：**架构 A-，逻辑 B+，简洁 B**。

---

## 二、架构层面

### A1（中）"平台无关核心"的定位与实现不符 —— 文档失真
`src/cagent_core.h:2` 称 core 为"**平台无关**核心"，README 第 3 行也这么说。但事实上 core 直接依赖 Windows：

- `core/base.h:8-10` 直接 `#include <windows.h> / <winhttp.h> / <wincrypt.h>`
- `net.h` 用 WinHTTP、`crypto.h` 用 DPAPI、`exec.h` 用 `CreateProcessW`、`workspace.h` 用宽字符 API

也就是说 core 是"**与 GUI 无关**"，不是"与平台无关"。这个措辞会误导后来者（例如有人想在 Linux 复用时才发现完全不可移植）。
**建议**：把措辞统一为"与界面解耦的 Windows 核心"，或明确标注"平台相关（Windows）"。

### A2（中）单编译单元下的前向声明分散，依赖图只靠注释维系
项目刻意采用"聚合头 + 全 static"的单 TU 模型，代价是**跨模块调用必须前向声明**，而这些声明散落在多个文件里：

- `get_app_path` 在 `agent.h:76`、`skill.h:39`、`log.h:27` 各声明一次
- `first_object_end` 在 `agent.h:180`、`history_save` 在 `agent.h:157`
- `fopen_utf8` / `utf8_to_wide` 在 `log.h:28-29`
- `json_*` / `utf8_*` 靠 include 顺序先于使用

真正决定编译成败的是 `cagent_core.h` / `main_gui.c` 里的 **include 顺序**，而顺序目前只用注释约束。任何人调整 include 顺序或新增一个跨模块调用，都可能出现"看起来对、编不过"或更隐蔽的"用到了未初始化全局"。
**建议**：新增 `core/forward.h`（或把全部跨模块前向声明集中到聚合头），让依赖关系有单一可检索的落点；并在 `cagent_core.h` 注释里明确"顺序即契约"。

### A3（低-中）全局可变状态过多，阻碍复用与并行
`core/state.h` 定义了大量全局：`messages/body/resp/tool_out` 四块各 ~1MB 静态缓冲，外加 `g_api_url / g_workspace / g_skills / g_running / g_cancel / g_touched_files` 等。这带来两个后果：

1. **进程内只能有一个 Agent 实例、一段对话**，`core` 实质上不是可复用的库；
2. **测试直接改全局**（`test/test.h:30` 直接写 `g_workspace`），测试之间靠约定隔离而非语言机制。

对单窗口桌面程序这是务实取舍，不必推翻。但应把它**显式写成设计约束**（README 已部分提到"静态缓冲 4 块"），避免后来者误以为可以多实例。

### A4（优点）核心 / 界面解耦的钩子设计干净
`base.h:33-43` 的 `cagent_emit / cagent_on_done / cagent_read_config_ui / cagent_stream_begin/undo` 用函数指针做宿主钩子，配合 `ui/hooks.h` 里 `PostMessage` 投递（而非跨线程 `SendMessage`），顺序与线程安全都考虑到了。这是本项目架构上最值得保留的部分。

### A5（优点）模块划分与注释质量
`core/` 按"版本/平台/UTF-8/状态/日志/加密/JSON/执行/网络/编码/工作区/技能/工具/Agent/会话/配置"拆分，边界基本干净；注释密度很高且绝大多数在解释**取舍与踩坑历史**（如 `wndproc.h:106` 为何行距只用 `g_dpi` 一个 DPI 来源）。这种注释是可维护性的正资产。

---

## 三、逻辑层面（含疑似缺陷）

> 标注 `[确认]` = 我已对照代码逐行确认；`[疑似]` = 依赖运行时条件，需复现确认。

### L1（中）`[确认]` 配置项无法被"清空" —— `read_edit_utf8` 空值早退
`src/ui/helpers.h:9-16`：

```c
static void read_edit_utf8(HWND h, char *out, size_t cap) {
    int wlen = GetWindowTextLengthW(h);
    if (wlen <= 0) return;          // ← 空输入直接返回，out 保持旧值
    ...
}
```

用户在界面上把 **Model / Key / 工作目录** 任一框清空后：
- 点"发送" → `start_task` 读回时全局仍是旧值（`task.h:20-23`），会**继续用旧配置**；
- 关窗保存 → `config_save` → `gui_read_config_ui` 同样读回旧值，**清空动作被静默丢弃**。

`set_edit_utf8`（`helpers.h:18-19`）对空值也早退，形成"空 ↔ 空"自洽，但代价是**任何字段一旦有值就再也清不掉**。
**建议**：`read_edit_utf8` 在 `wlen<=0` 时显式 `out[0]='\0'; return;`，让"清空"成为有效输入；`set_edit_utf8` 对空值应改为 `SetWindowTextW(h, L"")`。

### L2（中）`[确认]` `start_task` 不检查 `CreateThread` 返回值
`src/ui/task.h:89-92`：

```c
InterlockedExchange(&g_running, 1);
SetWindowTextW(g_hSend, L"停止");
EnableWindow(g_hSend, TRUE);
g_hThread = CreateThread(NULL, 0, agent_thread, task, 0, NULL);   // 未判空
```

若 `CreateThread` 失败（内存/句柄耗尽），`g_hThread==NULL` 但 `g_running==1`，按钮停在"停止"、`agent_thread` 永不执行、`gui_on_done` 永不回调 → **界面卡死在"运行中"**，只能关窗。
**建议**：判空后回滚 `g_running=0`、恢复按钮文本、提示用户。

### L3（低）`[确认]` `msg_role` 存在越界读（最大 ~105 字节）
`src/core/agent.h:215-233`：

```c
size_t scan = (len < 96) ? len - keylen : 96;
for (size_t i = 0; i <= scan; i++) {
    if (memcmp(obj + i, key, keylen) == 0) { ... }   // keylen = 9
}
```

当对象 `len ∈ [96, 104]` 时，`scan=96`，`memcmp` 会读到 `obj[96..104]`，即**越过对象尾部最多 9 字节**。正常情况下这些字节仍在 1MB 的 `messages` 缓冲内，不会崩；但若对象恰好位于缓冲尾部，理论上是数组越界读。
**建议**：改为 `scan = (len > keylen) ? len - keylen : 0;` 再与 96 取小，保证 `i + keylen <= len`。

### L4（低）`[确认]` `heal_bad_tool_args` 会就地把消息**变长**，可能撑破缓冲
`src/core/session.h:365-367`：把非法的 `"arguments":""`（或半截）替换成 `"arguments":"{}"`，每条**净增 2 字节**，且直接 `memmove` 在 `messages` 原地扩容。`messages` 载入时已被限制在 `BUFSZ` 内，若文件里坏参数极多，累计增长可能越过 `BUFSZ`。
**建议**：修复前检查剩余容量，或改为"载入时先按更小上限截断"。

### L5（低）`[确认]` 流被截断但重试耗尽时，`http_post_stream` 仍返回 200
`src/core/net.h:449/455/494`：成功条件是 `status==200 && completed`；但重试次数用尽后 `break`，最终 `return last_status`（=200）。于是 `agent_turn`（`agent.h:641`）把它当成功处理，可能把**半截 SSE 流**当完整回复入库。
影响有限（好过整轮回滚），但语义上"200 但流不完整"与"200 完整"应可区分。
**建议**：重试耗尽仍未 `completed` 时返回一个专门状态（如 -3）或置标志，让上层明确提示"响应不完整"。

### L6（低）`[确认]` `config_save` 在 DPAPI 加密失败时**静默省略** `api_key` 行
`src/core/config.h:101-104`：`dpapi_protect` 失败则 `api_key=` 行不写。下次启动该键缺失 → Key 丢失且无任何提示。
**建议**：至少 `log_line` 记录，并在界面/日志提示"Key 未能保存"。

### L7（低）`[确认]` `compact_drop_oldest` 依赖 `ks>=1` 才能安全复用 `conv` 指针
`src/core/agent.h:437,466-467`：先取 `conv = messages + strlen(g_system_prompt)`，随后 `reset_conversation()` 会覆写 `messages` 头部，最后仍用 `conv + ks` 作为源。它能正确工作**仅因为** `msg_bounds` 返回的 `start` 总 ≥1（跳过了前导逗号）。这是一处隐式不变量，读代码时很难看出安全性。
**建议**：加注释显式声明该不变量，或先把保留段 `strdup`/拷到临时缓冲再 `reset`。

### L8（低）`[确认]` 工作目录超过 `MAX_PATH` 会让所有文件工具失效
`src/core/workspace.h:98-118` 的 `path_in_workspace` / `resolve_in_workspace` 用 `wchar_t[MAX_PATH]` 承接 `GetFullPathNameW` 结果，一旦工作目录本身长于 `MAX_PATH`，`GetFullPathNameW` 返回所需长度 ≥ MAX_PATH → 函数直接返回 0 → **所有读写工具一律"路径在工作目录外/过长"**。
**建议**：改用 `PATHSZ`（项目已有该约定），或明确文档化该上限。

### L9（低）`config_load` 不剥 UTF-8 BOM
`src/core/config.h:16-17`：若用户用编辑器另存为"带 BOM 的 UTF-8"，首行键会带上 `EF BB BF` 前缀，`strcmp(key,"url_base")` 失配 → **第一个配置项被静默忽略**。程序自身写出的 ini 无 BOM，故仅在用户手工编辑时触发。
**建议**：读入后剥 BOM（`json.h` / `skill.h` 已有同类处理，可复用思路）。

### L10（优点）错误处理与降级链
会话**原子写**（`session_write_atomic`，`session.h:269`）、压缩**三级阶梯**（摘要→半摘要→整条丢弃，`agent.h:481`）、请求前 UTF-8 **预检 + 就地净化自愈**（`log.h:175` / `agent.h:592`）、`400 超限`自愈重试、命令执行**进程树超时终止**（`exec.h` 作业对象）—— 这些都是经过实战打磨、且有注释记录踩坑的成熟设计。

---

## 四、简洁 / 重复层面

### S1（中）重复的"整文件读进堆"样板 ×3
`session.h:380-393`（`history_load_from_file`）、`session.h:494-506`（`session_read_meta`）、`skill.h:95-121`（`skill_read_utf8`）三处是几乎逐字相同的 `fseek/ftell/malloc/fread/fclose` 序列，且都带"必须读全、不能截断"的注释。
**建议**：抽 `static char *read_file_alloc(const char *path, size_t *out_len)` 到 `workspace.h`，三处共用。

### S2（中）两套"大小写不敏感子串查找"
`agent.h:250` 的 `contains_ci`（用 `tolower`）与 `net.h:74` 的 `ascii_icontains`（手写 ASCII 折叠）功能重叠，只是对非 ASCII 字节的处理不同。
**建议**：合并为一个带注释说明"按字节比较、对 UTF-8 安全"的实现。

### S3（中）JSON 扫描逻辑散落 ×4
- `session.h:173` `first_object_end`（找顶层对象边界）
- `session.h:194` `find_messages_inner`（定位 `messages` 数组区间）
- `log.h:146` `msg_index_of`（数第几条消息）
- `session.h:326` `heal_bad_tool_args`（反转义并校验 arguments）

四者都在手写"字符串内跳过转义、跟踪括号深度"的扫描器。
**建议**：抽一个共享的 `json_scan_*` 小工具（跳字符串、找配对括号），减少重复且降低"某一份扫描器有 bug"的风险面。

### S4（中）两个对话框的样板重复
`about.h:225-267`（`show_about`）与 `session_dlg.h:278-327`（`show_session_dialog`）在"注册窗口类一次 + 按 owner DPI 估外框 + 居中于主窗口 + 禁用 owner + 前置"上几乎完全平行；`about_fonts_sync` 与 `sess_fonts_sync` 也是同构的"按局部 DPI 建三种/两种字体"。
**建议**：抽 `dlg_center_on_owner()`、`dlg_create_centered()`、`fonts_build(dpi, ...)` 等公共辅助，两处各减 ~30 行。

### S5（低-中）`config_save` 的"注释块 ↔ 配置项"是两份必须手工同步的清单
`config.h:71-113` 顶部用十几行 `fprintf` 描述全部配置项，底部再逐项写出，注释自己都强调"**顺序须与上方注释块保持一致**"。这正是上一提交 `dd06e09` 的主题（用 `block.txt` + `splice.ps1` 脚本改的），说明维护成本已经显现。
**建议**：改为数据驱动——用一张 `{key, fmt, ptr, 说明}` 表同时生成注释与配置行。

### S6（低）`StreamCtx` 每轮迭代"清零两次"且堆抖动大
`agent.h:619` `calloc(1, sizeof(StreamCtx))` 已清零，`net.h:439` 每次尝试又 `memset(ctx,0,sizeof(*ctx))`。`StreamCtx` ≈1.5MB（含 1MB `content_buf` + 8×64KB `args`），加上 `AgentToolCall calls[8]` ≈0.5MB，**每个工具迭代约 2MB 的 malloc/free 抖动**。
**建议**：把 `content_buf` 等大块改为复用（循环外分配一次），或至少去掉重复的 `memset`。

### S7（低）未跟踪的临时脚本残留在仓库根
`block.txt`（2.8KB）与 `splice.ps1`（680B）是 `dd06e09` 那次 `config.h` 注释块改写用的一次性脚本/数据，既未提交也未加入 `.gitignore`，会一直以 untracked 形式出现在 `git status` 里。
**建议**：删除，或移入 `docs/`（已被忽略）后按需保留。

### S8（低）其他小项
- `agent.h:263` 的 `is_context_overflow_error` 用 `contains_ci`，与 `net.h` 的分类函数风格不统一（见 S2）。
- `about.h` / `session_dlg.h` 各自 `static int reg` 注册类，模式相同却各写一遍（并入 S4）。
- `dpi.h:46-85` 三处 `GetProcAddress` 每次调用都查表，可缓存函数指针（收益小，可选）。

---

## 五、做得好的地方（应保留）

1. **构建纪律**：`Makefile` 的告警组（`-Wshadow -Wformat=2 -Wlogical-op -Wduplicated-* -Wnull-dereference -Wvla -Wundef` + 提前错误组 + 栈/PE 加固）在全部源码上**零噪音**，且注释写明了每条开关的动机。
2. **测试独立且复用真实实现**：`test/` 不聚合进产品二进制，直接调用 `src` 的 static 函数，测的就是真实代码路径（`test/main.c` + `test/test.h`）。
3. **原子写与多代备份**：`.tmp → MoveFileEx` 原子替换 + `.bak` + `.pre_compact`，且启动时清理孤儿备份。
4. **UTF-8 判据单一化**：`utf8.h` 明确把历史上散落的 4 份判据收敛为一份严格 RFC 3629 实现，并解释了"本地预检 vs 服务端 400"对齐的动机。
5. **高危命令的提示词约束 + 回滚提示区分"对话回滚"与"文件不回滚"**：`agent.h:941-951` 明确告知用户文件改动不会还原。

---

## 六、修复优先级建议

| 优先级 | 项 | 类型 | 说明 |
|---|---|---|---|
| P1 | L1 配置无法清空 | 逻辑 | 用户可感知的功能缺陷 |
| P1 | L2 `CreateThread` 未判空 | 逻辑 | 失败即卡死界面 |
| P2 | L3 `msg_role` 越界读 | 逻辑 | 加固，一行修 |
| P2 | A1 文档"平台无关"失真 | 架构/文档 | 一句话修正，避免误导 |
| P2 | S1/S2 重复样板 | 简洁 | 抽公共函数，降低 bug 面 |
| P3 | L4/L5/L6/L7/L8/L9 | 逻辑 | 边界加固 |
| P3 | S3/S4/S5/S6/S7 | 简洁 | 重构收益中等，可分批 |
| — | A2/A3 | 架构 | 属既定取舍，建议文档化而非重构 |

---

## 七、验证记录

```
$ git pull github master   → Already up to date.
$ git pull origin master   → Already up to date.
$ make test                → 零警告; json_selftest: OK / run_all_tests: OK (退出码 0)
$ make                     → 零警告; cagent.exe 生成成功 (258,560 bytes)
```

工具链：GCC 13.2.0 (MinGW-w64) / GNU Make 4.3。

---

## 八、修复记录（本次已实施）

> 按"我的理解"对上述发现做了修复，**只改确认项与安全的去重**；风险较高的大重构（S3 JSON 扫描器合并、S4 对话框样板、S5 config 数据驱动）暂缓，留待后续分批处理。

| 项 | 文件 | 改动 |
|---|---|---|
| L1 配置无法清空 | `src/ui/helpers.h` | `read_edit_utf8` 空输入显式清空 `out`；`set_edit_utf8` 空值也下发空串（对称） |
| L2 线程失败卡死 | `src/ui/task.h` | 判 `CreateThread` 返回值，失败则释放 task、回滚 `g_running`/按钮/焦点并提示 |
| L3 `msg_role` 越界读 | `src/core/agent.h` | 扫描上限改为 `min(len-keylen, 96)`，保证 `memcmp` 不越过对象尾部 |
| L4 净化撑破缓冲 | `src/core/session.h` | `heal_bad_tool_args` 变长前检查 `messages` 余量，放不下即停 |
| L5 半截流当成功 | `src/core/net.h` + `agent.h` | 重试耗尽仍"200 未收完整"返回 `-3`；上层明确提示"本轮未完成"而非当成功 |
| L6 Key 静默丢失 | `src/core/config.h` | DPAPI 加密失败时 `log_line` 留痕 |
| L7 隐式不变量 | `src/core/agent.h` | `compact_drop_oldest` 改为堆上拼装后一次性提交，不再依赖 `ks>=1` |
| L8 深层路径误判 | `src/core/workspace.h` + `tools.h` | 路径校验/解析缓冲由 `MAX_PATH` 放宽到 `PATHSZ` |
| L9 BOM 忽略首项 | `src/core/config.h` | `config_load` 剥 UTF-8 BOM |
| S1 重复样板 ×3 | `src/core/workspace.h` + `session.h`/`skill.h` | 抽 `read_file_alloc` 公共助手，三处复用 |
| S2 重复查找 ×2 | `src/core/agent.h` | 删 `contains_ci`，统一用 `net.h` 的 `ascii_icontains` |
| A1 文档失真 | `src/cagent_core.h` + `README.md` | "平台无关核心" → "与界面解耦的 Windows 核心" |
| S7 临时残留 | 仓库根 | 删除一次性脚本 `block.txt` / `splice.ps1` |

**新增回归测试**（`test/test.h`）：
- `read_file_alloc`：正常读取 / `grow_2x` 容量 / 超 `max_bytes` / 等于上限 / 文件不存在。
- `heal_bad_tool_args`：合法 arguments 不动 / 空 arguments → `{}` / 半截 JSON → `{}`。

**修复后验证（干净重建）**：

```
$ make clean && make    → 零警告，cagent.exe 生成成功
$ make test             → 零警告，json_selftest: OK / run_all_tests: OK (退出码 0)
```

仍未处理（建议后续分批）：A2 前向声明集中、A3 全局状态收敛、S3 JSON 扫描器合并、
S4 对话框样板、S5 config 数据驱动、S6 StreamCtx 堆抖动。

