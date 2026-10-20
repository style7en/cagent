# cagent

用 C 语言实现的极简 AI 编程 Agent，**零第三方依赖**，运行在 Windows 平台。使用 Win32 GUI，核心（`src/cagent_core.h`）与界面（`src/main_gui.c`）分离，共用同一套 Agent 循环。

> 设计目标：单编译单元、全 `static` 实现、无额外链接步骤；配置、日志、会话、技能都放在可执行文件同目录，整体可直接拷贝分发。

---

## 特性速览

- **极简实现**：C99 + Win32 + WinHTTP，不引入任何第三方库。
- **5 个内置工具**：`execute_bash`、`read_file`、`write_file`、`edit_file`、`load_skill`。
- **OpenAI 兼容接口**：原生结构化 `tool_calls`，支持 SSE 流式输出与多轮工具调用。
- **完整的 GUI**：配置持久化、API Key DPAPI 加密、高 DPI、流式 Markdown（ASCII 表格自动等宽，中文按 2 列对齐）、可折叠的思考/工具块。
- **会话管理**：自动保存、按时间倒序加载、上下文压缩与超限自愈。
- **技能系统**：递归扫描 `SKILL.md`，把技能索引注入系统提示词。
- **可验证**：离线核心回归测试、Markdown 单测、界面预览自检、GUI 端到端自动化。

---

## 快速开始

### 环境要求

- Windows 7 及以上（推荐 Windows 10 1809+，DPI 适配更佳）
- MinGW-w64（GCC 13+）或同等 C 编译器
- 标准 `make`
- 运行 GUI 端到端测试时需要 Python 3

### 编译

```bash
make            # 构建 cagent.exe
make clean      # 删除可执行文件与测试产物
```

也可以单独构建：

```bash
make cagent.exe
```

### 运行

直接双击 `cagent.exe`，或在终端执行：

```bash
./cagent.exe
```

首次启动没有 `cagent.ini`，在界面中填入接口地址、API Key 和模型名称即可；关闭窗口时会自动保存。

### 测试

| 命令 | 说明 |
|---|---|
| `make test` | 编译并运行离线核心回归测试，退出码 = 失败数（0 即通过） |
| `make mdtest` | 运行 Markdown 边界单测（标题、强调、围栏、ASCII 表格等） |
| `make preview` | 把输出区渲染成 BMP，并运行折叠、样式、DPI 等界面自检 |
| `python test/e2e_ui.py` | 启动真实 GUI + 本地 mock 服务，模拟点击/键入做端到端测试 |
| `make clean` | 删除 `cagent.exe`、`app.res` 与 `test/out/` |

`make test` 不需要网络或 GUI，改动核心代码后应作为基本护栏。`test/test.h` 的 `run_all_tests` 覆盖：JSON 解析/转义、编码检测与字符边界、工具分发与参数校验、`edit_file` 定点编辑的各类边界、HTTP 重试判定、上下文压缩（消息遍历 / 保底丢弃的配对完整性 / 超限 400 识别 / token 与字节水位）、`read_file` 的 offset 分块续读、外置系统提示词的转义包装与无效回退、SSE usage 的 `cached_tokens` 兼容解析。

`make preview` 需要可交互桌面，图片输出到 `test/out/`；`e2e_ui.py` 的截图存档在 `test/e2e_artifacts/`。`test/ui_preview.c` 直接 `#include "../src/main_gui.c"`，跑的就是产品代码本身——字体、行距、角色样式、折叠逻辑的取值不可能和真实界面跑偏；代价是单 TU 下大量 unused 告警，只在 `preview` 目标里关掉。

`make preview` 除了出图，还会跑以下自检：

- **折叠自检**：对每个块收起再展开，全文必须逐字还原。
- **样式还原**：折叠再展开后，字色、底色、字体保持一致（预览里的 `style selftest`）。
- **命中测试与点击链路**：用 `EM_POSFROMCHAR` 反查标题行坐标，再发送真实的 `WM_LBUTTONDOWN`，走完整“点击 → 命中 → 切换”链路。
- **思考块时序**：标题行只发一次、正文按序拼接、正式回答前思考块已闭合。
- **空串 delta**：服务端返回空 `reasoning_content`/`content` 时不得切断正文。
- **DPI 段落重算**：缩放前后读回段前距，应当精确翻倍。

> 测试代码全部独立在 `test/`，产品二进制不包含测试代码。测试直接复用 `src/cagent_core.h` 的全部 `static` 实现，测的就是真实代码路径，而不是另写一份桩。
>
> 历史上还有个 `cagent.exe --selftest` 入口（把结果写进 `selftest.txt`）：它存在的唯一理由是 GUI 子系统没有控制台、`printf` 没地方去，只能用文件中转——代价是逼着产品二进制带上整套测试代码（约 20KB），并给 GUI 编译加 `-I.`。已移除；要验二进制就用 `make test`。

---

## 目录结构

```text
cagent/
├── Makefile            # 构建脚本
├── cagent.exe          # 编译产物 —— 注意: exe 所在目录就是"数据根"
├── cagent.ini          # 配置文件 (首次保存时生成; 必须与 exe 同目录)
├── SYSTEM_PROMPT       # 可选: 外置系统提示词 (存在则替代内置默认)
├── sessions/           # 历史会话 (运行时生成; 旧版 history_*.json 启动时自动搬入)
│                       #   xxx.json=当前, xxx.json.bak=上一轮, xxx.json.pre_compact=压缩前存档
├── log/                # 运行日志 + 失败请求留档 (log=1 时生成, 不入库)
├── src/
│   ├── main_gui.c      # 界面聚合入口 (仅按依赖顺序 include ui/)
│   ├── cagent_core.h   # 核心聚合入口 (仅按依赖顺序 include core/)
│   ├── core/           # 与界面解耦的核心, 按子功能拆分 (单 TU, 全 static)
│   │   ├── version.h   # 版本号单一事实来源
│   │   ├── base.h      # 平台头 / 角色常量 / 宿主钩子 / 输出封装
│   │   ├── utf8.h      # UTF-8 解码与字符边界
│   │   ├── state.h     # 缓冲常量 / 全局状态 / 工具声明
│   │   ├── log.h       # 运行日志 / 失败请求留档 / UTF-8 预检
│   │   ├── crypto.h    # DPAPI: API Key 加密存储
│   │   ├── json.h      # 轻量 JSON 解析器 / 转义
│   │   ├── exec.h      # 命令执行: CreateProcess + 管道 + 超时
│   │   ├── net.h       # WinHTTP 与 SSE 流式解析
│   │   ├── encoding.h  # OEM(GBK) → UTF-8 转码
│   │   ├── workspace.h # 工作目录解析、越界限制、UTF-8 文件/路径
│   │   ├── tools.h     # 结构化工具
│   │   ├── skill.h     # 技能扫描 / frontmatter 解析 / 索引
│   │   ├── agent.h     # Agent 循环 / 工具调用 / 取消与回滚
│   │   ├── session.h   # 会话持久化 / 回放 / 多会话命名
│   │   └── config.h    # ini 配置读写
│   └── ui/             # Win32 界面, 按子功能拆分
│       ├── state.h     # 控件 ID / 消息 / 全局句柄与字体
│       ├── hooks.h     # 宿主钩子实现
│       ├── helpers.h   # Edit 读写 + RichEdit 分角色追加
│       ├── md.h        # 流式 Markdown 渲染
│       ├── task.h      # 启动一轮 Agent
│       ├── wndproc.h   # 布局 / 子类 / 主窗口过程
│       ├── dpi.h       # 高 DPI 感知与 dp()/dp_at() 缩放
│       ├── session_dlg.h # 历史会话对话框 (枚举 / 倒序列表 / 载入)
│       ├── about.h     # 关于对话框
│       └── main.h      # 程序入口
├── res/
│   ├── app.rc          # 图标 + 版本信息 + 应用程序清单
│   ├── app.ico
│   └── app.manifest    # comctl32 v6 依赖 + PerMonitorV2 DPI 声明
└── test/               # 回归测试与验证工具 (与 src/ 完全分开)
    ├── test.h          # 核心回归测试 run_all_tests + json_selftest
    ├── main.c          # 控制台入口 (make test)
    ├── md_test.c       # Markdown 边界单测 (make mdtest)
    ├── ui_preview.c    # 输出区排版/折叠离屏预览 + 自检 (make preview)
    ├── e2e_ui.py       # GUI 端到端自动化
    ├── mock_server.py  # e2e 使用的本地 mock 服务
    ├── ui_*_probe.c    # 排查用探针 (不进产品)
    ├── e2e_artifacts/  # e2e 截图存档
    └── out/            # 测试临时产物 (忽略)
```

> **为什么编译产物留在仓库根、而不收进 `build/`**：程序把 **exe 所在目录当作数据根**（见 `src/core/session.h` 的 `get_exe_dir_utf8`），用它定位 `cagent.ini`、`sessions\`、`log\` 和 `SYSTEM_PROMPT`。exe 一旦挪进子目录，这些路径都会跟着跑过去。

> **会话文件是原子写的**：先写 `xxx.json.tmp`，内容完整落盘后再用 `MoveFileEx` 整体替换。进程被杀、磁盘写满都不会把已有历史截断成半截。`xxx.json.bak` 保存上一轮完整历史，`xxx.json.pre_compact` 是上下文压缩前的存档；两者都只有一代。

---

## 配置

### `cagent.ini`

配置文件与 `cagent.exe` 同目录，UTF-8 编码（无 BOM），启动时自动读取，退出时自动保存。以 `#` 或 `;` 开头的行视为注释。

启动程序时如果 `cagent.ini` 不存在，程序会跳过读取；在界面中填好配置并关闭窗口后自动生成。

配置项一览：

| 配置项 | 界面可改 | 默认值 | 说明 |
|---|---|---|---|
| `url_base` | 是 | 空 | API 基础地址，填到 `/v1`；程序自动追加 `/chat/completions`。例：`https://token.sensenova.cn/v1` |
| `api_key` | 是 | 空 | OpenAI 兼容 API Key；界面中显示为 `*`，保存时经 DPAPI 加密 |
| `model` | 是 | 空 | 模型 ID，例如 `deepseek-v4-flash` |
| `workspace` | 是 | exe 目录 | 工作目录；文件工具仅允许访问此目录及其子目录 |
| `skip_cert_verify` | 否 | `0` | `1` = 跳过 SSL 证书校验（仅自签/调试端点使用） |
| `context_tokens` | 否 | `0` | 模型上下文窗口（token），如 `131072`（128K）、`1000000`（1M）。配置后按可用输入预算的 90% 水位触发压缩；`0` = 未知，退回按字节水位估算 |
| `max_tokens` | 否 | `65536` | 单次回复输出预算（token）；它和输入共用模型窗口，水位计算会先扣掉它。`0` = 不下发该字段 |
| `skills_dir` | 否 | `<exe>\skills` | 技能根目录；可用 `;` 分隔多个，相对路径按 exe 目录解析 |
| `last_session` | 否 | 空 | 最近使用的会话文件，由程序自动维护，通常不用手改 |
| `log` | 否 | `1` | 运行日志开关；`0` 关闭 |

示例：

```ini
# cagent 配置 (UTF-8, 退出时自动保存)
url_base=https://token.sensenova.cn/v1
api_key=dpapi:<DPAPI 加密串>
model=deepseek-v4-flash
workspace=C:\path\to\workspace
context_tokens=131072
max_tokens=65536
skills_dir=D:\my\skills
skip_cert_verify=0
last_session=sessions\history_<工作目录>_<哈希>.json
log=1
```

几个注意点：

- **修改 ini 前先退出 cagent**，否则程序退出时会用内存中的配置覆盖文件。
- `api_key` 绑定当前 Windows 用户。换机或换用户后 DPAPI 无法解密，程序会提示并清空 Key，重新填写即可。
- `workspace`、`skills_dir` 的相对值都按 **exe 目录**解析，不按当前命令行目录。
- `log=1` 时日志按天写入 `log\`；HTTP 失败时完整请求体另存为 `log\request_fail_N.json`，可直接对服务端重放复现。

### 外置系统提示词 `SYSTEM_PROMPT`

- **位置**：与 `cagent.exe` 同目录，文件名 `SYSTEM_PROMPT`（无扩展名）。
- **内容**：纯文本，作为 system 提示词正文；引号和换行由程序自动转义成 JSON。
- **编码**：UTF-8 优先；非 UTF-8（如中文 Windows 的 GBK/ANSI 另存）自动按系统代码页转换。
- **行为**：启动时读一次；文件不存在、为空或超过 32KB 时静默回退内置默认提示词。
- 有技能时，技能索引会追加到系统提示词末尾（外置提示词同样如此）。

### 技能 `skills\`

把常用任务的“操作说明书”预置成技能，模型在任务匹配时自动加载并照做。

- **位置**：默认 `<exe>\skills`；可在 `cagent.ini` 中用 `skills_dir=` 改为任意目录，也可用 `CAGENT_SKILLS_DIR` 环境变量临时覆盖（优先级低于 ini）。
- **扫描规则**：递归扫描目录树，任意深度的 `SKILL.md` 都算一个技能；跳过点开头目录与 junction/链接，防止成环。技能名默认取 `SKILL.md` 所在目录名。
- **多个根目录**：`skills_dir` 可用 `;` 分隔多个目录，按顺序扫描，重名先到先得。路径含空格直接写，不需要引号（写了成对引号也会被剥掉）；因为 `;` 是分隔符，路径本身不能含 `;`。
- **上限**：最多 32 个技能；单个 `SKILL.md` 不超过 32KB。编码 UTF-8 优先，非 UTF-8 自动转码，UTF-8 BOM 自动剥除。
- **生效方式**：启动时扫描一次，技能集在进程内固定；新增技能需要重启生效。

目录示例：

```text
cagent.exe
skills\
  ├─ build\
  │    └─ SKILL.md
  └─ deploy\
       └─ SKILL.md
```

`SKILL.md` 正文为纯文本，可选 frontmatter 声明元数据：

```markdown
---
name: build
description: 编译并测试本项目
---

1. 先跑 make clean && make
2. 再跑 make test
```

- 缺 `name` 时用 `SKILL.md` 所在目录名。
- 缺 `description` 时用正文第一个非空行。
- 正文会整体转义后进入上下文；`load_skill(name)` 返回完整正文，未知名会返回可用技能列表，由模型自行纠正。

---

## 使用

### 界面布局

```text
+-----------------------------------+
| 接口地址  [ https://...v1     ]   |  ← 三项配置, 启动时自动从 cagent.ini 读取
| API Key   [ ********          ]   |  ← 以 * 隐藏显示
| 模型名称  [ deepseek-v4-flash ]   |
| 工作目录  [ C:\...            ] [浏览...] [新建会话] [加载会话] |
+-----------------------------------+
|                                   |
| 分析当前项目                        |  ← 用户: 深蓝加粗 + 浅蓝底
|                                   |
| ▾ 思考过程                         |  ← 思考块标题 (可点击折叠): 加粗灰 + 冷灰底
|   先看 Makefile 弄清构建方式……      |  ← 思考正文: 灰 + 冷灰底, 缩进在标题之下
|                                   |
| AI 回复正文                        |  ← AI: 黑
| ▾ execute_bash                    |  ← 工具调用 (可点击折叠): 琥珀加粗
|   {"command": "dir /b"}           |  ← 参数: 等宽灰, 缩进在工具名之下
|   .superpowers                    |  ← 输出: 等宽, 带暖灰底的整块
|   .workbuddy                      |
|                                   |
| × edit_file                       |  ← 出错: 红
| 未执行: arguments 超过 8KB 上限     |
+-----------------------------------+
| [输入框]                 [ 停止 ] |
|                          [ 发送 ] |
+-----------------------------------+
| 就绪 | 缓存命中 本次83.2% 累计 91.0% 12次 | 上下文 45.2% 59.3K/256K |
+-----------------------------------+
```

**状态栏**（窗口最底部）显示模型此刻的状态：`就绪` / `等待响应…` / `正在生成…` / `正在执行 <工具名>…` / `被限流, 重试中…`。状态之后跟两段统计：

- `缓存命中 本次83.2% 累计 91.0% 12次`：**本次** = 最近一次请求的命中率，**累计** = 当前会话内所有请求的命中 token 之和 ÷ prompt token 之和，`12次` 是参与累计的请求数。服务端从不返回缓存字段（`usage.prompt_tokens_details.cached_tokens` / `prompt_cache_hit_tokens`）时整段省略，不把“不支持缓存”显示成 0%。
- `上下文 45.2% 59.3K/256K`：已用 token（按服务端 `prompt_tokens` 读数 + 之后新增字节估算）÷ 分母。分母优先用模型总窗口（ini 的 `context_tokens`）；未配置时用默认值 **256K**（`CONTEXT_TOKENS_DEFAULT`，只影响状态栏显示，压缩仍按字节水位兜底）。实际窗口不是 256K 的模型请在 ini 里配置 `context_tokens`。

统计口径与生命周期：**新建会话**、**载入会话**、**切换接入点或模型**时，本次/累计/上次 prompt 读数都会清零并立即刷新状态栏；切换接入点或模型在下次发送时生效，**对话历史保留**（允许换模型接着聊），但上下文分母仍按 ini 的 `context_tokens`——程序无法从模型名推断窗口。新建会话后显示约 `1.0K` 属于正常：那是每次都随请求发送的 system prompt（含技能索引）本身。

这些信息由 core 通过 `cagent_status` 钩子上报（见 `core/base.h`），UI 侧走消息队列投递到主线程再更新控件——core 在工作线程里报告，不能直接碰控件。它们属于**状态**而不是**对话内容**：早期是往输出区打旁白的（`等待响应…`、`(请求限流, 等待后重试...)`），既占地方，又会跟着历史一起保存和回放，重试时还会把对话冲得七零八落；现在只出现在状态栏。

**配色与排版**：输出区底色为 `RGB(247,247,247)`（`state.h` 的 `HIST_BG`），比输入框的系统白略深一档，让“只读输出”和“可编辑输入”一眼分层。247 不是随手取的——输出区最浅的文字是旁白的 `RGB(110,110,110)`，它把底色能压到的下限锁住了：再深到窗口自身的 `COLOR_BTNFACE`(240) 时，灰字对比度会掉到 4.47:1，低于 WCAG AA 的 4.5:1。反过来说，灰字也不能再调浅：138 灰在 247 底上只有 3.2:1，旁白想“更淡”只能靠字号或留白，不能靠调浅颜色。

三类内容靠“字体 + 底色冷暖 + 有没有引导行”同时区分：

| 类型 | 字体 | 底色 | 引导行 |
|---|---|---|---|
| 思考过程 | 正文 | 冷灰 `RGB(241,242,246)` | `▾ 思考过程` |
| 工具输出 | 等宽 | 暖灰 `RGB(237,237,233)` | `▸ 工具名` |
| AI 正文 | 正文 | 无 | 无 |

ASCII 表格行单独用 `NSimSun`（`dpi.h` 的 `CAGENT_TABLE_FACE`），而不是普通的 `Consolas`。原因不是“等宽”两个字就够了：模型按“中文占 2 个 ASCII 列”排 `+---+` 表格，而 RichEdit 里 Consolas 没有中文字形，回退后的中文实测只有约 1.8 个 ASCII 格宽，于是每经过一个中文单元格，竖线就向左漂一点。NSimSun 的中文严格等于 2 格，表格边框和内容才能真正对齐；普通代码块与工具输出仍用 Consolas，但**围栏内命中表格形态的行同样切 NSimSun**——模型经常把整张表包进 ` ``` ` 里，走代码路径时若沿用 Consolas，右边框照样会漂。

每类文字的样式由 `state.h` 的 `g_role_style` 一张表决定（数组下标 = `CAGENT_ROLE_*`）：文字色、底色、粗斜体、是否等宽、段前/段后留白、左缩进都在同一行。新增角色只加一行表项，调观感只改一处，不用去翻 `do_append` 的 if-else。核心侧通过 `emit_note` / `emit_think` / `emit_think_head` / `emit_tool` / `emit_args` / `emit_output` / `emit_err` 标记每段文字的归属（见 `core/base.h`），不要直接调 `cagent_emit` 传角色常量。

### 会话管理

- **新建会话**：当前对话自动落盘保留，另起一个空对话；不删除任何旧文件。
- **加载会话**：打开历史会话窗口，按文件修改时间**倒序排列，最新的在最上面**。每行用两行紧凑布局显示：工作目录 + 消息条数/时间/首句预览。选中后回放到界面，并可继续对话。
- **自动保存**：每轮结束后按工作目录保存到 `sessions\history_*.json`；同一工作目录支持多个会话文件。
- **启动行为**：启动不自动加载历史，默认是全新对话；需要继续旧会话时点“加载会话”。
- **回滚**：取消或出错时只回滚本轮**对话**，已执行的文件改动不会撤销；回滚提示会列出本轮改过的文件。

### 输入与操作

- **回车**发送；**Shift+回车**换行。
- **插话（排队）**：Agent 运行期间输入框照常可写，回车或点“发送”把消息排队，不打断当前轮。当前轮结束后自动接着发出；按钮显示排队计数“发送 (N)”，最多 8 条，每条上限 64KB。采用排队而不是打断：回滚只还原对话，已落盘的文件改动不会复原。
- **停止**：点“停止”按钮或按 **Esc** 请求取消当前轮，空闲时按钮置灰。取消是协作式的：在下一个安全点（下一轮迭代前或工具执行前）生效，已发出的 HTTP 请求无法立即打断。取消只影响当前轮，排队消息仍会发出。
- **按钮竖排**：停止在上、发送在下。并排会挤窄输入框；竖排后列宽只占一个按钮。
- **Ctrl + 滚轮**：调整输出区字号（等效 12~40 逻辑像素，默认 18）。窗口内任意位置生效，只影响输出区，不影响配置框；缩放在内存里，重启回到默认值。主窗口、输出框、输入框三处都挂了同一处理，否则焦点在输入框时会失灵。
- **实现方式**：输出区缩放用 `EM_SETZOOM`（整体缩放显示），不是改文字字号；这样行距会等比缩放，也不会在跨显示器重建字体时跳变。入口见 `wndproc.h` 的 `ui_history_zoom`。
- **流式显示**：LLM 回复逐字流式显示；按角色着色（用户=蓝加粗、AI=黑、旁白=灰、工具调用=琥珀、工具输出=等宽灰底、出错=红）；推理模型的思考过程单独成块，可点击标题收起/展开；emoji 以单色字形显示。
- **长内容**自动换行，自动滚动到底部。

### 可折叠块：思考过程与工具调用

两类内容可以点击标题行收起/展开（鼠标悬停标题行会变成手型）：

| 块 | 标题行 | 正文 | 什么时候出现 |
|---|---|---|---|
| 工具调用 | `▾ 调用工具 <名>` | 参数行 + 输出块（一起折叠） | 每次工具调用都有 |
| 思考过程 | `▾ 思考过程` | 推理正文 | 模型返回 `reasoning_content` 时 |

**工具块对所有模型都有用**：工具输出经常几十上百行（实测一轮任务能刷出 754 个 SSE 数据行），折起来界面才清爽。收起后只剩一行 `▸ 调用工具 <名>`，参数行与输出块全部收掉，展开时逐段按原角色还原。

思考过程来自推理模型的 `delta.reasoning_content`，与正式回答的 `delta.content` 并列。几条与 API 语义绑定的规则：

- **思考内容只上屏，不累积、不进 `messages`。** DeepSeek 明确要求多轮对话不要回传 `reasoning_content`，所以它既不占上下文也不重复计费。代价是重新加载会话时看不到思考过程——这是符合规范的必然结果，不是 bug。
- **标题行文案是 core 与界面的契约**（`CAGENT_FOLD_GLYPH_OPEN/SHUT`、`CAGENT_THINK_TITLE`）。首字符是折叠状态符号，切换时只替换这一个字符：`▾`(U+25BE) 与 `▸`(U+25B8) 都是单个 UTF-16 码元，替换后标题行长度不变，否则正文的字符位置会漂移。
- **思考块在正式回答之前必须闭合**（补一个换行）。不闭合的话正文会接在思考内容同一行，而且折叠块以“整段”为单位，会把正式回答一起吞掉。两条收尾路径（来了 content / 整条流结束在思考阶段）都走 `close_reasoning_block()`。
- **“本片没内容”有两种表达，空串必须按“没有”处理。** 有的服务端每个分片都同时带 `reasoning_content` 与 `content`，没在输出的那个给 `""`。`json_as_str` 对 `""` 返回非空指针，于是“键存在就处理”的写法会在吐正文时每片先空投一次 THINK；那次空投是非 AI 角色，前端见角色切换会 `md_flush()` 把正在攒的半行落地，正式回答因此每行都从句子中间断开。同理，空 `content` 会在第一个思考分片之后就把思考块提前闭合。所以 `stream_apply_delta()` 里两个字段都是 `if (s && *s)`，两个回调内部再各挡一层，前端 `do_append` 也对空内容直接返回。
- **索引表满了挤掉最老的块，而不是丢新块**（`ui_fold_new` → `ui_fold_drop_oldest`）。容量是 128 个块；长会话很容易超过它。挤掉最老的能保证文档末尾的块永远可折；挤掉时必须释放它持有的正文副本（收起态的块把正文 `malloc` 在 `segs[].text` 里），否则长会话每挤一块就漏一份。
- **正文按“角色段”记录**（`UiFoldSeg`），不是一整块纯文本：工具块正文里参数行和输出块样式不同，折叠时逐段读回、展开时按原角色重放，才能把样式原样还原。只比对文本不够——两段都是等宽字体，刷成同一个角色文本比对照样通过，但界面已经错了。
- **流式进行中不响应折叠**：正文正在往文档末尾追加时若把块收起，后续增量会直接接在标题行后面。要做到“边流边折叠”，得让追加改投隐藏缓冲、展开时再合并，那是另一套状态机。
- **工具输出的“尾部说明”不上屏**：`read_file` 这类工具会在输出末尾附一句协议层信息（`...(已截断: 文件共 N 字节, 本次返回第 1~6144 字节; 用 read_file 的 offset=6144 继续读取...)`）。它是给模型看的，用户不需要看到。上屏前用 `tool_note_split()` 把它剥掉，回传模型的 `tool_out` 保持完整。判据是“它必须是最后一行”，否则读一个恰好包含同样字样的文件会被误剥。

### 工具

内置 5 个工具（对齐 pi.dev 的极简理念：4 个基础工具 + 1 个技能加载工具）：

- `execute_bash(command)` — 执行 shell 命令并捕获输出（不拦截，由用户自担风险）。默认超时 60s，超时杀掉整个进程树；输出超限自动截断并附提示。输出末尾附 `[exit=N]` 退出码；`stdin` 给 `NUL`，交互式命令会直接读到 EOF，而不是挂到超时。
- `read_file(path, offset?)` — 读取文件文本内容；非 UTF-8（如 GBK）自动转成 UTF-8。大文件**首读**只回 6KB 头部预览（明示文件总大小与续读 offset），带 offset 按上限（默认 16KB）分块续读；超限时截断并告知总大小与下一次 offset。起点落在多字节字符中间时自动对齐到字符边界。预读大文件建议先用 `findstr` 定位关键词，再按 offset 读命中区间。
- `edit_file(path, old_text, new_text)` — 定点替换编辑（改局部内容的首选）。`old_text` 必须与原文完全一致且在文件中唯一出现（按非重叠出现计数），否则不改动文件并说明原因。改一行无需重写全文；非 UTF-8（GBK）文件会被拒绝，并提示改用 `write_file`。
- `write_file(path, content)` — 写入文件（直接覆盖）；覆盖已有文件时提示词要求模型先 `read_file` 确认内容。父目录需已存在。
- `load_skill(name)` — 按名称加载用户预置的技能说明；返回文本不执行任何东西。任务与某个技能相关时由模型主动调用并照说明执行。

> 没有专门的列目录/搜索工具：查看目录用 `dir`，递归搜索内容用 `findstr /s /i "关键词" *.*`，都走 `execute_bash`。

#### 高危命令需用户确认

由系统提示词约束模型行为，框架不做技术拦截：

- 调用 `execute_bash` 前，模型须先用一句话说明完整命令和目的；高危操作还要说明风险并停下来等用户明确确认，确认之前不执行，此时该轮自然结束，用户回复“确认”后模型再执行。
- 高危范围：删除或覆盖文件（`del`、`rd`、`rmdir /s`、`format`、`diskpart`）、改注册表或系统服务（`reg`、`sc`、`net user`、`schtasks`）、关机重启、批量移动或重命名、下载后直接执行外部脚本、改写 git 历史（`git push -f`、`reset --hard`），以及任何写入工作目录之外位置的操作。
- 用户确认只对当前这一条命令有效；命令、参数或影响范围变化时必须重新确认。列目录、读文件等只读操作不受影响；用户拒绝则改用更安全的方案。
- 这是**提示词层面的约束，不是沙箱**。真正隔离请在容器/虚拟机中运行。

#### 结果上限与调参

| 环境变量 / 常量 | 默认 | 说明 |
|---|---|---|
| `CAGENT_TOOL_OUTPUT_MAX` | 16384 (16KB) | 单次工具结果进入上下文的上限，超出即截断并附提示。`read_file` 首读预览取 `min(6KB, 上限)`，调小上限时只缩不放 |
| `CAGENT_CMD_TIMEOUT` | 60 (秒) | 命令执行超时，超时终止整个进程树 |
| `EDIT_MAX_BYTES` | 2MB | `edit_file` 可处理的单文件体积上限 |
| `CAGENT_HTTP_RETRIES` | 3（429 为 6） | 单次 LLM 请求的最大重试次数；仅对瞬时可恢复错误（网络层错误 / 408 / 429 / 5xx）重试。429 默认至少 6 次；环境变量显式调高时取较大者 |
| `CAGENT_HTTP_RETRY_MS` | 2000 (毫秒) | 网络类错误首次退避基数，后续按 2 倍指数增长（2 → 4 → 8 …，单次上限 60s）；429 限流单独用 5s 基数（5 → 10 → 20 …），服务端 `Retry-After` 更长时以其为准（上限 120s） |

### 上下文管理

对话上下文缓冲为 1MB（编译期常量 `BUFSZ`，约 30 万+ 汉字）。配合 `context_tokens` 可实现按模型真实窗口的 token 级水位控制。

- **水位判定**：配置 `context_tokens` 后，可用输入预算 = `context_tokens - max_tokens`（`max_tokens=0` 或不小于窗口时按窗口），优先用服务端流式响应里的 `usage.prompt_tokens` 精确估算（增量部分按约 3 字节/token 折算），到输入预算的 90% 触发压缩；未配置或服务端不发 usage 时，退回字节水位（75% 缓冲）。留出 10% 余量是为了尽量晚压缩——压缩会改写历史前缀，让整段 prefix cache 失效。
- **前缀缓存友好**：对话历史按追加方式保存，不在每轮改写；技能索引按名称排序，外置 `SYSTEM_PROMPT` 自动去 BOM、统一 CRLF/CR 为 LF，避免无关字节变化让缓存整体失效。请求携带 `stream_options.include_usage`，日志里的 `[usage]` 行会输出 `prompt` / `cached` / `hit`（本次命中率）与 `cum`（累计命中率/次数），状态栏也会在活动文案（如“就绪”）后显示 `缓存命中 本次… 累计… N次` 与 `上下文 百分比 已用/窗口`；服务端不返回缓存字段时不显示缓存段，避免把“不支持”误报成 0%。累计只统计报告过缓存字段的请求（新建/载入会话时清零）。缓存命中的 token 仍占模型窗口，不会从水位中扣除。
- **压缩时机**：每轮用户消息开始前和长工具链任务的每次迭代间隙都会检查。单轮内多次工具调用累积的上下文也会被及时压缩，而不是等到下一轮。
- **压缩方式（阶梯降级）**：先把较早的对话请模型压成交接摘要（≤2000 字，保留目标、步骤与结论、文件路径、待办），最近的若干条消息原样保留在摘要之后；摘要请求沿用 `tools` + `tool_choice:"none"`，让旧前缀仍可命中缓存，同时禁止模型改去调用工具。摘要请求失败则减半重试；仍失败则不经模型、按整条消息丢弃最旧部分（不拆散 `tool_call` 与结果配对）。压缩前当前会话文件自动归档为 `*.pre_compact`。
- **超限自愈**：服务端返回“上下文超限”类 400 时，自动压缩后原地重试（有次数上限），而不是回滚丢掉整轮；压缩彻底失败时，轮首另起新会话、轮中保留已完成的工作提前收束。

### 技术特性

| 特性 | 实现 |
|---|---|
| HTTPS 网络 | **WinHTTP**（Windows 内置，无外部依赖） |
| UI 不卡死 | LLM 调用走后台线程，UI 主线程用 `PostMessage` 接收追加事件 |
| 高 DPI 适配 | **PerMonitorV2**（声明在 `res/app.manifest`）+ 全部布局与字号经 `dp()` 换算 |
| 中文显示 | 全程 UTF-8 ↔ UTF-16 转换，字体 Microsoft YaHei UI |
| 静默工具执行 | `CreateProcess + CREATE_NO_WINDOW + 匿名管道`，不弹 cmd 黑框 |
| 纯内存 IO | 无 `req.json` / `resp.json` 临时文件 |
| 配置持久化 | 启动加载 / 退出保存 `cagent.ini` |
| 协作式取消 | 停止按钮或 Esc 在迭代间隙优雅退出并回滚本轮对话；已执行的文件改动不撤销 |
| 网络错误诊断 | WinHTTP 错误码翻译为中文（DNS / 连接 / 超时 / SSL 证书） |
| 网络重试/退避 | 网络抖动（连接重置 / 超时 / 限流 / 5xx）自动指数退避；429 尊重 `Retry-After`；额度用完类错误（402/403/429+欠费报文）不重试；401/402 在界面显示友好中文结论，原始报文进日志；重试前回删已上屏的半截回复 |
| API Key 加密 | DPAPI（`CryptProtectData`）加密存储，明文不入盘 |
| 工作目录隔离 | 文件工具仅限工作目录内（路径强制解析），越界访问被拒绝；启动/切换工作目录时会校验目录确实存在；**不解析符号链接/junction**，工作目录内指向外部的链接可绕过该限制，需要真正隔离请用容器/虚拟机 |
| 命令执行超时 | `run_pipe` 默认 60s，超时经作业对象终止整个进程树（可用 `CAGENT_CMD_TIMEOUT` 覆盖） |
| 对话历史持久化 | 每轮结束保存到 `sessions\history_*.json`；压缩前归档 `*.pre_compact`；启动不自动加载 |
| RichEdit 显示 | 历史框用 `RICHEDIT50W`，按角色分色；emoji 区段显式指定 Segoe UI Emoji 字体 |
| 流式 SSE | LLM 响应逐字显示（`stream:true`），工具调用 delta 累积 |

---

## 工作原理（Agent 循环）

```text
┌─────────────────────────────────────────┐
│  messages = [system, user]              │
│  loop (无轮次上限, 由模型自行终止):     │
│    resp = HTTP POST chat/completions    │
│    if resp 含 "tool_calls":             │
│      执行每个工具                       │
│      把 assistant + tool 消息追加进     │
│      messages, 继续下一轮               │
│    else:                                │
│      打印/显示 resp.content, 结束       │
└─────────────────────────────────────────┘
```

`src/cagent_core.h` 中的 `agent_turn` / `agent_thread` 就是这个循环的实现，GUI 通过宿主钩子（`cagent_emit` 等）与之交互。

### 工具调用协议

采用**原生结构化 `tool_calls`**（非文本 `Action:` 解析），与 OpenAI 规范一致：

1. 请求体带 `tools` 声明（五个工具的名称、参数、描述）。
2. 流式响应中按 `delta.tool_calls[].index` 累积 `id` / `name` / `arguments` 片段。
3. 框架按 `name` 分发，`arguments` 二次 JSON 解析后执行。
4. 回填一条 `assistant`（含 `tool_calls` 数组，保留模型本轮的自然语言说明）与 N 条 `tool`（`tool_call_id` 与调用一一配对）消息，再进入下一轮。

结束与错误语义：

- 一轮结束原因取自 `finish_reason`：正常 `stop` / `tool_calls` 之外，`length`（长度截断）与 `content_filter`（内容过滤）会在界面明确提示。
- 工具失败不会中断循环：参数缺失、`arguments` 非法 JSON、未知工具等都会把可读的错误原因作为工具结果交回模型，由它决定重试或换策略。
- 未实现工具结果缓存：本地读写类工具结果随时可能变化，缓存会带来正确性风险。

---

## 常见问题

**Q: GUI 启动后输入框是空的？**
A: 首次启动 `cagent.ini` 不存在，请在界面中填入配置；关闭窗口后会自动保存。

**Q: 提示 `[HTTP 401]`？**
A: API Key 错误或未授权，检查 Key 字段。

**Q: 工具调用反复执行不停止？**
A: 循环无内置轮次上限，模型应自行终止；若陷入死循环，点“停止”会在下一个迭代间隙取消（取消是协作式的，已发出的请求无法中途打断）。

**Q: 长输出被截断？**
A: 对话上下文缓冲为 `BUFSZ=1MB`（单次 LLM 响应也受它约束）；单次工具输出默认上限 16KB，`read_file` 首读大文件只回 6KB 预览。可通过 `CAGENT_TOOL_OUTPUT_MAX` 调整工具输出上限，或按工具提示用 `offset` 继续读取。

**Q: GUI 不响应，看起来卡了？**
A: LLM 请求通常 1–5 秒，期间界面显示 `等待响应…` 但仍可拖动窗口；推理模型还会实时显示可折叠的思考过程。如真卡死请反馈。

**Q: 点了“停止”但还在转？**
A: 取消是协作式的：已发出的 HTTP 请求无法中途打断，会在下一个迭代间隙生效。若长时间无响应（如服务端不返回），等待超时后才会退出。

**Q: 模型还在跑，我能插话吗？**
A: 能。运行期间输入框照常可写，回车或点“发送”即把消息排队（按钮显示“发送 (N)”），不打断当前轮，当前轮结束后自动接着发出，最多 8 条。排队 vs 打断的取舍：回滚只还原对话，已落盘的文件改动不会复原，所以默认排队；真要打断请点“停止”（只停当前轮，排队消息仍会发出）。

**Q: 换了电脑/用户后 API Key 解密失败？**
A: Key 用 DPAPI 加密，绑定当前 Windows 用户。换机/换用户无法解密，程序会提示并清空 Key，重新填写即可。

**Q: 如何开新会话？如何回到旧会话？**
A: 点“新建会话”另起一个空对话（当前对话自动存盘保留，不删除任何文件）；点“加载会话”列出已保存的会话，列表按时间倒序，最新在最上面，选择后按角色回放并可继续对话。

**Q: 历史会话列表按什么排序？**
A: 按会话文件的最后修改时间倒序，最新的在最上面。每行两行紧凑显示：工作目录，以及消息条数 · 时间 · 首句预览。

**Q: 启动后怎么没有之前的对话？**
A: 启动不自动加载历史（全新对话）。需要继续之前的会话时，点“加载会话”选择即可。

---

## 版本号

版本号与对外文案的**唯一事实来源**是 `src/core/version.h`：

- `CAGENT_VER_MAJOR/MINOR/PATCH`：版本分量。
- `CAGENT_VERSION_STR`：由分量派生的人类可读版本串。
- `CAGENT_TAGLINE` / `CAGENT_PROJECT_URL` / `CAGENT_PROJECT_URL_GITEE`：对外文案与项目地址。

需要改版本号时只改这一处，其他位置通过宏引用，不重复硬编码。

**Windows 文件属性**：`res/app.rc` 的 `VERSIONINFO` 资源，右键 `cagent.exe` → 属性 → 详细信息可见（当前数值 `1.2.7.0` + 字符串 `1.2.7`）。

- 资源名必须用整数 `1`；写 `VS_VERSION_INFO` 会被 windres 当成字符串名，导致读取失败（错误 1813）。
- `app.rc` 里的 `#include "core/version.h"` 与 `ICON "app.ico"` 都相对自身目录解析，因此 Makefile 给 windres 传了 `-I src -I res`。
- Makefile 还给 windres 传了 `-c 65001`：`app.rc` 是 UTF-8（无 BOM），而 windres 默认按系统 ANSI 码页解释源码（中文 Windows 上是 936/GBK）。漏掉这个选项，非 ASCII 字符串会被按 GBK 拆成乱码写进资源，表现为文件属性里出现 `C 璇█鏋佺畝缂栫▼ Agent` 这类乱码。**新增非 ASCII 文本前请确认该选项仍在。**
- `FileDescription` 当前是纯英文（`cagent - a minimal AI coding agent in C`），作为额外的编码保险。

**命令行**：`cagent.exe --version` 打印 `cagent 1.2.7`（有父控制台则打印到终端，否则弹对话框）。

**「关于」**：主窗口系统菜单里的「关于(A)...」（点标题栏图标 / `Alt+Space` / 右键标题栏都能打开），显示一句话功能、版本号和两个项目地址（GitHub 与 Gitee 镜像，国内访问 gitee 更稳）。两个地址都可点击打开默认浏览器，正文也可用 `Ctrl+C` 整段复制。

> **窗口标题栏只写 `cagent`，不带版本号**——版本在「关于」和 exe 文件属性里就够；标题栏那串数字每次发版都得跟着改，对用户也没有信息量。
>
> **为什么「关于」不放标题栏问号按钮**：`WS_EX_CONTEXTHELP` 与 `WS_MINIMIZEBOX`/`WS_MAXIMIZEBOX` 互斥（MSDN 原文）。实测在带最小化/最大化的窗口上，加了这个扩展样式后问号根本不会被绘制（样式位却在，光看 `GetWindowLongPtr` 查不出来，必须看图）。另外问号按钮的语义是“上下文相关帮助”（点它再点某个控件才弹说明），本来就不是“关于”的入口。

---

## 界面实现细节与踩坑

### 控件外观与 `res/app.manifest`

`res/app.rc` 里有一行 `1 24 "app.manifest"`，把清单作为 **RT_MANIFEST**（类型必须写数字 `24`，ID 必须是 `1`）嵌进 exe。

- **为什么必须有**：不声明 `Microsoft.Windows.Common-Controls 6.0.0.0` 依赖的进程会拿到 comctl32 **v5**，控件就是 Windows 2000 那种经典外观（3D 凸起按钮、2px 凹陷输入框）。实测本机（Win11 build 26100）即使 exe 完全没有清单，也碰巧加载了 `WinSxS\...\comctl32 6.0.26100`——加清单前外观本来就是主题化的，但那是撞运气，换台机器可能静默退回经典外观且不报任何错。加清单是把“碰巧”变成“保证”，视觉上零变化。
- **怎么验证**：`FindResourceW(exe, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24))` 能取到即为已内嵌；进程里实际加载的 `comctl32.dll` 路径含 `WinSxS` 即为 v6。
- **别用 `PrintWindow` 判断外观**：它渲染控件时不走主题引擎，会把主题化的扁平按钮画成 v5 的 3D 凸起按钮。判断外观只能用屏幕 `BitBlt`，且必须确认窗口在最前。首次排查这个问题时就被它误导过。
- 清单里还声明了 DPI 感知（见下一节）：`<dpiAwareness>PerMonitorV2,PerMonitor</dpiAwareness>` 加 `<dpiAware>true/pm</dpiAware>`。放清单而不用 `SetProcessDpiAwarenessContext`，是因为加载器在任何代码运行之前就应用它，没有“调用失败被静默忽略”这种坑。

### 高 DPI（`src/ui/dpi.h`）

- **约定：所有布局常量与字号都按 96 DPI 的“逻辑像素”书写，一律经 `dp()`（主窗口）/ `dp_at()`（对话框）换算成物理像素。** 想调尺寸就改逻辑值，不要再往布局里塞裸像素数字。适用于 `wndproc.h`（主窗口）、`about.h`、`session_dlg.h` 三处布局。
- 字号同样要过换算：`CreateFontW` 的高度参数是“字符单元高度”的像素数，不跟着 DPI 走的话，在高缩放屏上字会小一半。
- 当前是 **`PER_MONITOR_AWARE_V2`**：进程活在“窗口当前所在显示器”的 DPI 里，每块屏都按原生分辨率绘制，跨屏不受系统位图拉伸。
- **代价：窗口跨屏时必须自己重算。** 收到 `WM_DPICHANGED` 要做齐四步，漏一步界面就会按旧 DPI 的尺寸画在新屏上：
  1. 取该窗口的新 DPI（`HIWORD(wParam)`，或用 `dpi_of_window()`）。
  2. 重建字体并下发给全部控件（`ui_fonts_rebuild`：建新的 → 下发 → 再删旧的）。
  3. 重设 RichEdit 的精确行距（`ui_history_parafmt`：`dyLineSpacing` 是绝对 twips，不随字体自动变；且段落属性只作用于选中范围，所以要先全选、改完还原选区）。
  4. 用 `AdjustWindowRectExForDpi` 反推外框 + `SetWindowPos` + 重新排布。
- 主窗口在 `wndproc.h` 里做齐了；两个对话框各有一份（`about_fonts_sync`/`about_layout`、`sess_fonts_sync`/`sess_create_list`/`sess_layout`）。
- **`g_dpi` 只属于主窗口。** 对话框可能被拖到别的显示器上，所以各自维护 `g_about_dpi` / `g_sess_dpi` 并用 `dp_at()` 换算，绝不能去写 `g_dpi`，否则主窗口下次 `layout()` 会用错比例。对话框字体也按自己的 DPI 从字体族直接建（`CAGENT_UI_FACE`），不克隆主窗口的 `g_hFont`。
- 会话列表的行高由 `WM_MEASUREITEM` 一次性决定（当前 `SESS_ROW_H = 46` 逻辑像素，两行文字基线在 3 / 24），而 ownerdraw 列表不接受 `LB_SETITEMHEIGHT` 改行高，所以 DPI 变化时只能重建列表控件（`sess_create_list`）。
- **DPI 感知由 `res/app.manifest` 声明，不在代码里设。** `src/ui/dpi.h` 只负责查询 DPI（`dpi_system` / `dpi_of_window`）和换算（`dp` / `dp_at`）——原来那条约 25 行的动态 `GetProcAddress` 降级链（`SetProcessDpiAwarenessContext` → `SetProcessDpiAwareness` → `SetProcessDPIAware`）已经删掉。清单由加载器在建进程前应用，运行时不能再改（这是好事）。
- **两行声明的分工**（版本门槛按官方文档，元素和“值”要分清）：
  - `<dpiAwareness>`（2016 命名空间）元素从 **Windows 10 1607** 起被识别，其中的 `permonitorv2` 值要 1703+；更早的版本按列表取下一个认得的项，所以写成 `PerMonitorV2,PerMonitor` 这种降级列表。
  - `<dpiAware>`（2005 命名空间）从 Vista 起就认，其中 `true/pm` 这个值要 **Windows 8.1**。Windows 10 1607 及以上会忽略它（`dpiAwareness` 覆盖 `dpiAware`）——它纯粹是给 Win8.1 ~ Win10 1607 那几年留的兜底。
  - 值只能是 `true` / `false` / `true/pm` / `per monitor`。写成别的字符串，在 Vista/7/8.0 上会变成不感知且不可运行时修改；而 `true/pm` 在那三个系统上只是退成系统级感知，所以用 `true/pm`——最坏情况最轻。
  - 实测：把 `dpiAware` 故意改成非法值后重编，进程仍是 PMv2、跨屏尺寸不变，证实这行在本机（Win11）完全没被读取。
- **历史 bug**：这里原先传的是 `-5` = `DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED`，名字像“感知”实际是不感知——界面被整体位图放大，且窗口物理尺寸随所在显示器变化（同一份 `720x560` 的请求，在 200% 屏上是 `1440x1120`、250% 屏上是 `1800x1400`）。这也解释了早期“本机缩放到底是 200% 还是 250%”两次测量对不上的原因：不是测量误差，是窗口落在了不同显示器上。

实测（把窗口在 200% 主屏与 250% 副屏之间来回移动）：

| 位置 | 客户区 | 第 1 个 Static |
|---|---|---|
| 主屏 200% | `1440x1120` = 720×2 | `@(16,24) 160x44` |
| 副屏 250% | `1800x1400` = 720×2.5 | `@(20,30) 200x55` |

来回切换可重复，数值精确到像素；关于对话框客户区 `840x360` = `dp(420)xdp(180)`，且关闭它不会带歪主窗口的比例。

### 单行输入框为什么是 22 逻辑像素高

**Win32 的单行 `EDIT` 没有垂直对齐的开关**——没有任何对应的 `ES_*` 样式，也没有 `EM_*` 消息（`EM_SETRECT` 只对多行框有效）。文字位置由系统决定：锚在“距控件顶约 0.28×字号”处，余下的空间全留在下方，所以框给高了就看着偏上。

实测（16 逻辑像素字号 @200%，逐行扫描字形的上下界）：

| 控件高 | 上留白 | 下留白 | 观感 |
|---|---|---|---|
| `dp(26)` = 52px | 9 | 17 | 偏上 |
| **`dp(22)` = 44px** | **9** | **9** | **居中** |

锚点与字形高都随字号等比缩放，所以 `dp(22)` 在任何 DPI 下都成立。`layout()` 里的 `row_h` 就是这个值，注释写了推导过程——**别随手改大**。

### 输出区的字号、行距、段落留白与折叠（RichEdit 的五个坑）

输出区（历史框）是 `RICHEDIT50W`。它有几条与直觉相反的行为，都踩过：

1. **`CHARFORMAT.yHeight` 是 em 高，而 `WM_SETFONT` 的 `CreateFontW(h)` 是字身格高。** 两者不是一回事：Microsoft YaHei 的“字身格 / em ≈ 1.27”，所以给一段文字写死 `yHeight` 会让它比默认字体大 27%；而行距是按默认字体算的，于是字的顶边被行盒削掉（实测 36px 字号下削 10px）。**结论：不要给 run 写死字号**（`helpers.h` 的 `do_append` 刻意不置 `CFM_SIZE`）。
2. **`WM_SETFONT` 会让 run 上的显式字号失效。** 所以一旦跨显示器触发 `WM_DPICHANGED` 里那轮字体重建，写死过字号的文字会从“大”跳回“默认”，看着像换了套排版。这也是输出区缩放走 `EM_SETZOOM`（只缩放显示、不碰字符格式）的原因。
3. **空字符串 `WM_SETTEXT` 会把段落格式一并复位**（实测行距会跳回 RichEdit 的自然行距，比我们设的松），而加载会话正是用 `SetWindowTextW(g_hHistory, L"")` 清屏的——所以清空之后必须重新调一次 `ui_history_parafmt()` 与 `ui_history_zoom_apply()`，并且把 `g_last_role`、折叠块索引一起复位。
4. **段落留白是“整段”属性，而多行内容里每一行都是独立段落。** 所以工具输出这种多行块必须把段前/段后距留 0，否则每一行都带上间距，整块会被拉散成稀疏的几行。块与块之间的间隔一律靠下一个块首行的段前距提供（见 `state.h` 的 `g_role_style`）。
5. **RichEdit 没有“块”的概念，也没有隐藏文本**（`CFE_HIDDEN` 在它这儿不生效）。所谓“折叠”只能是把正文从字符流里删掉、展开时再插回去——于是必须自己维护一张块位置索引表（`state.h` 的 `UiFoldBlock`），并在所有会改动内容的路径上同步它：流式追加、重试回删、清空、载入会话。三条容易写错的：
   - **位置一律用 cp**（`EM_SETSEL` / `EM_CHARFROMPOS` 的坐标系）。别和 `GetWindowTextLengthW` 混用——后者把每个段落标记按 CRLF 算两个字符，拿它当 cp 会偏大。所以 `g_stream_chars` 记的也是实际 cp 增量而不是 wbuf 的码元数。
   - **标题行长度必须恒定**。状态符号 `▾`/`▸` 都是单个 UTF-16 码元，替换后长度不变；换个多字符的图标，正文的起始位置就得跟着重算。
   - **展开时要用实际插入量回写 `body_len`**。`EM_GETTEXTRANGE` 把 CRLF 还原成单个 CR，读回来的长度会小于原来的 cp 长度，直接沿用旧值会让后续块的位置整体偏移。
   - **正文要按角色分段记录**（`UiFoldSeg`），不能当成一整块纯文本。工具块正文里参数行与输出块样式不同，重放时必须逐段用原角色写回——否则文本对得上、样式已经错了。

**行距**由 `ui_history_parafmt()` 用 `PARAFORMAT2` 的“精确”行距（rule 4）设定，值 = `dp(HIST_LPX_DEF + HIST_LINE_EXTRA)` 换算成 twips。两个要点：

- 用 rule 4 时，**行盒小于字形就会被削顶**，所以留了 `HIST_LINE_EXTRA = 1` 逻辑像素的余量（约 +5%）。设成正好等于字身格高就会出现用户反馈的“字的顶被削掉”。
- 换算**只用 `g_dpi` 这一个 DPI 来源**。曾经用 `GetDeviceCaps(hdc, LOGPIXELSY)` 去量，但那是错的：在 `WM_DPICHANGED` 里子控件的 DPI 上下文还没更新，那个 DC 报的仍是旧屏的 DPI，而 `dp()` 已经是新屏的——两个口径混用会让行距整体偏一个 DPI 比例（实测 200%→250% 时行距从应有的 47px 变成 60px）。

**段落留白与缩进**（段前/段后距、左缩进）同样是绝对 twips，不随字体缩放，所以跨显示器时也得重算——这件事由 `ui_para_reflow()` 做：逐段读出旧值、按 DPI 比例缩放后写回。它不需要知道每段是什么角色（缩放是线性的，各角色的相对关系自然保留），但必须只处理段落起点：`EM_GETLINECOUNT` / `EM_LINEINDEX` 数的是显示行，软换行也占一行，逐行缩放会把同一个段落的留白缩放多次。

---

## 许可

MIT License

Copyright (c) 2026 style7en

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
