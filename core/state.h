/*
 * core/state.h - 缓冲常量 / 全局状态 / 工具声明(TOOLS_JSON)
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

#define BUFSZ           (256 * 1024)
#define ARGS_MAX        8192   /* tool_calls.arguments 累积上限 (流式与执行期统一) */

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
    "\"description\":\"Execute a shell command via cmd /c\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"read_file\","
    "\"description\":\"Read text content of a file\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"write_file\","
    "\"description\":\"Write content to a file (overwrite)\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"content\"]}}}]";

