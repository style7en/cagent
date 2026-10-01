# cagent

用 C 语言实现的极简 AI Agent,**零第三方依赖**,运行在 Windows 平台。Win32 GUI 版,核心 (`cagent_core.h`) 与界面 (`cagent_ui.c`) 分离,共用同一段 Agent 循环。

---

## 目录结构

```
cagent/
├── Makefile            # 构建脚本
├── LICENSE             # MIT 许可证
├── cagent.ini          # 配置文件 (首次启动后自动生成)
├── SYSTEM_PROMPT       # 可选: 外置系统提示词 (存在则替代内置默认)
├── sessions/           # 历史会话 (运行时生成, 首次保存时自动创建; 旧版散落的 history_*.json 启动时自动搬入)
├── cagent_ui.c         # 界面聚合入口 (仅按依赖顺序 include ui/)
├── cagent_core.h       # 核心聚合入口 (仅按依赖顺序 include core/)
├── core/               # 平台无关核心, 按子功能拆分 (单 TU, 全 static)
│   ├── base.h          # 平台头 / 角色常量 / 宿主钩子 / 输出封装
│   ├── state.h         # 缓冲常量 / 全局状态 / 工具声明
│   ├── crypto.h        # DPAPI: API Key 加密存储
│   ├── json.h          # 轻量 JSON 解析器 / 转义 / 自测
│   ├── exec.h          # 命令执行: CreateProcess + 管道 + 超时
│   ├── net.h           # WinHTTP 与 SSE 流式解析
│   ├── encoding.h      # 编码转换: UTF-8/OEM 检测与转换、字符边界
│   ├── workspace.h     # 工作目录解析、越界限制、UTF-8 文件/路径
│   ├── tools.h         # 结构化工具: execute_bash / read_file / write_file / edit_file
│   ├── agent.h         # Agent 循环: LLM <-> 工具调用、取消与回滚
│   ├── session.h       # 会话持久化: 存取 / 回放 / 多会话命名
│   └── config.h        # ini 配置读写
├── ui/                 # Win32 界面, 按子功能拆分
│   ├── state.h         # 控件 ID / 消息 / 全局句柄与字体
│   ├── hooks.h         # 宿主钩子实现
│   ├── helpers.h       # Edit 读写(UTF-8) + RichEdit 分角色追加
│   ├── task.h          # 启动一轮 Agent
│   ├── wndproc.h       # 布局 / 子类 / 主窗口过程
│   ├── dpi.h           # 高 DPI 适配
│   ├── session_dlg.h   # 会话选择对话框 (枚举 / 自绘列表 / 载入)
│   └── main.h          # 程序入口
├── app.rc / app.ico    # 图标资源
└── cagent.exe          # 编译产物
```

> 拆分方式: `core/` 与 `ui/` 下的头文件全部是 `static` 实现, 由聚合头按依赖顺序
> include, 仍是**单编译单元**, 无额外链接步骤, 也不引入任何第三方依赖。

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

项目带一套**离线自动化回归测试**(`core/test.h` 的 `run_all_tests`),覆盖 JSON 解析/转义、
编码检测与字符边界、工具分发与参数校验、`edit_file` 定点编辑的各类边界、HTTP 重试判定、
上下文压缩(消息遍历 / 保底丢弃的配对完整性 / 超限 400 识别 / token 与字节水位)、
`read_file` 的 offset 分块续读、外置系统提示词的转义包装与无效回退。无需网络或 GUI,改动核心代码后跑一遍即可当作护栏。

```bash
make test          # 编译 cagent_test.exe 并运行, 退出码 = 失败数 (0 即通过)
```

也可对 GUI 二进制直接自测(结果写到 `selftest.txt`):

```bash
./cagent.exe --selftest
```

> 测试通过 `test_main.c` 直接复用 `cagent_core.h` 的全部 `static` 实现编译成控制台程序,
> 因此测的就是真实代码路径,而非另写一份桩。

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
  last_session=sessions\history_<工作目录>_<哈希>.json
  ```

  - `context_tokens`:模型上下文窗口(token 数),如 131072 (128K)、1000000 (1M)。
    配置后按服务端报告的 `prompt_tokens` 精确控制水位(到 80% 触发压缩);
    `0` 或不写 = 未知,退回按字节水位。

### 外置系统提示词 `SYSTEM_PROMPT`

- **位置**:与 `cagent.exe` 同目录,文件名 `SYSTEM_PROMPT`(无扩展名)
- **内容**:纯文本,作为 system 提示词的正文;引号/换行由程序自动转义成 JSON
- **编码**:UTF-8 优先;非 UTF-8(如中文 Windows 的 GBK/ANSI 另存)自动按系统代码页转换
- **行为**:启动时读一次;文件不存在 / 为空 / 超过 32KB 时静默回退内置默认提示词

### 操作

- **新建会话**:当前对话自动落盘保留,另起一个空对话(文件不删除,可随时加载回)
- **加载会话**:列出已保存的会话(目录 + 条数 + 时间 + 首句预览),选中后回放到界面并继续
- **回车**发送(等同点击"发送"按钮)
- **运行中取消**:Agent 调用期间"发送"按钮变为"停止",点击即请求取消;取消在下一个安全点(下一轮迭代前或工具执行前)生效,正在进行的 HTTP 请求无法立即打断
- **流式显示**:LLM 回复逐字流式显示(纯文本),按角色着色(你=蓝加粗、AI=黑、思考/工具提示=灰);emoji 以单色字形显示
- 模型调用期间输入框自动禁用,完成后自动恢复
- 长内容自动换行,自动滚动到底部

### 技术特性

| 特性 | 实现 |
|---|---|
| HTTPS 网络 | **WinHTTP** (Windows 内置,无外部依赖) |
| UI 不卡死 | LLM 调用走后台线程,UI 主线程 `PostMessage` 接收追加事件 |
| 高 DPI 适配 | 三级降级: `SetProcessDpiAwarenessContext` → `SetProcessDpiAwareness` → `SetProcessDPIAware` |
| 中文显示 | 全程 UTF-8 ↔ UTF-16 转换,字体 Microsoft YaHei UI |
| 静默工具执行 | `CreateProcess + CREATE_NO_WINDOW + 匿名管道`,不弹 cmd 黑框 |
| 纯内存 IO | 无 `req.json`/`resp.json` 临时文件 |
| 配置持久化 | 启动加载/退出保存 `cagent.ini` |
| 协作式取消 | Agent 运行时"发送"按钮变"停止",点击后在迭代间隙优雅退出并回滚本轮**对话**;已执行的文件改动不会撤销,回滚提示会列出改过哪些文件 |
| 网络错误诊断 | WinHTTP 错误码翻译为中文(DNS/连接/超时/SSL 证书),便于排错 |
| 网络重试/退避 | 网络抖动(连接重置/超时/限流/5xx)自动指数退避重试,避免整轮对话回滚;取消与 4xx 客户端错误不重试;重试前**回删已上屏的半截回复**,界面不会出现两段重复内容 |
| API Key 加密 | DPAPI (`CryptProtectData`) 加密存储 `cagent.ini` 中的 Key,明文不入盘 |
| 工作目录隔离 | 文件工具仅限工作目录内(路径强制解析),越界访问被拒绝;**不解析符号链接/junction**,工作目录内指向外部的链接可绕过该限制,需要真正隔离请用容器/虚拟机;启动前会校验工作目录确实存在 |
| 命令执行超时 | `run_pipe` 默认 60s,超时经作业对象终止整个进程树,避免长时间卡死(可用 `CAGENT_CMD_TIMEOUT` 以秒覆盖) |
| 对话历史持久化 | 每轮 done 后按工作目录存到 `sessions\history_*.json`(不污染根目录);上下文压缩前自动归档 `*.pre_compact` 保留完整原始记录;启动不自动加载,通过"加载会话"按钮按需载入并按角色回放到界面 |
| RichEdit 显示 | 历史框用 RICHEDIT50W,按角色分色;emoji 区段显式指定 Segoe UI Emoji 字体 |
| 流式 SSE | LLM 响应逐字流式显示(`stream:true`),工具调用 delta 累积 |

### 工具

目前内置(对齐 pi.dev 的极简理念,核心只保留 4 个工具):

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
| `CAGENT_HTTP_RETRY_MS` | 800 (毫秒) | 首次重试的退避基数,后续按 2 倍指数增长(800 → 1600 → 3200 …) |

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

`cagent_core.h` 中的 `agent_turn` / `agent_thread` 就是这个循环的实现,GUI 通过宿主钩子 (`cagent_emit` 等) 与之交互。

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

版本号的**唯一事实来源**在 `core/version.h`（`CAGENT_VER_MAJOR/MINOR/PATCH` 与 `CAGENT_VERSION_STR`），改动版本只改这一处，三处同步引用：

- **Windows 文件属性**：`app.rc` 的 `VERSIONINFO` 资源，右键 `cagent.exe` → 属性 → 详细信息可见（当前数值 `1.1.1.0` + 字符串 `1.1.1`）。注意资源名必须用整数 `1`，写 `VS_VERSION_INFO` 会被 windres 当成字符串名导致读取失败（错误 1813）。
- **命令行**：`cagent.exe --version` 打印 `cagent 1.1.1`（有父控制台则打印到终端，否则弹对话框）。
- **窗口标题栏**：显示 `cagent 1.1.1`。

---

## 许可

[MIT](LICENSE)
