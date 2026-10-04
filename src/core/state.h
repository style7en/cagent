/*
 * core/state.h - 缓冲常量 / 全局状态 / 工具声明(TOOLS_JSON)
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* 上下文缓冲 1MB: 装得下约 30 万+ 汉字 / 更多的英文 token。
 * 静态缓冲 4 块 (messages/body/resp/tool_out) 各 1MB; 请求期还堆分配 StreamCtx
 * (~1.5MB: content_buf 1MB + 8 个 arguments 累积) 与工具调用表 (~0.5MB), 峰值约 6.5MB。 */
#define BUFSZ           (1024 * 1024)
#define ARGS_MAX        (64 * 1024)  /* tool_calls.arguments 累积上限 (流式与执行期统一)。
                                      * 旧值 8192 会静默截断大参数产出非法 JSON;
                                      * 超上限现在置 overflow 标志拒绝执行, 不再截断。
                                      * 64KB ≈ 2 万汉字, 覆盖绝大多数调用; 相比 256KB
                                      * 单轮并行 8 个调用省下约 1.5MB 堆。 */
#define ARGS_DISPLAY_MAX 8192        /* 界面 [Tool] 行 arguments 展示上限 (执行与回传不受影响) */
#define TOOL_OUTPUT_CAP (16 * 1024)  /* 单个工具结果进入上下文的上限, 超出即截断 */
#define EDIT_MAX_BYTES  (2 * 1024 * 1024) /* edit_file 可处理的最大文件体积 */
/* 请求体缓冲: 内容是 messages(<= BUFSZ) 再套一层 JSON 框架并拼上 model 与 TOOLS_JSON,
 * 所以必须**大于** BUFSZ —— 用 BUFSZ 装会触发 -Wformat-truncation, 而且真截断时
 * 发出去的是半截 JSON (服务端只会回 400 invariant), 属于"静默失败"里最糟的一种。
 * 8KB 余量远大于框架+工具表的实际长度, 使截断在数学上不可能发生。 */
#define BODY_SZ         (BUFSZ + 8192)

/* 路径缓冲尺寸约定 —— 消除 -Wformat-truncation 的根因:
 * MAX_PATH(260) 是 Windows **单个路径分量**的上限, 但代码里到处在做
 * "<目录> + <子目录/文件名>" 的二次拼接, 结果天然可能超过 260。用 260 的缓冲去接,
 * 一是 GCC -Wformat-truncation=2 会告警 (它按声明大小估算源串长度, 259+259 > 259),
 * 二是真超长时 snprintf 只静静截断, 留下一条"看起来合法"的半截路径, 后续
 * CreateFile/DeleteFile 会操作到错误的目录 —— 这比报错更危险。故约定:
 *   - 输入侧 (exe 目录、FindFirst 文件名等天然 <= MAX_PATH 的) 仍用 MAX_PATH;
 *   - 拼接结果一律用 PATHSZ: 259 + 259 + 1 装得下, snprintf 的输出长度有证可依。
 * 代价只是栈上多 260 字节 (函数内局部) / 全局表多几十 KB, 可忽略。 */
#define PATHSZ (MAX_PATH * 2)

/* ===== 路径拼接 (不用 snprintf("%s%s")) =====
 * 两种场景 snprintf 都不合适:
 *   - 目标是运行时 cap 的参数缓冲时, GCC 无法证明放得下, 只能告警;
 *   - 截断后留下的是半截路径, 看起来合法, 比显式失败更危险。
 * 这里先算长度再拷: 放不下就返回 0 并把 out 置空 —— 空路径转宽字符 / 打开文件
 * 都会失败, 调用方自然跳过该项, 不会误伤真实文件。 */
static int path_copy(char *out, size_t cap, const char *s) {
    size_t n = strlen(s);
    if (cap == 0) return 0;
    if (n + 1 > cap) { out[0] = '\0'; return 0; }
    memcpy(out, s, n + 1);
    return 1;
}

/* 在上一段之后继续追加 (out 需已有内容; 失败同样置空) */
static int path_append(char *out, size_t cap, const char *s) {
    size_t lo = strlen(out), ls = strlen(s);
    if (cap == 0) return 0;
    if (lo + ls + 1 > cap) { out[0] = '\0'; return 0; }
    memcpy(out + lo, s, ls + 1);
    return 1;
}

/* 定长拷贝并**允许**截断: 用于"目标本就比源短"的刻意截断 (如技能名索引只取前 N 字符)。
 * 总是 NUL 结尾, 不返回失败 —— 调用方要的就是截断后的前缀。 */
static void str_copy_into(char *out, size_t cap, const char *s) {
    size_t n = strlen(s);
    if (cap == 0) return;
    if (n + 1 > cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = '\0';
}

/* ===== 全局状态 ===== */
static char g_api_url[1024] = "";      /* 例: https://token.sensenova.cn/v1 */
static char g_api_key[512]  = "";
static char g_model[128]    = "";
static int  g_max_tokens    = 65536; /* 单次回复输出预算 (ini: max_tokens); 0 = 不下发该字段。
                                      * 默认按 agnes-2.5-flash 规格: 最大输出 64K token */
static int g_key_decrypt_failed = 0;   /* DPAPI 解密失败标志, 启动后提示 */
static int g_skip_cert_verify = 0;     /* 1=跳过 SSL 证书校验 (自签端点用) */
static char g_workspace[MAX_PATH] = "";/* 工作目录 (文件工具限制在此目录内, 默认 exe 目录) */
static char g_skills_dir[MAX_PATH] = ""; /* 技能根目录 (ini 的 skills_dir; 空 = 默认 <exe>\skills) */
static char g_active_ws[MAX_PATH] = "";/* 当前已加载历史所对应的工作目录 */
static char g_last_session[MAX_PATH] = ""; /* 最近使用的会话文件 (持久化到 ini) */
static char g_history_file[MAX_PATH] = ""; /* 当前对话绑定的会话文件 (懒生成) */

/* 本轮改动过的文件路径 (work线程用, 回滚时列给用户看: 对话能回滚, 文件不能) */
static char g_touched_files[1024] = "";

/* Agent 工作缓冲(只在工作线程使用,主线程不碰) */
static char messages[BUFSZ];
static char body[BODY_SZ];      /* 请求体: 见 BODY_SZ, 比其它三块多 8KB 的 JSON 框架余量 */
static char resp[BUFSZ];
static char tool_out[BUFSZ];

static volatile LONG g_running = 0;   /* 1 = Agent 工作线程运行中 */
static volatile LONG g_cancel  = 0;   /* 1 = 请求取消 */

/* ===== 上下文水位 (token 优先, 字节兜底, 见 agent.h 的 context_pressure) ===== */
static long g_context_tokens = 0;      /* 模型上下文窗口 (token), 来自 ini 的 context_tokens; 0=未知 */
static long g_last_prompt_tokens = 0;  /* 上次请求服务端报告的 prompt_tokens (SSE usage; 无则 0) */
static size_t g_last_prompt_bytes = 0; /* 上次请求发出时 messages 的字节数 (与上者配对做增量估算) */

static const char *TOOLS_JSON =
    "[{\"type\":\"function\",\"function\":{"
    "\"name\":\"execute_bash\","
    "\"description\":\"Run a shell command via 'cmd /c' on Windows, in the current workspace directory. "
        "stdout and stderr are captured together and returned as text. Default timeout 60s "
        "(env CAGENT_CMD_TIMEOUT overrides, in seconds), after which the whole process tree is killed. "
        "Output longer than about 16KB is truncated, so prefer precise commands "
        "(dir /b, findstr, type with a range) over dumping large files.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"command\":{\"type\":\"string\","
        "\"description\":\"The full command line, e.g. 'dir /b' or 'gcc -o hello.exe hello.c'\"}},"
    "\"required\":[\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"read_file\","
    "\"description\":\"Read a text file as UTF-8. Path must stay inside the workspace directory, "
        "otherwise the call is rejected. At most about 16KB is returned; when the file is larger, "
        "the result ends with a truncation note giving the total size and the next offset - "
        "pass that offset to keep reading chunk by chunk, or use execute_bash (findstr) to locate "
        "content directly.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"File path, absolute or relative to the workspace directory\"},"
    "\"offset\":{\"type\":\"integer\","
        "\"description\":\"Optional byte offset to start reading from (for chunked reading of "
        "large files); omit or 0 to read from the beginning\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"write_file\","
    "\"description\":\"Create a file or overwrite it completely with UTF-8 content. Path must stay "
        "inside the workspace directory, otherwise the call is rejected. The parent directory must "
        "already exist (create it with execute_bash first if needed). Returns the bytes written.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"File path, absolute or relative to the workspace directory\"},"
        "\"content\":{\"type\":\"string\",\"description\":\"Full new file content (UTF-8)\"}},"
    "\"required\":[\"path\",\"content\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"edit_file\","
    "\"description\":\"Targeted edit: replace old_text with new_text in an existing UTF-8 file. "
        "Preferred way to change part of a file - no need to rewrite the whole content. old_text must "
        "match exactly once, including whitespace and line breaks; otherwise the file is left "
        "untouched and the reason is returned, so read the file first when unsure. Path must stay "
        "inside the workspace directory.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"File path, absolute or relative to the workspace directory\"},"
        "\"old_text\":{\"type\":\"string\","
        "\"description\":\"Exact existing snippet to replace (must be unique in the file)\"},"
        "\"new_text\":{\"type\":\"string\",\"description\":\"Replacement text; empty string deletes it\"}},"
    "\"required\":[\"path\",\"old_text\",\"new_text\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"load_skill\","
    "\"description\":\"Load the full instructions of a user-provided skill by name. "
        "Available skills are listed in the system prompt. When the current task matches "
        "one of them, call this tool first and then strictly follow the returned "
        "instructions. Loading a skill returns text only - it does not execute anything.\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"name\":{\"type\":\"string\","
        "\"description\":\"Skill name, exactly as listed in the system prompt\"}},"
    "\"required\":[\"name\"]}}}]";

