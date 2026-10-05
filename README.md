# cagent

用 C 语言实现的极简 AI Agent,**零第三方依赖**,运行在 Windows 平台。Win32 GUI 版,核心 (`src/cagent_core.h`) 与界面 (`src/main_gui.c`) 分离,共用同一段 Agent 循环。

---

## 目录结构

```
cagent/
├── Makefile            # 构建脚本
├── LICENSE             # MIT 许可证
├── cagent.exe          # 编译产物 —— 注意: exe 所在目录就是"数据根"
├── cagent.ini          # 配置文件 (首次启动后自动生成; 必须与 exe 同目录)
├── SYSTEM_PROMPT       # 可选: 外置系统提示词 (存在则替代内置默认)
├── sessions/           # 历史会话 (运行时生成, 首次保存时自动创建; 旧版散落的 history_*.json 启动时自动搬入)
│                       #   xxx.json=当前, xxx.json.bak=上一轮, xxx.json.pre_compact=压缩前存档
├── log/                # 运行日志 + 失败请求留档 (log=1 时生成, 不入库)
├── src/                # 全部源码
│   ├── main_gui.c      # 界面聚合入口 (仅按依赖顺序 include ui/)
│   ├── cagent_core.h   # 核心聚合入口 (仅按依赖顺序 include core/)
│   ├── core/           # 与界面解耦的 Windows 核心, 按子功能拆分 (单 TU, 全 static)
│   │   ├── version.h   # 版本号单一事实来源 (RC / --version / 标题栏引用)
│   │   ├── base.h      # 平台头 / 角色常量 / 宿主钩子 / 输出封装
│   │   ├── utf8.h      # UTF-8 解码与字符边界 (全项目唯一实现)
│   │   ├── state.h     # 缓冲常量 / 全局状态 / 工具声明
│   │   ├── log.h       # 运行日志 / 失败请求留档 / 发送前 UTF-8 预检
│   │   ├── crypto.h    # DPAPI: API Key 加密存储
│   │   ├── json.h      # 轻量 JSON 解析器 / 转义 / 自测
│   │   ├── exec.h      # 命令执行: CreateProcess + 管道 + 超时
│   │   ├── net.h       # WinHTTP 与 SSE 流式解析
│   │   ├── encoding.h  # OEM(GBK) → UTF-8 转码
│   │   ├── workspace.h # 工作目录解析、越界限制、UTF-8 文件/路径
│   │   ├── tools.h     # 结构化工具: execute_bash / read_file / write_file / edit_file
│   │   ├── agent.h     # Agent 循环: LLM <-> 工具调用、取消与回滚
│   │   ├── session.h   # 会话持久化: 存取 / 回放 / 多会话命名
│   │   └── config.h    # ini 配置读写
│   └── ui/             # Win32 界面, 按子功能拆分
│       ├── state.h     # 控件 ID / 消息 / 全局句柄与字体
│       ├── hooks.h     # 宿主钩子实现
│       ├── helpers.h   # Edit 读写(UTF-8) + RichEdit 分角色追加与 Markdown 渲染
│       ├── task.h      # 启动一轮 Agent
│       ├── wndproc.h   # 布局 / 子类 / 主窗口过程
│       ├── dpi.h       # 高 DPI: 感知设定 + 统一缩放 dp() (布局与字号都经它换算)
│       ├── session_dlg.h # 会话选择对话框 (枚举 / 自绘列表 / 载入)
│       ├── about.h     # 关于对话框 (一句话功能 / 版本 / 两个项目地址, 地址可点)
│       └── main.h      # 程序入口
└── test/               # 回归测试 (与 src/ 完全分开, 产品二进制不带测试代码)
    ├── test.h          # 测试套件本体 run_all_tests + json_selftest
    └── main.c          # 控制台入口 (make test)
└── res/                # 资源
    ├── app.rc          # 图标 + 版本信息 + 应用程序清单
    ├── app.ico
    └── app.manifest    # comctl32 v6 依赖声明 (控件现代视觉样式)
```

> 拆分方式: `src/core/` 与 `src/ui/` 下的头文件全部是 `static` 实现, 由聚合头按依赖顺序
> include, 仍是**单编译单元**, 无额外链接步骤, 也不引入任何第三方依赖。

> 为什么编译产物留在仓库根、而不收进 `build/`: 程序把 **exe 所在目录当作数据根**
> (见 `src/core/session.h` 的 `get_exe_dir_utf8`), 用它定位 `cagent.ini` / `sessions\` /
> `log\` / `SYSTEM_PROMPT`。exe 一旦挪进子目录, 这些都会跟着跑过去。

> 会话文件是**原子写**的: 先写 `xxx.json.tmp`, 内容完整落盘后再用 `MoveFileEx` 整体替换。
> 进程被杀、磁盘写满都不会把已有历史截断成半截 —— 旧文件在新内容落盘前一个字节都不动。
> 在此之上, `xxx.json.bak` 存上一轮的完整历史供回退, `xxx.json.pre_compact` 是上下文压缩
> 前的存档; 两者都只有一代, 且主文件被删后会在下次启动时作为孤儿清掉。

---

## 编译

### 环境要求

- Windows 7 及以上 (推荐 Win10 1809+,DPI 适配更佳)
- MinGW-w64 (GCC 13+) 或同等 C 编译器
- 标准 `make`

### 一键编译

```bash
make            # 构建 cagent.exe
make clean      # 删除可执行文件
```

或单独构建:

```bash
make cagent.exe
```

---

## 测试

项目带一套**离线自动化回归测试**(`test/test.h` 的 `run_all_tests`),覆盖 JSON 解析/转义、
编码检测与字符边界、工具分发与参数校验、`edit_file` 定点编辑的各类边界、HTTP 重试判定、
上下文压缩(消息遍历 / 保底丢弃的配对完整性 / 超限 400 识别 / token 与字节水位)、
`read_file` 的 offset 分块续读、外置系统提示词的转义包装与无效回退。无需网络或 GUI,改动核心代码后跑一遍即可当作护栏。

```bash
make test          # 编译 cagent_test.exe 并运行, 退出码 = 失败数 (0 即通过)
```

> **测试代码全部独立在 `test/`, 产品二进制一行都带。** 它不进 `cagent_core.h` 的聚合,
> 只有 `test/main.c` 会 include —— 所以 `cagent.exe` 里没有测试代码, 构建时也不需要
> 额外的 `-I`。测试直接复用 `src/cagent_core.h` 的全部 `static` 实现,因此测的就是真实
> 代码路径,而非另写一份桩。
>
> 历史上还有个 `cagent.exe --selftest` 入口(把结果写进 `selftest.txt`): 它存在的唯一理由
> 是 GUI 子系统没有控制台、`printf` 没地方去, 只能用文件中转 —— 代价是逼着产品二进制
> 带上整套测试代码(约 20KB)并给 GUI 编译加 `-I.`。已移除; 要验二进制就用 `make test`。

---

## GUI 版: `cagent.exe`

聊天式对话窗口,**支持运行时配置、配置持久化、HTTPS 直连、多轮工具调用可视化**。

### 启动

直接双击 `cagent.exe`,或:

```bash
./cagent.exe
```

### 界面布局

```
+-----------------------------------+
| Base Url: [ https://...v1     ]   |  ← 三项配置, 启动时自动从 cagent.ini 读取
| Key:      [ ********          ]   |  ← API Key 以 * 隐藏显示
| Model:    [ deepseek-v4-flash ]   |
| 工作目录: [ C:\...            ] [浏览...] [新建会话] [加载会话] |
+-----------------------------------+
|                                   |
|  >>> 你的问题                     |
|  (thinking...)                    |
|  [Tool] execute_bash({...})       |  ← 工具调用过程可视化
|  [Output] ...                     |
|  AI 回复                          |
|                                   |
+-----------------------------------+
| [输入框]              [ 发送 ]   |
+-----------------------------------+
```

**配色**:输出区底色 `RGB(247,247,247)`(`state.h` 的 `HIST_BG`),比输入框的系统白略深一档,让"只读输出"与"可编辑输入"一眼分层。247 不是随手取的——输出区里最浅的文字是系统提示的 `RGB(110,110,110)`,它把底色能压到的下限锁住了:再深到窗口自身的 `COLOR_BTNFACE`(240)时,那行灰字的对比度会掉到 4.47:1,**低于 WCAG AA 的 4.5:1**。

### 配置说明

| 字段 | 说明 |
|---|---|
| **Base Url** | 填到 `/v1` 即可,程序自动追加 `/chat/completions`<br>例: `https://token.sensenova.cn/v1` |
| **Key** | OpenAI 兼容的 API Key,显示为 `*` |
| **Model** | 模型 ID,例: `deepseek-v4-flash` |
| **skip_cert_verify** | 写在 `cagent.ini` 中,`1`=跳过 SSL 证书校验(自签端点用,默认 `0` 严格) |

### 配置文件 `cagent.ini`

- **位置**:与 `cagent.exe` 同目录
- **编码**:UTF-8
- **行为**:
  - 程序启动时自动读取(若文件不存在则跳过)
  - 程序关闭时自动把当前三个输入框的值写回
- **格式**:
  ```ini
  # cagent 配置 (UTF-8, 退出时自动保存)
  url_base=https://token.sensenova.cn/v1
  api_key=dpapi:<DPAPI 加密的 base64>
  model=deepseek-v4-flash
  skip_cert_verify=0
  context_tokens=131072
  workspace=C:\path\to\workspace
  skills_dir=D:\my\skills
  last_session=sessions\history_<工作目录>_<哈希>.json
  log=1
  ```

  - `context_tokens`:模型上下文窗口(token 数),如 131072 (128K)、1000000 (1M)。
    配置后按服务端报告的 `prompt_tokens` 精确控制水位(到 80% 触发压缩);
    `0` 或不写 = 未知,退回按字节水位。
  - `skills_dir`:技能根目录(见下文"技能 `skills\`"),可用 `;` 分隔配置**多个**目录
    (同 Windows PATH 风格,按顺序扫描,重名先到先得)。相对值按 **exe 目录**解析
    (与 `workspace` 同一规矩);不写 = 默认 `<exe>\skills`。路径含空格**直接写**,
    不需要引号(写了成对引号也会被剥掉);因 `;` 是分隔符,路径里无法包含 `;`。
  - `log`:运行日志开关,默认 `1`。写 `0` 关闭;日志在 `log\` 目录按天一个文件,
    HTTP 失败时完整请求体另存为 `log\request_fail_N.json`,可直接对服务端重放复现。

### 外置系统提示词 `SYSTEM_PROMPT`

- **位置**:与 `cagent.exe` 同目录,文件名 `SYSTEM_PROMPT`(无扩展名)
- **内容**:纯文本,作为 system 提示词的正文;引号/换行由程序自动转义成 JSON
- **编码**:UTF-8 优先;非 UTF-8(如中文 Windows 的 GBK/ANSI 另存)自动按系统代码页转换
- **行为**:启动时读一次;文件不存在 / 为空 / 超过 32KB 时静默回退内置默认提示词

### 技能 `skills\`

把常用任务的"操作说明书"预置成技能,模型在任务匹配时自动加载并照着执行:

- **位置**:默认与 `cagent.exe` 同目录的 `skills\`;可在 `cagent.ini` 里用 `skills_dir=`
  改成任意目录(相对值按 exe 目录解析),**递归扫描目录树,任意深度的 `SKILL.md` 都算
  一个技能**(目录里放 `SKILL.md` 即成,不要求固定层级;跳过点开头目录与 junction/链接,
  防环),缺省技能名取 `SKILL.md` 所在目录名:

  ```
  cagent.exe
  skills\
    ├─ build\
    │    └─ SKILL.md     ← 技能正文(说明/步骤/约束)
    └─ deploy\
         └─ SKILL.md
  ```

- **SKILL.md 格式**:正文为纯文本(会被整体转义进上下文);可选 frontmatter 声明元数据:

  ```markdown
  ---
  name: build
description: 编译并测试本项目
  ---
  1. 先跑 make clean && make
  2. ...
  ```

  缺 `name` 时用目录名,缺 `description` 时用正文第一个非空行。
- **生效方式**:启动时扫描一次并生成索引,**追加到 system 提示词尾部**(外置 SYSTEM_PROMPT
  同样会拼上),告诉模型有哪些技能、何时该用 `load_skill`;模型调用 `load_skill(name)`
  后返回完整正文。未知名会返回可用列表,由模型自行纠正。`skills_dir` 可配 `;` 分隔的
  多个根目录,按顺序扫描、重名先到先得(同一技能名全局唯一,先扫到的生效)。
- **上限**:最多 32 个技能;单个 `SKILL.md` 不超过 32KB(超限不加载);编码 UTF-8 优先,
  非 UTF-8 自动按系统代码页转换(中文 Windows 即 GBK),UTF-8 BOM 自动剥除。技能集进程内固定,新增技能**重启生效**。
- 环境变量 `CAGENT_SKILLS_DIR` 可把技能根目录指到别处(优先级低于 ini 的 `skills_dir`;
  测试/临时切换用)。

### 操作

- **新建会话**:当前对话自动落盘保留,另起一个空对话(文件不删除,可随时加载回)
- **加载会话**:列出已保存的会话(目录 + 条数 + 时间 + 首句预览),选中后回放到界面并继续
- **回车**发送(等同点击"发送"按钮)
- **Ctrl + 滚轮**:调整输出区字号(等效 12~40 逻辑像素,默认 18)。窗口内任意位置都生效,只影响输出区、不影响配置框。缩放在内存里,重启回到默认值。滚轮消息是按**焦点窗口**投递的,所以三个地方都挂了同一个处理(主窗口 / 输出框 / 输入框子类),否则焦点在输入框时会失灵
- **Ctrl + 滚轮的实现是 `EM_SETZOOM`**(整体缩放显示),不是去改文字的字号 —— 这样行距会跟着等比缩放,也不会在跨显示器时因为字体被重建而跳变。详见 `wndproc.h` 的 `ui_history_zoom`
- **运行中取消**:Agent 调用期间"发送"按钮变为"停止",点击即请求取消;取消在下一个安全点(下一轮迭代前或工具执行前)生效,正在进行的 HTTP 请求无法立即打断
- **流式显示**:LLM 回复逐字流式显示(纯文本),按角色着色(你=蓝加粗、AI=黑、思考/工具提示=灰);emoji 以单色字形显示
- 模型调用期间输入框自动禁用,完成后自动恢复
- 长内容自动换行,自动滚动到底部

### 技术特性

| 特性 | 实现 |
|---|---|
| HTTPS 网络 | **WinHTTP** (Windows 内置,无外部依赖) |
| UI 不卡死 | LLM 调用走后台线程,UI 主线程 `PostMessage` 接收追加事件 |
| 高 DPI 适配 | **PerMonitorV2**（声明在 `res/app.manifest`，每块显示器按原生分辨率绘制）+ 全部布局与字号经 `dp()` 换算 |
| 中文显示 | 全程 UTF-8 ↔ UTF-16 转换,字体 Microsoft YaHei UI |
| 静默工具执行 | `CreateProcess + CREATE_NO_WINDOW + 匿名管道`,不弹 cmd 黑框 |
| 纯内存 IO | 无 `req.json`/`resp.json` 临时文件 |
| 配置持久化 | 启动加载/退出保存 `cagent.ini` |
| 协作式取消 | Agent 运行时"发送"按钮变"停止",点击后在迭代间隙优雅退出并回滚本轮**对话**;已执行的文件改动不会撤销,回滚提示会列出改过哪些文件 |
| 网络错误诊断 | WinHTTP 错误码翻译为中文(DNS/连接/超时/SSL 证书),便于排错 |
| 网络重试/退避 | 网络抖动(连接重置/超时/限流/5xx)自动指数退避重试,避免整轮对话回滚;重试提示**按错误类别区分**(网络瞬时错误/请求限流/服务端错误);429 用更长退避并尊重服务端 `Retry-After`;**额度用完类错误(402/403/429+欠费报文)不重试**;鉴权(401)/额度(402)在界面显示友好中文结论,原始报文进日志;取消与 4xx 客户端错误不重试;重试前**回删已上屏的半截回复**,界面不会出现两段重复内容 |
| API Key 加密 | DPAPI (`CryptProtectData`) 加密存储 `cagent.ini` 中的 Key,明文不入盘 |
| 工作目录隔离 | 文件工具仅限工作目录内(路径强制解析),越界访问被拒绝;**不解析符号链接/junction**,工作目录内指向外部的链接可绕过该限制,需要真正隔离请用容器/虚拟机;启动前会校验工作目录确实存在 |
| 命令执行超时 | `run_pipe` 默认 60s,超时经作业对象终止整个进程树,避免长时间卡死(可用 `CAGENT_CMD_TIMEOUT` 以秒覆盖) |
| 对话历史持久化 | 每轮 done 后按工作目录存到 `sessions\history_*.json`(不污染根目录);上下文压缩前自动归档 `*.pre_compact` 保留完整原始记录;启动不自动加载,通过"加载会话"按钮按需载入并按角色回放到界面 |
| RichEdit 显示 | 历史框用 RICHEDIT50W,按角色分色;emoji 区段显式指定 Segoe UI Emoji 字体 |
| 流式 SSE | LLM 响应逐字流式显示(`stream:true`),工具调用 delta 累积 |

### 工具

目前内置(对齐 pi.dev 的极简理念,核心只保留 4 个基础工具 + 1 个技能加载工具):

- `execute_bash(command)` — 执行 shell 命令并捕获输出(不拦截,由用户自担风险);
  默认超时 60s(超时杀掉整个进程树),输出超限自动截断并附提示;
  输出末尾附 `[exit=N]` **退出码**,模型据此判断命令成败;`stdin` 给 `NUL`(读到即 EOF),
  交互式命令跑不起来而不是挂到超时
- `read_file(path, offset?)` — 读取文件文本内容;非 UTF-8(如 GBK)文本会自动转成 UTF-8;
  超过上限时截断并告知文件总大小与**下一次的 offset**,带 offset 可分块续读
  (起点落在多字节字符中间时自动对齐到字符边界)
- `edit_file(path, old_text, new_text)` — **定点替换编辑**(改局部内容的首选):`old_text`
  必须与原文完全一致且在文件中**唯一出现**(按非重叠出现计数),否则不改动文件并说明原因;
  改一行无需重写全文,非 UTF-8(GBK)文件会被拒绝并提示改用 `write_file`
- `write_file(path, content)` — 写入文件(直接覆盖,无确认);父目录需已存在
- `load_skill(name)` — 按名称加载用户预置的技能说明(见上文"技能 `skills\`"),
  返回文本不执行任何东西;任务与某个技能相关时由模型主动调用并照说明执行

> 没有专门的列目录/搜索工具:查看目录用 `dir`,递归搜索内容用 `findstr /s /i "关键词" *.*`,都走 `execute_bash`。

**高危命令需用户确认**(由系统提示词约束模型行为,框架不做技术拦截):

- 调用 `execute_bash` 前,模型须先用一句话说明要做什么和风险,然后**停下来等用户确认**;
  确认之前不执行——此时该轮自然结束,用户回复「确认」后模型再执行。
- 高危范围:删除或覆盖文件(`del`、`rd`、`rmdir /s`、`format`、`diskpart`)、
  改注册表或系统服务(`reg`、`sc`、`net user`、`schtasks`)、关机重启、
  批量移动或重命名、下载后直接执行外部脚本、改写 git 历史(`git push -f`、`reset --hard`)、
  以及任何写入工作目录之外位置的操作。
- 列目录、读文件等只读操作不受影响;用户同意后同类操作不重复询问,用户拒绝则换更安全的方案。
- 注意:这是**提示词层面的约束**,不是沙箱。真正隔离请在容器/虚拟机中运行本程序。

**结果上限与调参**(避免单次工具输出撑爆上下文):

| 环境变量 / 常量 | 默认 | 说明 |
|---|---|---|
| `CAGENT_TOOL_OUTPUT_MAX` | 16384 (16KB) | 单次工具结果进入上下文的上限,超出即截断并附提示 |
| `CAGENT_CMD_TIMEOUT` | 60 (秒) | 命令执行超时,超时终止整个进程树 |
| `EDIT_MAX_BYTES` | 2MB | `edit_file` 可处理的单文件体积上限 |
| `CAGENT_HTTP_RETRIES` | 3 | 单次 LLM 请求的最大重试次数(仅对瞬时可恢复错误:网络层错误 / 408 / 429 / 5xx) |
| `CAGENT_HTTP_RETRY_MS` | 2000 (毫秒) | 网络类错误首次退避基数,后续按 2 倍指数增长(2 → 4 → 8 …,单次上限 60s);429 限流单独用 5s 基数(5 → 10 → 20 …),服务端 `Retry-After` 更长时以其为准(上限 120s) |

**对话上下文缓冲**为 1MB(编译期常量 `BUFSZ`,约 30 万+ 汉字);配合 `context_tokens`
配置可实现按模型真实窗口的 token 级水位控制(见上文"上下文管理")。

**上下文管理**:对话上下文缓冲 1MB,水位与压缩分三级处理:

- **水位判定**:在 `cagent.ini` 里配置 `context_tokens`(模型上下文窗口,token 数)后,
  优先用服务端流式响应里的 `usage.prompt_tokens` 精确估算(增量部分按约 3 字节/token 折算),
  到窗口的 80% 触发压缩;未配置或服务端不发 usage 时,退回字节水位(75% 缓冲)。
- **压缩时机**:每轮用户消息开始前**和长工具链任务的每次迭代间隙**都会检查——
  单轮内多次工具调用累积的上下文也会被及时压缩,而不是等到下一轮。
- **压缩方式(阶梯降级)**:先把较早的对话请模型压成交接摘要(≤2000 字,保留目标、
  步骤与结论、文件路径、待办),**最近的若干条消息原样保留**在摘要之后;
  摘要请求失败则减半重试;仍失败则不经模型、按整条消息丢弃最旧部分(不拆散
  tool_call 与结果配对)。压缩前当前会话文件自动归档为 `*.pre_compact`,
  完整原始记录不依赖单代备份。
- **超限自愈**:服务端返回"上下文超限"类 400 时,自动压缩后原地重试(有次数上限),
  而不是回滚丢掉整轮;压缩彻底失败时,轮首另起新会话、轮中保留已完成的工作提前收束。

---

## 工作原理(Agent 循环)

```
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

`src/cagent_core.h` 中的 `agent_turn` / `agent_thread` 就是这个循环的实现,GUI 通过宿主钩子 (`cagent_emit` 等) 与之交互。

### 工具调用协议

采用**原生结构化 tool_calls**(非文本 `Action:` 解析),与 OpenAI 规范一致:

1. 请求体带 `tools` 声明(三个函数的名称/参数/描述);
2. 流式响应中按 `delta.tool_calls[].index` 累积 `id` / `name` / `arguments` 片段;
3. 框架按 `name` 分发,`arguments` 二次 JSON 解析后执行;
4. 回填一条 `assistant`(含 `tool_calls` 数组, **保留模型本轮的自然语言说明**)
   与 N 条 `tool`(`tool_call_id` 与调用一一配对)消息,再进入下一轮。

结束与错误语义:

- 一轮结束原因取自 `finish_reason`:正常 `stop` / `tool_calls` 之外,`length`(长度截断)
  与 `content_filter`(内容过滤)会在界面明确提示;
- 工具失败不会中断循环:参数缺失、`arguments` 非法 JSON、未知工具等都会把
  **可读的错误原因**作为工具结果交回模型,由它决定重试或换策略;
- 未实现工具结果缓存:本地读写类工具结果随时可能变化,缓存会带来正确性风险。

---

## 常见问题

**Q: GUI 启动后输入框是空的?**
A: 首次启动 `cagent.ini` 不存在,请在三个 Edit 中填入配置;关闭窗口后会自动保存到 ini。

**Q: 提示 `[HTTP 401]`?**
A: API Key 错误或未授权,检查 Key 字段。

**Q: 工具调用反复执行不停止?**
A: 循环无内置轮次上限,模型应自行终止;若陷入死循环,点"停止"会在下一个迭代间隙取消(取消是协作式的,已发出的请求无法中途打断)。

**Q: 长输出被截断?**
A: 缓冲区上限 `BUFSZ=1MB`(对话上下文/单次 LLM 响应),单次工具输出另有 16KB 上限,按需调整。

**Q: GUI 不响应,看起来卡了?**
A: LLM 请求通常 1-5 秒,期间界面显示 `(thinking...)` 但仍可拖动窗口。如真卡死请反馈。

**Q: 点了"停止"但还在转?**
A: 取消是协作式的:已发出的 HTTP 请求无法中途打断,会在下一个迭代间隙生效。若长时间无响应(如服务端不返回),等待超时后才会退出。

**Q: 换了电脑/用户后 API Key 解密失败?**
A: Key 用 DPAPI 加密,绑定当前 Windows 用户。换机/换用户无法解密,程序会提示并清空 Key,重新填写即可。

**Q: 如何开新会话? 如何回到旧会话?**
A: 点"新建会话"另起一个空对话(当前对话自动存盘保留,不删除任何文件);点"加载会话"列出已保存的会话,选择后按角色回放到界面并可继续对话。每个工作目录支持多个会话文件。

**Q: 启动后怎么没有之前的对话?**
A: 启动不自动加载历史(全新对话)。需要继续之前的会话时,点"加载会话"选择即可。

---

## 版本号

版本号与对外文案的**唯一事实来源**在 `src/core/version.h`（`CAGENT_VER_MAJOR/MINOR/PATCH`、`CAGENT_VERSION_STR`；另有 `CAGENT_TAGLINE` / `CAGENT_PROJECT_URL` / `CAGENT_PROJECT_URL_GITEE`），改动只改这一处，三处同步引用：

- **Windows 文件属性**：`res/app.rc` 的 `VERSIONINFO` 资源，右键 `cagent.exe` → 属性 → 详细信息可见（当前数值 `1.2.2.0` + 字符串 `1.2.2`）。注意资源名必须用整数 `1`，写 `VS_VERSION_INFO` 会被 windres 当成字符串名导致读取失败（错误 1813）。
  - `app.rc` 里的 `#include "core/version.h"` 与 `ICON "app.ico"` 都相对自身目录解析，故 Makefile 给 windres 传了 `-I src -I res`。
  - Makefile 还给 windres 传了 `-c 65001`：`app.rc` 是 UTF-8(无 BOM)，而 windres 默认按**系统 ANSI 码页**解释源码（中文 Windows 上是 936/GBK）。漏了这个选项，非 ASCII 字符串会被按 GBK 拆成乱码写进资源 —— 表现为文件属性里出现 `C 璇█鏋佺畝缂栫▼ Agent` 这类乱码。**新增非 ASCII 文本前请确认该选项仍在。**

### 控件外观与 `res/app.manifest`

`res/app.rc` 里有一行 `1 24 "app.manifest"`，把清单作为 **RT_MANIFEST**（类型必须写数字 `24`，ID 必须是 `1`）嵌进 exe。

- **为什么必须有**：不声明 `Microsoft.Windows.Common-Controls 6.0.0.0` 依赖的进程会拿到 comctl32 **v5**，控件就是 Windows 2000 那种经典外观（3D 凸起按钮、2px 凹陷输入框）。实测本机（Win11 build 26100）**即使 exe 完全没有清单，也碰巧加载了 `WinSxS\...\comctl32 6.0.26100`** —— 也就是说加清单前外观本来就是主题化的，但那是撞运气，换台机器可能静默退回经典外观且不报任何错。加清单是把"碰巧"变成"保证"，**视觉上零变化**。
- **怎么验证**：`FindResourceW(exe, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24))` 能取到即为已内嵌；进程里实际加载的 `comctl32.dll` 路径含 `WinSxS` 即为 v6。
- **⚠️ 别用 `PrintWindow` 判断外观**：它渲染控件时**不走主题引擎**，会把主题化的扁平按钮画成 v5 的 3D 凸起按钮。判断外观只能用屏幕 `BitBlt`（且必须确认窗口在最前）。首次排查这个问题时就被它误导过。
- 清单里**还声明了 DPI 感知**（见下一节）：`<dpiAwareness>PerMonitorV2,PerMonitor</dpiAwareness>` 加 `<dpiAware>true/pm</dpiAware>`。放清单而不用 `SetProcessDpiAwarenessContext`，是因为加载器在**任何代码运行之前**就应用它，没有"调用失败被静默忽略"这种坑 —— 历史上正是那么栽的（见下节的历史 bug）。

### 高 DPI（`src/ui/dpi.h`）

- **约定：所有布局常量与字号都按 96 DPI 的"逻辑像素"书写，一律经 `dp()`（主窗口）/ `dp_at()`（对话框）换算成物理像素。** 想调尺寸就改逻辑值，不要再往布局里塞裸像素数字。适用于 `wndproc.h`（主窗口）、`about.h`、`session_dlg.h` 三处布局。
- 字号同样要过换算：`CreateFontW` 的高度参数是"字符单元高度"的**像素数**，不跟着 DPI 走的话，在高缩放屏上字会小一半。
- 当前是 **`PER_MONITOR_AWARE_V2`**：进程活在"窗口当前所在显示器"的 DPI 里，**每块屏都按原生分辨率绘制**，跨屏不受系统位图拉伸。
- **代价：窗口跨屏时必须自己重算。** 收到 `WM_DPICHANGED` 要做齐四步，漏一步界面就会按旧 DPI 的尺寸画在新屏上（比"不感知"更糟 —— 尺寸直接错）：
  1. 取该窗口的新 DPI（`HIWORD(wParam)`，或用 `dpi_of_window()`）
  2. 重建字体并下发给全部控件（`ui_fonts_rebuild`：**建新的 → 下发 → 再删旧的**）
  3. 重设 RichEdit 的精确行距（`ui_history_parafmt`：`dyLineSpacing` 是**绝对 twips**，不随字体自动变；且段落属性只作用于选中范围，所以要先全选、改完还原选区）
  4. 用 `AdjustWindowRectExForDpi` 反推外框 + `SetWindowPos` + 重新排布

  主窗口在 `wndproc.h` 里做齐了；两个对话框各有一份（`about_fonts_sync`/`about_layout`、`sess_fonts_sync`/`sess_create_list`/`sess_layout`）。
- **`g_dpi` 只属于主窗口。** 对话框可能被拖到别的显示器上，所以各自维护 `g_about_dpi` / `g_sess_dpi` 并用 `dp_at()` 换算，**绝不能去写 `g_dpi`** —— 否则主窗口下次 `layout()` 会用错比例。对话框的字体也按自己的 DPI 从字体族直接建（`CAGENT_UI_FACE`），不克隆主窗口的 `g_hFont`。
- 会话列表的行高由 `WM_MEASUREITEM` 一次性决定，而 ownerdraw 列表**不接受** `LB_SETITEMHEIGHT` 改行高，所以 DPI 变化时只能重建列表控件（`sess_create_list`）。
- **感知由 `res/app.manifest` 声明，不在代码里设**。`src/ui/dpi.h` 只负责查询 DPI（`dpi_system` / `dpi_of_window`）和换算（`dp` / `dp_at`）—— 原来那条约 25 行的动态 `GetProcAddress` 降级链（`SetProcessDpiAwarenessContext` → `SetProcessDpiAwareness` → `SetProcessDPIAware`）已经删掉。清单由加载器在建进程前应用，**运行时不能再改**（这是好事）。
- **两行声明的分工**（版本门槛按官方文档，元素和"值"要分清）：
  - `<dpiAwareness>`（2016 命名空间）**元素**从 **Windows 10 1607** 起被识别，其中的 `permonitorv2` **值**要 1703+；更早的版本按列表取下一个认得的项，所以我们写的是 `PerMonitorV2,PerMonitor` 这种降级列表。
  - `<dpiAware>`（2005 命名空间）从 **Vista** 起就认，其中 `true/pm` 这个**值**要 **Windows 8.1**。**Windows 10 1607 及以上会忽略它**（`dpiAwareness` 覆盖 `dpiAware`）—— 它纯粹是给 Win8.1 ~ Win10 1607 那几年留的兜底。
  - 值只能是 `true` / `false` / `true/pm` / `per monitor`。写成别的字符串，在 Vista/7/8.0 上会变成**不感知且不可运行时修改**；而 `true/pm` 在那三个系统上只是退成系统级感知。所以用 `true/pm` —— 最坏情况最轻。
  - 实测：把 `dpiAware` 故意改成非法值后重编，进程**仍是 PMv2**、跨屏尺寸不变，证实这行在本机（Win11）完全没被读取。
- **历史 bug**：这里原先传的是 `-5` = `DPI_AWARENESS_CONTEXT_UNAWARE_GDISCALED`，名字像"感知"实际是**不感知** —— 界面被整体位图放大，且窗口物理尺寸随所在显示器变化（同一份 `720x560` 的请求，在 200% 屏上是 `1440x1120`、250% 屏上是 `1800x1400`）。这也解释了早期"本机缩放到底是 200% 还是 250%"两次测量对不上的原因：不是测量误差，是窗口落在了不同显示器上。

**实测（把窗口在 200% 主屏与 250% 副屏之间来回移动）**：

| 位置 | 客户区 | 第 1 个 Static |
|---|---|---|
| 主屏 200% | `1440x1120` = 720×2 | `@(16,24) 160x44` |
| 副屏 250% | `1800x1400` = 720×2.5 | `@(20,30) 200x55` |

来回切换可重复，数值精确到像素；关于对话框客户区 `840x360` = `dp(420)xdp(180)`，且关闭它不会带歪主窗口的比例。

### 单行输入框为什么是 22 逻辑像素高

**Win32 的单行 `EDIT` 没有垂直对齐的开关** —— 没有任何对应的 `ES_*` 样式，也没有 `EM_*` 消息（`EM_SETRECT` 只对多行框有效）。文字位置由系统决定：**锚在"距控件顶约 0.28×字号"处，余下的空间全留在下方**，所以框给高了就看着偏上。

实测（16 逻辑像素字号 @200%，逐行扫描字形的上下界）：

| 控件高 | 上留白 | 下留白 | 观感 |
|---|---|---|---|
| `dp(26)` = 52px | 9 | 17 | 偏上 |
| **`dp(22)` = 44px** | **9** | **9** | **居中** |

锚点与字形高都随字号等比缩放，所以 `dp(22)` 在任何 DPI 下都成立。`layout()` 里的 `row_h` 就是这个值，注释写了推导过程 —— **别随手改大**。
  - `FileDescription` 当前是纯英文（`cagent - a minimal AI coding agent in C`），一个额外的保险。
- **命令行**：`cagent.exe --version` 打印 `cagent 1.2.2`（有父控制台则打印到终端，否则弹对话框）。
- **「关于」**：主窗口**系统菜单**里的「关于(A)...」（点标题栏图标 / `Alt+Space` / 右键标题栏都能打开），弹出对话框显示一句话功能 + 版本 + 两个项目地址（GitHub 与 Gitee 镜像，国内访问 gitee 更稳）。文案取自 `CAGENT_TAGLINE` / `CAGENT_VERSION_STR` / `CAGENT_PROJECT_URL` / `CAGENT_PROJECT_URL_GITEE`，两个地址都可点击打开默认浏览器，正文也可用 `Ctrl+C` 整段复制。

> **窗口标题栏只写 `cagent`，不带版本号** —— 版本在「关于」和 exe 文件属性里就够；标题栏那串数字每次发版都得跟着改，对用户也没有信息量。
>
> **为什么「关于」不放标题栏问号按钮**：`WS_EX_CONTEXTHELP` 与 `WS_MINIMIZEBOX`/`WS_MAXIMIZEBOX` 互斥（MSDN 原文）。实测在带最小化/最大化的窗口上，加了这个扩展样式**问号根本不会被绘制**（样式位却在，光看 `GetWindowLongPtr` 查不出来，必须看图）。另外问号按钮的语义是「上下文相关帮助」（点它再点某个控件才弹说明），本来就不是「关于」的入口。

### 输出区的字号与行距（RichEdit 的三个坑）

输出区（历史框）是 `RICHEDIT50W`。它有几条与直觉相反的行为，都踩过：

1. **`CHARFORMAT.yHeight` 是 em 高，而 `WM_SETFONT` 的 `CreateFontW(h)` 是字身格高。** 两者不是一回事：Microsoft YaHei 的「字身格 / em ≈ 1.27」，所以给一段文字写死 `yHeight` 会让它比默认字体**大 27%**；而行距是按默认字体算的，于是字的顶边被行盒削掉（实测 36px 字号下削 10px）。**结论：不要给 run 写死字号**（`helpers.h` 的 `do_append` 刻意不置 `CFM_SIZE`）。
2. **`WM_SETFONT` 会让 run 上的显式字号失效。** 所以一旦跨显示器触发 `WM_DPICHANGED` 里那轮字体重建，写死过字号的文字会从"大"跳回"默认"，看着像换了套排版。这也是输出区缩放走 `EM_SETZOOM`（只缩放显示、不碰字符格式）的原因。
3. **空字符串 `WM_SETTEXT` 会把段落格式一并复位**（实测行距会跳回 RichEdit 的自然行距，比我们设的松），而加载会话正是用 `SetWindowTextW(g_hHistory, L"")` 清屏的 —— 所以清空之后必须重新调一次 `ui_history_parafmt()` 与 `ui_history_zoom_apply()`。

**行距**由 `ui_history_parafmt()` 用 `PARAFORMAT2` 的「精确」行距（rule 4）设定，值 = `dp(HIST_LPX_DEF + HIST_LINE_EXTRA)` 换算成 twips。两个要点：

- 用 rule 4 时，**行盒小于字形就会被削顶**，所以留了 `HIST_LINE_EXTRA = 1` 逻辑像素的余量（约 +5%）。设成正好等于字身格高就会出现用户反馈的"字的顶被削掉"。
- 换算**只用 `g_dpi` 这一个 DPI 来源**。曾经用 `GetDeviceCaps(hdc, LOGPIXELSY)` 去量，但那是错的：在 `WM_DPICHANGED` 里子控件的 DPI 上下文还没更新，那个 DC 报的仍是**旧屏**的 DPI，而 `dp()` 已经是新屏的 —— 两个口径混用会让行距整体偏一个 DPI 比例（实测 200%→250% 时行距从应有的 47px 变成 60px）。

---

## 许可

[MIT](LICENSE)
