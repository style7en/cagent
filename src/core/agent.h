/*
 * core/agent.h - Agent 循环: LLM ↔ 工具调用、取消与回滚
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== Agent 循环 ===== */

typedef struct { char user_msg[BUFSZ]; } AgentTask;

/* 本轮要执行的一个工具调用 (从 StreamCtx 拷贝而来)。
 * 每项含 256KB 参数缓冲, 8 项约 2MB —— 必须放堆, 不能放栈 (工作线程默认栈 1MB)。 */
typedef struct {
    char id[256]; char name[64]; char args[ARGS_MAX]; char *output;
    int too_big;   /* arguments 超上限: 不执行, 回填占位 "{}" + 原因交回模型 */
} AgentToolCall;

/* 内置默认系统提示词: 外置 SYSTEM_PROMPT 文件缺失或无效时的兜底。 */
#define SYSTEM_PROMPT_DEFAULT \
    "{\"role\":\"system\",\"content\":\"你是 cagent,一个极简的编程 Agent。" \
    "你有四个工具: execute_bash(执行命令)、read_file(读文件)、write_file(写文件)、edit_file(定点替换编辑)。" \
    "改已有文件的局部内容,优先用 edit_file: old_text 必须与文件中的原文完全一致(含空白与换行)且在文件中唯一, 只出现一次才会替换。" \
    "edit_file 报告未找到匹配或匹配到多处时, 先 read_file 看清原文, 再调整 old_text 重试。" \
    "新建文件用 write_file; 需要整体重写时先用 read_file 读取现有内容再整体写入。" \
    "列目录用 execute_bash 跑 dir,递归搜索内容用 findstr /s /i 关键词 *.* 。" \
    "命令通过 cmd /c 执行,用 Windows 命令风格:不要 mkdir -p(直接 mkdir),运行当前程序不要 ./ 前缀。" \
    "文件工具仅限工作目录内,用相对路径。" \
    "高危命令必须先征得用户同意:调用 execute_bash 前,先用一句话说明要做什么和风险,然后停下来等用户明确确认;确认之前不要执行。" \
    "高危包括:删除或覆盖文件(del、rd、rmdir /s、format、diskpart)、改注册表或系统服务(reg、sc、net user、schtasks)、关机重启、批量移动或重命名文件、下载后直接执行外部脚本、改写 git 历史(git push -f、reset --hard)、以及任何写入工作目录之外位置的操作。" \
    "用户同意后执行一次即可,同类操作不必反复询问;用户拒绝则放弃该做法并换一个更安全的方案。" \
    "任务不明确时,先向用户澄清。" \
    "任务完成后,停止并简要总结你做了什么。" \
    "始终用中文回答。回答简洁。\"}"

/* 实际生效的 system 消息 JSON。默认指向内置默认串; 启动时 exe 同目录存在
 * SYSTEM_PROMPT 文件的话, system_prompt_init() 替换为外置文件包装后的堆串
 * (进程生命周期内最多替换一次, 不释放)。所有 messages 偏移都按它计算。 */
static const char *g_system_prompt = SYSTEM_PROMPT_DEFAULT;

/* messages 缓冲水位线: 接近上限时触发压缩, 防止越界. */
#define MESSAGES_WATERMARK  ((BUFSZ * 3) / 4)

/* ===== 上下文压缩参数 ===== */
/* token 估算: 中文约 3 字节/token, 英文约 4; 取 3 偏保守 (高估 token → 提早压缩, 安全)。 */
#define BYTES_PER_TOKEN_EST        3
/* 摘要硬上限 (字节); 提示词要求模型控制在 2000 字以内, 此处兜底截断。 */
#define COMPACT_SUMMARY_MAX        8192
/* 压缩时尾部原样保留的预算 (字节): 摘要 + 最近原文, 避免刚读过的内容全部丢失。 */
#define COMPACT_TAIL_KEEP_BYTES    (32 * 1024)
/* 400 上下文超限后"压缩并重试"的最大次数, 防止压缩不奏效时无限打转。 */
#define CONTEXT_OVERFLOW_RETRIES   3

static const char *ROLLBACK_HINT = "\r\n(本轮对话已回滚, 不影响后续对话; 已执行的文件改动不会自动撤销)\r\n";

/* 初始化 messages 为只含 system prompt 的状态。 */
static void reset_conversation(void) {
    snprintf(messages, BUFSZ, "%s", g_system_prompt);
}

/* ===== 外置系统提示词 (exe 同目录的 SYSTEM_PROMPT 文件) ===== */

/* 外置提示词原文上限 (字节); 超限视为无效, 静默回退默认 */
#define SYSTEM_PROMPT_MAX_RAW (32 * 1024)

static void get_app_path(char *out, size_t cap, const char *filename);   /* 定义在 session.h */

/* 把提示词原文包装成 system 消息 JSON 并提交 (转义由 json_escape 负责,
 * 引号/换行/CRLF 都安全)。空原文或超限返回 0, g_system_prompt 保持原值。 */
static int system_prompt_set_raw(const char *raw) {
    if (!raw || !raw[0] || strlen(raw) > SYSTEM_PROMPT_MAX_RAW) return 0;
    char *esc = json_escape_alloc(raw);
    if (!esc) return 0;
    size_t need = strlen(esc) + 64;   /* 转义串 + 包装前后缀 + '\0' */
    char *buf = (char*)malloc(need);
    if (!buf) { free(esc); return 0; }
    int n = snprintf(buf, need, "{\"role\":\"system\",\"content\":\"%s\"}", esc);
    free(esc);
    if (n <= 0 || (size_t)n >= need) { free(buf); return 0; }
    g_system_prompt = buf;
    return 1;
}

/* 启动时读取 exe 同目录的 SYSTEM_PROMPT 外置提示词。
 * 文件不存在 / 空 / 超限 / 转码后为空 -> 静默保持内置默认。 */
static void system_prompt_init(void) CAGENT_MAYBE_UNUSED;   /* 测试二进制不调用 (保持默认指针) */
static void system_prompt_init(void) {
    char path[MAX_PATH];
    get_app_path(path, sizeof(path), "SYSTEM_PROMPT");
    FILE *f = fopen_utf8(path, "rb");
    if (!f) { log_line("[prompt] 未找到外置提示词, 使用内置默认: %s", path); return; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); log_line("[prompt] 外置提示词读取失败: %s", path); return; }
    long fsz = ftell(f);
    if (fsz <= 0 || fsz > SYSTEM_PROMPT_MAX_RAW) {
        fclose(f);
        log_line("[prompt] 外置提示词无效 (%ld bytes, 上限 %d), 回退默认: %s",
             fsz, SYSTEM_PROMPT_MAX_RAW, path);
        return;
    }
    rewind(f);
    /* 2x 容量: GBK -> UTF-8 最坏膨胀 1.5 倍, oem_to_utf8 需要余量 */
    char *buf = (char*)malloc((size_t)fsz * 2 + 1);
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    if (n == 0) { free(buf); return; }
    buf[n] = '\0';
    if (!is_valid_utf8((const unsigned char*)buf, n)) {   /* 非法 UTF-8: 按 GBK/ANSI 转换 */
        oem_to_utf8(buf, (size_t)fsz * 2 + 1);
        n = strlen(buf);
    }
    if (n == 0 || !is_valid_utf8((const unsigned char*)buf, n)) {
        free(buf);
        log_line("[prompt] 外置提示词转码后为空/仍非法 UTF-8, 回退默认: %s", path);
        return;
    }
    if (system_prompt_set_raw(buf))
        log_line("[prompt] 使用外置提示词 %s (%zu bytes)", path, n);
    else
        log_line("[prompt] 外置提示词包装失败, 回退默认: %s", path);
    free(buf);
}

/* 前向声明: agent_turn 调用历史保存, 其定义在 session.h (聚合顺序在本文件之后) */
static void history_save(void);

/* 拼接 chat/completions 完整 URL (agent_turn 与上下文压缩共用) */
static void chat_completions_url(char *out, size_t cap) {
    size_t nurl = strlen(g_api_url);
    snprintf(out, cap, "%s%schat/completions", g_api_url,
             (nurl > 0 && g_api_url[nurl - 1] == '/') ? "" : "/");
}

/* ===== messages 遍历辅助 (压缩按"整条消息"操作, 避免拆散 assistant/tool 配对) =====
 * messages 形如 g_system_prompt + ",{...},{...}"; conv = messages + strlen(g_system_prompt)。 */

/* 前向声明: 定义在 session.h (聚合顺序在本文件之后), 同 exec.h 前向声明 utf8_trim_len */
static size_t first_object_end(const char *s);

/* conv 中的顶层消息对象个数 */
static size_t msg_count(const char *conv) {
    size_t n = 0, pos = 0;
    for (;;) {
        size_t e = first_object_end(conv + pos);
        if (!e) break;
        n++;
        pos += e;
    }
    return n;
}

/* 第 idx 个消息对象的边界 [start,end) (相对 conv; start 指向 '{', end 在 '}' 之后)。
 * 找不到返回 0。 */
static int msg_bounds(const char *conv, size_t idx, size_t *start, size_t *end) {
    size_t n = 0, pos = 0;
    for (;;) {
        size_t e = first_object_end(conv + pos);
        if (!e) return 0;
        if (n == idx) {
            size_t b = pos;
            while (conv[b] && conv[b] != '{') b++;   /* 跳过前导逗号, 指向对象真正的 '{' */
            *start = b;
            *end = pos + e;
            return 1;
        }
        n++;
        pos += e;
    }
}

/* 消息对象内的 role 值。本程序序列化时 role 恒在最前 (无空白), 在对象前部查找即可;
 * 识别不了返回空串 (调用方视为非 tool, 压缩时保守处理)。 */
static void msg_role(const char *obj, size_t len, char *out, size_t cap) {
    static const char key[] = "\"role\":\"";
    const size_t keylen = sizeof(key) - 1;
    out[0] = '\0';
    if (len < keylen) return;
    size_t scan = (len < 96) ? len - keylen : 96;   /* role 总在对象开头不远处 */
    for (size_t i = 0; i <= scan; i++) {
        if (memcmp(obj + i, key, keylen) == 0) {
            const char *r = obj + i + keylen;
            size_t j = 0;
            while (r[j] && r[j] != '"' && j + 1 < cap && (size_t)(r - obj) + j < len) {
                out[j] = r[j];
                j++;
            }
            out[j] = '\0';
            return;
        }
    }
}

/* 向 messages 尾部追加一段格式化 JSON。返回 1 成功; 容量不足返回 0
 * (vsnprintf 已做截断写入, 调用方按保存点回滚)。 */
static int msg_append(const char *fmt, ...) {
    size_t len = strlen(messages);
    if (len >= BUFSZ) return 0;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(messages + len, BUFSZ - len, fmt, ap);
    va_end(ap);
    return (n > 0 && (size_t)n < BUFSZ - len);
}

/* ===== 超限识别 ===== */

/* 大小写不敏感子串查找 (strcasestr 非标准, 自带一份); 找到返回命中位置, 否则 NULL */
static const char *contains_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return hay;
    }
    return NULL;
}

/* 服务端 400 是否为"上下文超限": 各家措辞不一, 宽松匹配常见写法 */
static int is_context_overflow_error(const char *err) {
    if (!err) return 0;
    return contains_ci(err, "context length") != NULL
        || contains_ci(err, "context_length") != NULL
        || contains_ci(err, "maximum context") != NULL
        || contains_ci(err, "too many tokens") != NULL
        || contains_ci(err, "prompt is too long") != NULL
        || contains_ci(err, "input length exceeds") != NULL
        || contains_ci(err, "reduce the length") != NULL;
}

/* ===== 上下文水位 (token 优先, 字节兜底) ===== */

/* 估算当前 messages 折合的 prompt token 数:
 * 有服务端读数 (SSE usage) 时按"上次读数 + 新增字节折算", 否则整体按估算折算。 */
static long estimate_prompt_tokens(void) {
    size_t now = strlen(messages);
    if (g_last_prompt_tokens > 0) {
        long grown = (long)now - (long)g_last_prompt_bytes;
        if (grown < 0) grown = 0;
        return g_last_prompt_tokens + grown / BYTES_PER_TOKEN_EST;
    }
    return (long)now / BYTES_PER_TOKEN_EST;
}

/* 是否应当压缩: 配置了模型窗口 (ini 的 context_tokens) 则按 token 估算到 80% 触发;
 * 未配置或服务端不发 usage 时退回字节水位 (75% BUFSZ)。 */
static int context_pressure(void) {
    if (g_context_tokens > 0 &&
        estimate_prompt_tokens() > g_context_tokens * 4 / 5) return 1;
    return strlen(messages) > MESSAGES_WATERMARK;
}

/* 压缩目标: 压完至少降到该字节数以下 (配置了窗口按窗口折算, 否则水位的一半) */
static size_t compact_target_bytes(void) {
    if (g_context_tokens > 0) {
        size_t t = (size_t)g_context_tokens * BYTES_PER_TOKEN_EST / 2;
        if (t > MESSAGES_WATERMARK) t = MESSAGES_WATERMARK;
        if (t == 0) t = 1;
        return t;
    }
    return MESSAGES_WATERMARK / 2;
}

/* 压缩前把当前会话文件归档为 <原名>.pre_compact: 完整原始记录不再只依赖单代 .bak */
static void archive_session_before_compact(void) {
    if (!g_history_file[0]) return;
    wchar_t wsrc[MAX_PATH], wdst[MAX_PATH + 16];
    char dst[MAX_PATH + 32];
    if (!utf8_to_wide(g_history_file, wsrc, MAX_PATH)) return;
    if (GetFileAttributesW(wsrc) == INVALID_FILE_ATTRIBUTES) return;
    snprintf(dst, sizeof(dst), "%s.pre_compact", g_history_file);
    if (!utf8_to_wide(dst, wdst, MAX_PATH + 16)) return;
    CopyFileW(wsrc, wdst, FALSE);
}

/* ===== 压缩阶梯 =====
 * ① 摘要旧段, 最近 COMPACT_TAIL_KEEP_BYTES 原文保留 (刚读过的内容不丢, 请求也更小);
 * ② 请求仍失败: 只摘要更早的一半 (摘要请求再减半);
 * ③ 保底: 不经模型, 从最旧开始按整条消息丢弃 (无网络依赖, 不拆散 tool 配对)。
 * 全部失败返回 0, 由调用方收束: 轮首另起新会话, 轮中保留现场提前结束本轮。 */

/* 摘要压缩。halve=0: 摘要 [0,k) 保留 [k,n) 原文; halve=1: 只摘要 [0,k) 的前一半。
 * 成功返回 1 且已重写 messages; 失败返回 0 且 messages 保持原样。 */
static int compact_via_summary(int halve) {
    const char *conv = messages + strlen(g_system_prompt);
    size_t total = strlen(conv);
    size_t n = msg_count(conv);
    if (n == 0) return 0;

    /* 1. 尾部保留: 从最后一条往前累计, 预算 COMPACT_TAIL_KEEP_BYTES */
    size_t k = n, acc = 0;
    while (k > 0) {
        size_t s, e;
        if (!msg_bounds(conv, k - 1, &s, &e)) break;
        if (acc + (e - s) > COMPACT_TAIL_KEEP_BYTES) break;
        acc += e - s;
        k--;
    }
    /* 2. halve: 保留段再向前扩到 [0,k) 的中点, 摘要请求更小 */
    if (halve && k > 1) {
        size_t mid = k / 2;
        if (mid >= 1 && mid < k) k = mid;
    }
    /* 3. 保留段起点不能是孤立的 tool 结果 (其 assistant 已进摘要), 否则请求必 400 */
    while (k < n) {
        size_t s, e;
        if (!msg_bounds(conv, k, &s, &e)) return 0;
        char role[16];
        msg_role(conv + s, e - s, role, sizeof(role));
        if (strcmp(role, "tool") != 0) break;
        k++;
    }
    if (k == 0) return 0;
    size_t ks = total;   /* 保留段第一条消息的起点 (k==n 时无保留段) */
    if (k < n) {
        size_t s, e;
        if (!msg_bounds(conv, k, &s, &e)) return 0;
        ks = s;
    }

    /* 4. 摘要请求: system + [0,ks) + ask (不带 tools, 避免模型改为调用工具) */
    static const char *ask =
        "上下文即将超出长度上限。请把以上对话压缩成一份交接摘要, 供后续继续工作。"
        "必须保留: 1) 用户的目标与约束; 2) 已完成的关键步骤与结论; 3) 创建或改动过的文件路径; "
        "4) 仍未完成的事项与下一步。不要寒暄、不要复述原文, 直接输出摘要, 控制在 2000 字以内。"
        "(最近的几条消息会原样保留, 不必复述。)";
    char *esc_ask = json_escape_alloc(ask);
    if (!esc_ask) return 0;
    size_t rcap = strlen(g_system_prompt) + ks + strlen(esc_ask) + 64;
    char *req_msgs = (char*)malloc(rcap);
    if (!req_msgs) { free(esc_ask); return 0; }
    int rn = snprintf(req_msgs, rcap, "%s%.*s%s{\"role\":\"user\",\"content\":\"%s\"}",
                      g_system_prompt, (int)ks, conv, (k < n) ? "" : ",", esc_ask);
    free(esc_ask);
    if (rn <= 0 || (size_t)rn >= rcap) { free(req_msgs); return 0; }

    size_t bcap = (size_t)rn + strlen(g_model) + 128;
    char *bodybuf = (char*)malloc(bcap);
    if (!bodybuf) { free(req_msgs); return 0; }
    snprintf(bodybuf, bcap, "{\"model\":\"%s\",\"messages\":[%s],\"stream\":true}",
             g_model, req_msgs);
    free(req_msgs);

    char url[1280];
    chat_completions_url(url, sizeof(url));
    StreamCtx *sctx = (StreamCtx*)calloc(1, sizeof(StreamCtx));
    if (!sctx) { free(bodybuf); return 0; }
    char err[512]; err[0] = '\0';
    if (cagent_stream_begin) cagent_stream_begin();
    int status = http_post_stream(url, g_api_key, bodybuf, strlen(bodybuf), err, sizeof(err), sctx);
    free(bodybuf);

    char *summary = NULL;
    if (status == 200 && sctx->content_len > 0) {
        size_t slen = sctx->content_len;
        if (slen > COMPACT_SUMMARY_MAX)
            slen = utf8_trim_len(sctx->content_buf, COMPACT_SUMMARY_MAX);   /* 摘要本身也设上限 */
        sctx->content_buf[slen] = '\0';
        /* 摘要自身被输出上限截断: 明确标注, 不静默接受半截摘要 */
        if (strcmp(sctx->finish, "length") == 0)
            strncat(sctx->content_buf, "\n...(摘要因输出长度上限被截断)",
                    sizeof(sctx->content_buf) - strlen(sctx->content_buf) - 1);
        summary = strdup(sctx->content_buf);
    }
    free(sctx);
    if (!summary) return 0;

    /* 5. 拼装新 messages: SYSTEM + 摘要 user 消息 + 保留段原文。
     *    先在堆上拼好并全部校验, 成功才提交; 失败时 messages 保持原样 (下一级阶梯接手)。 */
    char *esc_sum = json_escape_alloc(summary);
    free(summary);
    if (!esc_sum) return 0;
    size_t tail_len = total - ks;
    size_t ncap = strlen(g_system_prompt) + strlen(esc_sum) + tail_len + 256;
    char *newbuf = (char*)malloc(ncap);
    if (!newbuf) { free(esc_sum); return 0; }
    int wn = snprintf(newbuf, ncap,
        "%s,{\"role\":\"user\",\"content\":\"[上下文已压缩] 以下是此前对话的摘要, "
        "请据此继续, 不要重复已完成的工作:\\n\\n%s\"}%s%.*s",
        g_system_prompt, esc_sum, (k < n) ? "," : "", (int)tail_len, conv + ks);
    free(esc_sum);
    if (wn <= 0 || (size_t)wn >= ncap || wn >= BUFSZ) { free(newbuf); return 0; }
    memcpy(messages, newbuf, (size_t)wn + 1);
    free(newbuf);
    append_text("(上下文已压缩为摘要, 最近的消息保留原文, 继续对话)\r\n");
    return 1;
}

/* 保底压缩: 不经模型, 从最旧开始按整条消息丢弃, 直到低于 compact_target_bytes。
 * 成功返回 1; 没有可丢的 (全丢/本来就没超) 返回 0, 交由调用方收束。 */
static int compact_drop_oldest(void) {
    const char *conv = messages + strlen(g_system_prompt);
    size_t total = strlen(conv);
    size_t target = compact_target_bytes();
    if (total <= target) return 0;
    size_t n = msg_count(conv);
    if (n == 0) return 0;

    size_t cut = 0, dropped = 0;
    while (cut < n) {
        size_t s, e;
        if (!msg_bounds(conv, cut, &s, &e)) break;
        dropped += e - s;
        cut++;
        if (total - dropped <= target) break;
    }
    size_t dropped_n = cut;
    /* 保留起点避开孤立的 tool 结果 (其 assistant 已被丢弃) */
    while (cut < n) {
        size_t s, e;
        if (!msg_bounds(conv, cut, &s, &e)) return 0;
        char role[16];
        msg_role(conv + s, e - s, role, sizeof(role));
        if (strcmp(role, "tool") != 0) break;
        cut++;
    }
    if (cut >= n) return 0;   /* 全部都要丢: 让调用方走重置路径 */
    size_t ks, ke;
    if (!msg_bounds(conv, cut, &ks, &ke)) return 0;

    reset_conversation();
    if (!msg_append(",%.*s", (int)(total - ks), conv + ks)) {
        reset_conversation();   /* 理论不可达 (只丢不加必能装下), 兜底回到安全空态 */
        return 0;
    }
    char note[160];
    snprintf(note, sizeof(note),
             "(摘要压缩失败, 已按整条消息丢弃最早的 %d 条; 早期细节不再在上下文中)\r\n",
             (int)dropped_n);
    append_text(note);
    return 1;
}

/* 上下文压缩总入口: 先归档当前会话文件, 再按阶梯尝试。
 * 成功返回 1 (messages 已改写), 失败返回 0 (messages 保持原样)。 */
static int compact_conversation(void) {
    archive_session_before_compact();
    int ok = compact_via_summary(0)      /* ① 摘要旧段 + 尾部保留 */
          || compact_via_summary(1)      /* ② 只摘要更早一半, 请求更小 */
          || compact_drop_oldest();      /* ③ 保底: 不经模型直接丢最旧 */
    if (ok) {
        /* 压缩改写了历史, 上次请求的 token 读数已失效 */
        g_last_prompt_tokens = 0;
        g_last_prompt_bytes = 0;
    }
    return ok;
}

/* 执行一轮对话 (user_msg -> 直至最终回复或回滚)。线程无关, 可在主线程直接调用。
 * 用户消息的界面回显由前端负责 (核心不管渲染)。 */
static void agent_turn(const char *user_msg) {
    InterlockedExchange(&g_cancel, 0);
    g_touched_files[0] = '\0';                  /* 本轮改动记录重新计 (回滚提示要引用) */
    /* 用户输入按字符边界截取: %.120s 会切半汉字, 日志自己就成了非法 UTF-8 */
    char uprev[160];
    utf8_safe_copy(user_msg, 120, uprev, sizeof(uprev));
    log_line("[turn] 开始 ws=%s msgs_len=%zu user=%s",
         g_workspace[0] ? g_workspace : "-", strlen(messages), uprev);

    /* 上下文压力检查 (token 优先, 字节兜底): 先压缩, 再开始本轮 */
    if (context_pressure()) {
        append_text("(上下文接近上限, 正在压缩为摘要...)\r\n");
        int cok = compact_conversation();
        log_line("[compact] 轮首触发 -> %s, msgs_len=%zu", cok ? "成功" : "失败", strlen(messages));
        if (!cok) {
            append_text("(压缩失败, 已自动另起新会话)\r\n");
            reset_conversation();
            g_history_file[0] = '\0';
        }
    }
    if (messages[0] == '\0') reset_conversation();

    size_t savepoint = strlen(messages);   /* 出错回滚到这里, 丢弃本轮 user message */
    int rolled_back = 0;
    int iter = 0;              /* 迭代序号: 首轮的水位检查已在上面做过 */
    int overflow_retries = 0;  /* 400 上下文超限后的"压缩重试"次数 */
    int utf8_heal_tried = 0;   /* 历史 UTF-8 自愈只做一次, 防止修不动时原地打转 */
    /* 回滚/提前收束的原因。每一条离开本轮的非正常路径都要写上 —— 用户看到的只有
     * "(本轮对话已回滚, ...)" 一句话, 日志里若不带原因, 事后根本无从判断是哪一步。 */
    const char *reason = NULL;
    int ok;
    AgentToolCall *calls = NULL;   /* 本轮工具调用表 (堆分配); done 处兜底释放 */
    StreamCtx *ctx = NULL;         /* 本轮流式上下文 (堆分配); done 处兜底释放 */

    char *escaped = json_escape_alloc(user_msg);
    if (!escaped) {
        append_text("(内存不足, 已忽略本轮)\r\n");
        reason = "内存不足 (user 消息转义失败)";
        rolled_back = 1;
        goto done;
    }
    ok = msg_append(",{\"role\":\"user\",\"content\":\"%s\"}", escaped);
    free(escaped);
    if (!ok) {
        append_text("(输入过长, 已忽略本轮)\r\n");
        reason = "输入过长 (user 消息拼不进 messages)";
        rolled_back = 1;
        goto done;                            /* 截断交给 done 统一做, 这里不再重复 */
    }

    for (;;) {
        if (InterlockedCompareExchange(&g_cancel, 0, 0)) {
            append_text("(已取消)\r\n");
            reason = "用户取消 (轮次开始前)";
            rolled_back = 1;
            goto done;
        }
        /* 轮中水位检查: 长工具链任务单轮就能涨几十上百 KB, 迭代间隙消息序列完整,
         * 是安全的压缩点。压缩失败不回滚 —— 已完成的工具调用与结论都保留,
         * 本轮提前收束, 用户可继续下一条指令。 */
        if (iter > 0 && context_pressure()) {
            append_text("(上下文接近上限, 正在压缩为摘要...)\r\n");
            int cok = compact_conversation();
            log_line("[compact] 迭代间隙触发 -> %s, msgs_len=%zu", cok ? "成功" : "失败", strlen(messages));
            if (!cok) {
                append_text("(压缩失败, 本轮到此暂停; 已完成的步骤都保留在对话中)\r\n");
                reason = "上下文压缩失败 (保留已完成步骤, 不回滚)";
                goto done;
            }
            savepoint = strlen(messages);   /* 历史已改写, 回滚基点随之更新 */
        }
        iter++;

        snprintf(body, BUFSZ,
            "{\"model\":\"%s\",\"messages\":[%s],\"tools\":%s,\"stream\":true}",
            g_model, messages, TOOLS_JSON);
        if (log_check_utf8("请求体", body, strlen(body))) {
            /* 本地预检发现非法 UTF-8: 拦截不发送 —— 发出去也只会被服务端 400
             * invalid unicode 拒掉。坏字节偏移/消息序号在上面的 [utf8] 行里,
             * 完整请求体留档 log\request_fail_<时间>_<序号>.json 供复现。 */
            log_dump_request(body, strlen(body));
            /* 回滚救不了"历史已污染": 坏字节若来自已持久化的 messages (旧版 oem_to_utf8
             * 转码失败留下的 GBK 字节、或外部损坏的会话文件), 截回本轮 savepoint 之后
             * 它照样在, 下一轮又被拦 —— 会话从此永久卡死, 用户只能去删文件。
             * 所以先就地净化一次再重发; 净化不动 (坏字节不在 messages 里, 如 model 字段)
             * 才走回滚。 */
            if (!utf8_heal_tried) {
                utf8_heal_tried = 1;
                size_t healed = utf8_sanitize_inplace(messages);
                if (healed) {
                    log_line("[utf8] 历史含非法 UTF-8, 已就地净化 %zu 字节后重发 (msgs_len=%zu)",
                             healed, strlen(messages));
                    append_text("(消息历史含非法 UTF-8 字节, 已就地修复并重发本请求)\r\n");
                    iter--;              /* 这次请求没发出去, 不计入迭代序号 */
                    continue;            /* 重新拼 body (messages 已变) 再发一次 */
                }
            }
            /* 提示要跟开关走: 日志关着时不会有留档, 说"详见 log\"就是假的 */
            append_text(g_log_enabled
                ? "(请求体含非法 UTF-8 字节, 已本地拦截未发送; 本轮回滚, 详见 log\\)\r\n"
                : "(请求体含非法 UTF-8 字节, 已本地拦截未发送; 本轮回滚)\r\n");
            reason = "请求体含非法 UTF-8, 本地拦截未发送";
            rolled_back = 1;
            goto done;
        }
        log_line("[http] 请求 iter=%d msgs_len=%zu body_len=%zu",
             iter, strlen(messages), strlen(body));

        append_text("(thinking...)\r\n");

        /* ctx 内含 1MB content_buf: 放堆, 不占工作线程的 1MB 栈。
         * 上一迭代已消费的 ctx 在此释放 (每迭代一个, 不能等 done 才释放)。 */
        free(ctx);
        ctx = (StreamCtx*)calloc(1, sizeof(StreamCtx));
        if (!ctx) {
            append_text("(内存不足, 本轮回滚)\r\n");
            append_text(ROLLBACK_HINT);
            reason = "内存不足 (StreamCtx 分配失败)";
            rolled_back = 1;
            goto done;
        }

        char full_url[1280];
        chat_completions_url(full_url, sizeof(full_url));

        if (cagent_stream_begin) cagent_stream_begin();   /* 标记新分段, 供重试时回删 */
        int status = http_post_stream(full_url, g_api_key, body, strlen(body),
                                      resp, BUFSZ, ctx);

        if (status == -2) {
            append_text("(已取消)\r\n");
            reason = "用户取消 (流式请求进行中)";
            rolled_back = 1;
            goto done;
        }
        if (status != 200) {
            /* 响应体按字符边界截取: %.1000s 会切半汉字, 让日志自己先变成非法 UTF-8 */
            char rprev[1024];
            utf8_safe_copy(resp, 1000, rprev, sizeof(rprev));
            log_line("[http] 失败 status=%d iter=%d msgs_len=%zu resp=%s",
                 status, iter, strlen(messages), rprev);
            log_dump_request(body, strlen(body));
            /* 服务端报告上下文超限 (400): 压缩后原地重试, 而不是回滚丢掉整轮。
             * 重试有次数上限; 压缩失败则照常回滚。 */
            if (status == 400 && overflow_retries < CONTEXT_OVERFLOW_RETRIES &&
                is_context_overflow_error(resp)) {
                overflow_retries++;
                append_text("(服务端报告上下文超限, 正在压缩后重试...)\r\n");
                free(ctx);
                ctx = NULL;
                if (compact_conversation()) {
                    log_line("[compact] 400超限触发 -> 成功, msgs_len=%zu", strlen(messages));
                    savepoint = strlen(messages);   /* 历史已改写, 回滚基点随之更新 */
                    continue;
                }
                log_line("[compact] 400超限触发 -> 失败");
                append_text("(压缩失败)\r\n");
            }
            append_text(resp);
            /* 请求阶段失败 = 没收到任何响应内容, messages 状态完整 (此前所有工具往返
             * 都已闭合): 保留已完成步骤, 只结束本轮 —— 学压缩失败的处理, 不让 429/网络
             * 抖动白白吃掉整轮 (文件改动本来也不回滚)。用户取消在上面另行回滚。 */
            append_text("(本轮未完成: 已完成的工具步骤保留在对话中, 可直接发下一条指令继续)\r\n");
            reason = "HTTP 请求失败 (保留已完成步骤, 不回滚)";
            goto done;
        }

        log_line("[http] 成功 iter=%d finish=%s prompt_tokens=%ld data_lines=%d bad_json=%d msgs_len=%zu",
             iter, ctx->finish[0] ? ctx->finish : "-", ctx->prompt_tokens,
             ctx->n_data_lines, ctx->n_bad_json, strlen(messages));

        /* 服务端报告的 prompt_tokens (与请求时的 messages 长度配对, 供水位估算) */
        if (ctx->prompt_tokens > 0) {
            g_last_prompt_tokens = ctx->prompt_tokens;
            g_last_prompt_bytes = strlen(messages);
        }

        if (strcmp(ctx->finish, "length") == 0)
            append_text("(达到长度上限被截断; 可让模型继续补全)\r\n");
        else if (strcmp(ctx->finish, "content_filter") == 0)
            append_text("(内容被服务端的过滤策略拦截)\r\n");

        /* content 装不下时补一条截断标记, 否则模型下一轮看不到自己说过什么,
         * 却以为说完了 (界面已完整显示)。 */
        if (ctx->truncated) {
            strncat(ctx->content_buf,
                    "\n...(回复过长, 超出缓冲上限, 上文只记录了前半部分)",
                    CONTENT_TAIL_RESERVE - 1);
            append_text("(模型回复过长, 上文只记录了前半部分)\r\n");
        }

        if (ctx->n_calls == 0 && ctx->n_dropped == 0) {
            /* 无 tool_calls: content 收尾。流式已逐字显示, 这里只追加换行 + messages。 */
            if (ctx->content_len > 0) {
                if (ctx->content_buf[ctx->content_len - 1] != '\n' &&
                    ctx->content_buf[ctx->content_len - 1] != '\r')
                    append_text("\r\n");   /* 末尾已有换行则不补, 避免双倍空行 */
                char *esc = json_escape_alloc(ctx->content_buf);
                if (esc) {
                    if (!msg_append(",{\"role\":\"assistant\",\"content\":\"%s\"}", esc)) {
                        append_text("(历史空间不足, 本轮回滚)\r\n");
                        reason = "历史空间不足 (assistant content 拼不进 messages)";
                        rolled_back = 1;
                    }
                    free(esc);
                } else {
                    /* 转义分配失败: 这轮回复不会进历史, 下一轮模型看不到自己说过什么 */
                    log_line("[turn] assistant content 转义失败(内存不足), 本轮回复未写入历史");
                }
            } else {
                /* 200 却没有 content: resp(=err_out) 在成功路径上是空的, 唯一线索就是
                 * 原始 SSE 首行与解析失败计数 —— 不记下来这条回滚根本无从下手。 */
                log_line("[http] 200 但无 content: finish=%s iter=%d data_lines=%d bad_json=%d sse_head=%s",
                         ctx->finish[0] ? ctx->finish : "-", iter,
                         ctx->n_data_lines, ctx->n_bad_json, ctx->sse_head);
                if (strcmp(ctx->finish, "length") == 0)
                    append_text("(响应为空且已达长度上限, 本轮回滚)\r\n");
                else
                    append_text("(响应解析失败)\r\n");
                append_text(ROLLBACK_HINT);
                reason = "服务端返回空 content (响应解析失败/空回复)";
                rolled_back = 1;
            }
            goto done;
        }

        /* 有 tool_calls: 转 AgentToolCall 并执行。
         * 调用表 (8 项 ≈ 2MB) 放堆: 放栈会和工作线程默认 1MB 栈争空间。 */
        calls = (AgentToolCall*)calloc(STREAM_CALLS_MAX, sizeof(AgentToolCall));
        if (!calls) {
            append_text("(内存不足, 本轮回滚)\r\n");
            reason = "内存不足 (工具调用表分配失败)";
            rolled_back = 1;
            goto done;
        }
        int n_calls = 0;
        for (int i = 0; i < ctx->n_calls && n_calls < STREAM_CALLS_MAX; i++) {
            if (!ctx->calls[i].name[0]) continue;
            AgentToolCall *c = &calls[n_calls++];
            snprintf(c->id,   sizeof(c->id),   "%s", ctx->calls[i].id);
            snprintf(c->name, sizeof(c->name), "%s", ctx->calls[i].name);
            c->too_big = ctx->calls[i].args_overflow;
            if (c->too_big)
                snprintf(c->args, sizeof(c->args), "{}");                    /* 残缺 JSON 不回传, 用占位 */
            else
                snprintf(c->args, sizeof(c->args), "%s", ctx->calls[i].args);/* 旧的写法先整段拷 256KB 再被覆盖 */
        }
        if (n_calls == 0 && ctx->n_dropped == 0) {
            log_line("[http] 200 但 tool_calls 无法还原: finish=%s iter=%d data_lines=%d bad_json=%d sse_head=%s",
                     ctx->finish[0] ? ctx->finish : "-", iter,
                     ctx->n_data_lines, ctx->n_bad_json, ctx->sse_head);
            append_text("(tool_calls 解析失败)\r\n");
            append_text(ROLLBACK_HINT);
            reason = "tool_calls 解析失败 (没能还原出可执行的调用)";
            rolled_back = 1;
            goto done;
        }

        for (int i = 0; i < n_calls; i++) {
            if (InterlockedCompareExchange(&g_cancel, 0, 0)) {
                append_text("(已取消)\r\n");
                reason = "用户取消 (工具执行中)";
                rolled_back = 1;
                goto done;
            }
            AgentToolCall *c = &calls[i];
            if (c->too_big) {
                /* arguments 超上限: 拒绝执行并把原因交回模型 (执行只会得到非法 JSON) */
                char msg[512];
                snprintf(msg, sizeof(msg), "[Tool] %s -> 未执行 (arguments 超过 %dKB 上限)\r\n",
                         c->name, (int)(ARGS_MAX / 1024));
                append_text(msg);
                snprintf(tool_out, sizeof(tool_out),
                         "(未执行: arguments 超过 %dKB 上限, 已拒绝; 大文件请改用 edit_file "
                         "分段修改, 或用 execute_bash 分批写入)", (int)(ARGS_MAX / 1024));
                log_line("[tool] 拒绝执行: name=%s arguments 超过 %dKB 上限",
                         c->name, (int)(ARGS_MAX / 1024));
            } else {
                /* [Tool] 提示行按展示上限堆分配, 不占栈; 执行与回传仍是完整参数。
                 * 摘要必须按字符边界截取 (utf8_safe_copy): 旧的 %.8192s 会切半汉字,
                 * 而且硬编码的 8192 与 ARGS_DISPLAY_MAX 脱钩 —— 改宏它不会跟着变。 */
                size_t cap = ARGS_DISPLAY_MAX + 256;
                char *line = (char*)malloc(cap);
                if (line) {
                    if (c->args[ARGS_DISPLAY_MAX]) {
                        char *prev = (char*)malloc(cap);
                        if (prev) {
                            utf8_safe_copy(c->args, ARGS_DISPLAY_MAX, prev, cap);
                            snprintf(line, cap, "[Tool] %s(%s…)\r\n", c->name, prev);
                            free(prev);
                        } else {
                            snprintf(line, cap, "[Tool] %s(参数过长, 未显示)\r\n", c->name);
                        }
                    } else {
                        snprintf(line, cap, "[Tool] %s(%s)\r\n", c->name, c->args);
                    }
                    append_text(line);
                    free(line);
                }
                dispatch_tool(c->name, c->args, ctx->finish);   /* 错误原因写进 tool_out 交回模型 */
            }
            c->output = strdup(tool_out);
            size_t tl = strlen(tool_out);
            char *oline = (char*)malloc(tl + 64);
            if (oline) {
                int eol = (tl > 0 && (tool_out[tl-1] == '\n' || tool_out[tl-1] == '\r'));
                snprintf(oline, tl + 64, eol ? "[Output]\r\n%s" : "[Output]\r\n%s\r\n", tool_out);
                append_text(oline);
                free(oline);
            }
        }

        /* 超上限被丢弃的调用: 界面如实说明"没执行", 与下面回填的 tool 结果一致 */
        for (int i = 0; i < ctx->n_dropped; i++) {
            char line[512];
            snprintf(line, sizeof(line),
                     "[Tool] %s -> 未执行 (超出单轮 %d 个并行调用上限, 请分批调用)\r\n",
                     ctx->dropped[i].name[0] ? ctx->dropped[i].name : "?", STREAM_CALLS_MAX);
            append_text(line);
        }

        /* 写回 messages: assistant 消息 (含 tool_calls 数组) + 每个调用的 tool 结果。
         * content 保留模型本轮的自然语言说明, 否则下一轮它看不到自己说过什么;
         * 说明过大放不下时退化为空串 (不影响工具调用本身)。 */
        size_t mstart = strlen(messages);
        char *esc_narr = (ctx->content_len > 0) ? json_escape_alloc(ctx->content_buf) : NULL;
        ok = msg_append(",{\"role\":\"assistant\",\"content\":\"%s\",\"tool_calls\":[",
                        esc_narr ? esc_narr : "");
        free(esc_narr);
        if (!ok)
            ok = msg_append(",{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[");

        for (int i = 0; i < n_calls && ok; i++) {
            AgentToolCall *c = &calls[i];
            char *ei = json_escape_alloc(c->id);
            char *en = json_escape_alloc(c->name);
            char *ea = json_escape_alloc(c->args);
            ok = ei && en && ea && msg_append(
                "%s{\"id\":\"%s\",\"type\":\"function\","
                "\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}",
                i ? "," : "", ei, en, ea);
            free(ei); free(en); free(ea);
        }

        /* 超上限被丢弃的调用也回填占位 tool_call: 否则模型以为它们已经执行过了 */
        for (int i = 0; ok && i < ctx->n_dropped; i++) {
            char *ei = json_escape_alloc(ctx->dropped[i].id);
            char *en = json_escape_alloc(ctx->dropped[i].name[0] ? ctx->dropped[i].name : "unknown");
            ok = ei && en && msg_append(
                "%s{\"id\":\"%s\",\"type\":\"function\","
                "\"function\":{\"name\":\"%s\",\"arguments\":\"{}\"}}",
                (i > 0 || n_calls > 0) ? "," : "", ei, en);
            free(ei); free(en);
        }
        if (ok) ok = msg_append("]}");

        for (int i = 0; i < n_calls && ok; i++) {
            char *ei = json_escape_alloc(calls[i].id);
            char *eo = json_escape_alloc(calls[i].output ? calls[i].output : "");
            ok = ei && eo && msg_append(
                ",{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":\"%s\"}", ei, eo);
            free(ei); free(eo);
        }

        /* 丢弃的调用: 回一条"未执行"的 tool 结果, 模型才知道要分批重来 */
        for (int i = 0; ok && i < ctx->n_dropped; i++) {
            char *ei = json_escape_alloc(ctx->dropped[i].id);
            ok = ei && msg_append(
                ",{\"role\":\"tool\",\"tool_call_id\":\"%s\",\"content\":"
                "\"(未执行: 本轮并行工具调用超过 %d 个上限, 这一次没有运行; "
                "请减少一次发出的调用数量, 分多轮执行)\"}",
                ei, STREAM_CALLS_MAX);
            free(ei);
        }

        for (int i = 0; i < n_calls; i++) free(calls[i].output);
        free(calls);
        calls = NULL;

        if (!ok) {
            /* 任一步失败: 把本次拼接的 assistant+tool 段全部截掉, 回滚整轮 */
            messages[mstart] = '\0';
            append_text("(历史空间或内存不足, 本轮回滚)\r\n");
            reason = "历史空间/内存不足 (assistant+tool 段拼装失败)";
            rolled_back = 1;
            goto done;
        }
    }
done:
    /* 兜底释放: 取消/中途出错时这些堆缓冲可能还没释放 */
    free(ctx);
    if (calls) {
        for (int i = 0; i < STREAM_CALLS_MAX; i++) free(calls[i].output);
        free(calls);
    }
    log_line("[turn] 结束 iter=%d 回滚=%d reason=%s msgs_len=%zu touched=%s",
             iter, rolled_back, reason ? reason : "-", strlen(messages),
             g_touched_files[0] ? g_touched_files : "-");
    /* reason 是给日志用的: 界面上那句"(本轮对话已回滚, ...)"对用户够用, 但事后排查必须
     * 能对上到底走的哪条路径 (取消/内存/UTF-8/解析失败/HTTP)。只记一个 回滚=1 定位不了。 */
    if (rolled_back) {
        messages[savepoint] = '\0';
        /* 对话回滚了, 但文件已经改了 —— 说清楚并列出改了哪些, 免得用户以为也还原了 */
        if (g_touched_files[0]) {
            char note[1200];
            snprintf(note, sizeof(note),
                     "(注意: 本轮已改动的文件不会随对话一起还原: %s)\r\n",
                     g_touched_files);
            append_text(note);
        }
    }
    history_save();   /* 每轮 done 后保存 (含回滚后状态) */
}

/* 后台线程入口: 跑一轮后通过钩子通知前端。 */
static CAGENT_MAYBE_UNUSED DWORD WINAPI agent_thread(LPVOID arg) {
    AgentTask *task = (AgentTask*)arg;
    agent_turn(task->user_msg);
    free(task);
    if (cagent_on_done) cagent_on_done();
    return 0;
}

