# cagent

用 C 语言实现的极简 AI Agent,**单文件 + 零第三方依赖**,运行在 Windows 平台。Win32 GUI 版,核心是同一段 Agent 循环。

---

## 目录结构

```
cagent/
├── Makefile            # 构建脚本
├── cagent.ini          # 配置文件 (首次启动后自动生成)
├── cagent_gui.c        # Win32 GUI 版 (~1300 行)
└── cagent_gui.exe      # 编译产物
```

---

## 编译

### 环境要求

- Windows 7 及以上 (推荐 Win10 1809+,DPI 适配更佳)
- MinGW-w64 (GCC 13+) 或同等 C 编译器
- 标准 `make`

### 一键编译

```bash
make            # 构建 cagent_gui
make clean      # 删除可执行文件
```

或单独构建:

```bash
make cagent_gui.exe
```

---

## GUI 版: `cagent_gui.exe`

聊天式对话窗口,**支持运行时配置、配置持久化、HTTPS 直连、多轮工具调用可视化**。

### 启动

直接双击 `cagent_gui.exe`,或:

```bash
./cagent_gui.exe
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

- **位置**:与 `cagent_gui.exe` 同目录
- **编码**:UTF-8
- **行为**:
  - 程序启动时自动读取(若文件不存在则跳过)
  - 程序关闭时自动把当前三个输入框的值写回
- **格式**:
  ```ini
  # cagent GUI 配置 (UTF-8, 退出时自动保存)
  url_base=https://token.sensenova.cn/v1
  api_key=dpapi:<DPAPI 加密的 base64>
  model=deepseek-v4-flash
  skip_cert_verify=0
  ```

### 操作

- **回车**发送(等同点击"发送"按钮)
- **运行中取消**:Agent 调用期间"发送"按钮变为"停止",点击即请求取消;取消在下一个安全点(下一轮迭代前或工具执行前)生效,正在进行的 HTTP 请求无法立即打断
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
| 命令沙箱 | 危险命令(rm/del/format 等)执行前弹窗确认,拒绝则把结果交回模型 |

### 工具

目前只内置:

- `execute_bash(command)` — 通过 `cmd /c` 执行命令并捕获输出

---

## 工作原理(Agent 循环)

```
┌─────────────────────────────────────────┐
│  messages = [system, user]              │
│  loop (最多 MAX_ITERATIONS 轮):         │
│    resp = HTTP POST chat/completions    │
│    if resp 含 "tool_calls":             │
│      执行每个工具                       │
│      把 assistant + tool 消息追加进     │
│      messages, 继续下一轮               │
│    else:                                │
│      打印/显示 resp.content, 结束       │
└─────────────────────────────────────────┘
```

`cagent_gui.c` 的 `agent_thread` 就是这个循环的实现。

---

## 常见问题

**Q: GUI 启动后输入框是空的?**
A: 首次启动 `cagent.ini` 不存在,请在三个 Edit 中填入配置;关闭窗口后会自动保存到 ini。

**Q: 提示 `[HTTP 401]`?**
A: API Key 错误或未授权,检查 Key 字段。

**Q: 工具调用反复执行不停止?**
A: 达到 `MAX_ITERATIONS=20` 后会显示 "(max iterations reached)",这是模型未能终止的兜底。可在源码中调大。

**Q: 长输出被截断?**
A: 缓冲区上限 `BUFSZ=256KB`(单次工具输出/单次 LLM 响应),按需调整。

**Q: GUI 不响应,看起来卡了?**
A: LLM 请求通常 1-5 秒,期间界面显示 `(thinking...)` 但仍可拖动窗口。如真卡死请反馈。

**Q: 点了"停止"但还在转?**
A: 取消是协作式的:已发出的 HTTP 请求无法中途打断,会在下一个迭代间隙生效。若长时间无响应(如服务端不返回),等待超时后才会退出。

**Q: 换了电脑/用户后 API Key 解密失败?**
A: Key 用 DPAPI 加密,绑定当前 Windows 用户。换机/换用户无法解密,程序会提示并清空 Key,重新填写即可。

**Q: 模型要删文件时弹了确认框?**
A: 命令沙箱拦截了危险命令(rm/del/format 等)。选"否"会把"(用户拒绝执行)"返回模型,模型可改用其他方案。

---

## 许可

代码为学习用途,无许可证限制。
