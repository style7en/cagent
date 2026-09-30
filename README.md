# cagent

用 C 语言实现的极简 AI Agent,**零第三方依赖**,运行在 Windows 平台。Win32 GUI 版,核心 (`cagent_core.h`) 与界面 (`cagent_ui.c`) 分离,共用同一段 Agent 循环。

---

## 目录结构

```
cagent/
├── Makefile            # 构建脚本
├── cagent.ini          # 配置文件 (首次启动后自动生成)
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
│   ├── tools.h         # 结构化工具: execute_bash / read_file / write_file
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
| Url-Base: [ https://...v1     ]   |  ← 三项配置, 启动时自动从 cagent.ini 读取
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
| **Url-Base** | 填到 `/v1` 即可,程序自动追加 `/chat/completions`<br>例: `https://token.sensenova.cn/v1` |
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
  workspace=C:\path\to\workspace
  last_session=history_<工作目录>_<哈希>.json
  ```

### 操作

- **新建会话**:当前对话自动落盘保留,另起一个空对话(文件不删除,可随时加载回)
- **加载会话**:列出已保存的会话(目录 + 条数 + 时间 + 首句预览),选中后回放到界面并继续
- **回车**发送(等同点击"发送"按钮)
- **运行中取消**:Agent 调用期间"发送"按钮变为"停止",点击即请求取消;取消在下一个安全点(下一轮迭代前或工具执行前)生效,正在进行的 HTTP 请求无法立即打断
- **流式显示**:LLM 回复逐字流式显示(纯文本),按角色着色(你=蓝、AI=黑、工具/提示=灰);emoji 以单色字形显示
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
| 协作式取消 | Agent 运行时"发送"按钮变"停止",点击后在迭代间隙优雅退出并回滚本轮历史 |
| 网络错误诊断 | WinHTTP 错误码翻译为中文(DNS/连接/超时/SSL 证书),便于排错 |
| API Key 加密 | DPAPI (`CryptProtectData`) 加密存储 `cagent.ini` 中的 Key,明文不入盘 |
| 工作目录隔离 | 文件工具仅限工作目录内(路径强制解析),越界访问被拒绝 |
| 命令执行超时 | `run_pipe` 默认 60s,超时经作业对象终止整个进程树,避免长时间卡死(可用 `CAGENT_CMD_TIMEOUT` 以秒覆盖) |
| 对话历史持久化 | 每轮 done 后按工作目录保存 `history_*.json`;启动不自动加载,通过"加载会话"按钮按需载入并按角色回放到界面 |
| RichEdit 显示 | 历史框用 RICHEDIT50W,按角色分色;emoji 区段显式指定 Segoe UI Emoji 字体 |
| 流式 SSE | LLM 响应逐字流式显示(`stream:true`),工具调用 delta 累积 |

### 工具

目前内置(对齐 pi.dev 的极简理念,核心只保留 3 个工具):

- `execute_bash(command)` — 执行 shell 命令并捕获输出(不拦截,由用户自担风险);
  默认超时 60s(超时杀掉整个进程树),输出超限自动截断并附提示
- `read_file(path)` — 读取文件文本内容;非 UTF-8(如 GBK)文本会自动转成 UTF-8;
  超过上限时截断并告知文件总大小与续读方式
- `write_file(path, content)` — 写入文件(直接覆盖,无确认);父目录需已存在

> 没有专门的列目录/搜索工具:查看目录用 `dir`,递归搜索内容用 `findstr /s /i "关键词" *.*`,都走 `execute_bash`。

**结果上限与调参**(避免单次工具输出撑爆上下文):

| 环境变量 | 默认 | 说明 |
|---|---|---|
| `CAGENT_TOOL_OUTPUT_MAX` | 16384 (16KB) | 单次工具结果进入上下文的上限,超出即截断并附提示 |
| `CAGENT_CMD_TIMEOUT` | 60 (秒) | 命令执行超时,超时终止整个进程树 |

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
A: 缓冲区上限 `BUFSZ=256KB`(单次工具输出/单次 LLM 响应),按需调整。

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

## 许可

代码为学习用途,无许可证限制。
