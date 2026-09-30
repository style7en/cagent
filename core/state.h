/*
 * core/state.h - 缓冲常量 / 全局状态 / 工具声明(TOOLS_JSON)
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

#define BUFSZ           (256 * 1024)
#define ARGS_MAX        8192   /* tool_calls.arguments 累积上限 (流式与执行期统一) */
#define TOOL_OUTPUT_CAP (16 * 1024)  /* 单个工具结果进入上下文的上限, 超出即截断 */

/* ===== 全局状态 ===== */
static char g_api_url[1024] = "";      /* 例: https://token.sensenova.cn/v1 */
static char g_api_key[512]  = "";
static char g_model[128]    = "";
static int g_key_decrypt_failed = 0;   /* DPAPI 解密失败标志, 启动后提示 */
static int g_skip_cert_verify = 0;     /* 1=跳过 SSL 证书校验 (自签端点用) */
static char g_workspace[MAX_PATH] = "";/* 工作目录 (文件工具限制在此目录内, 默认 exe 目录) */
static char g_active_ws[MAX_PATH] = "";/* 当前已加载历史所对应的工作目录 */
static char g_last_session[MAX_PATH] = ""; /* 最近使用的会话文件 (持久化到 ini) */
static char g_history_file[MAX_PATH] = ""; /* 当前对话绑定的会话文件 (懒生成) */

/* Agent 工作缓冲(只在工作线程使用,主线程不碰) */
static char messages[BUFSZ];
static char body[BUFSZ];
static char resp[BUFSZ];
static char tool_out[BUFSZ];

static volatile LONG g_running = 0;   /* 1 = Agent 工作线程运行中 */
static volatile LONG g_cancel  = 0;   /* 1 = 请求取消 */

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
        "the result ends with a truncation note giving the total size, and the rest can be read "
        "via execute_bash (findstr or a chunked command).\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\","
        "\"description\":\"File path, absolute or relative to the workspace directory\"}},"
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
    "\"required\":[\"path\",\"content\"]}}}]";

