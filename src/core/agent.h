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
    int bad_json;  /* arguments 非合法 JSON (疑似被长度上限截断): 不执行, 降级占位 "{}" + 原因 */
    /* 服务端给的思考签名 (Gemini 3 的 extra_content.google.thought_signature)。
     * **跟着调用一起走**: 回写 assistant 消息时必须原样放回**同一条** tool_call 里,
     * 否则下一轮请求直接被 400 拒掉 (missing a thought_signature in functionCall parts)。
     * 空串 = 该条没有签名 (多数服务端都不发这个字段), 回写时原样省略。 */
    char sig[TOOL_SIG_MAX];
} AgentToolCall;

/* 内置默认系统提示词正文 (单行原文)。内容不含引号与反斜杠, 可直接作为 JSON 字符串值,
 * SYSTEM_PROMPT_DEFAULT 由它拼出 —— 保证"无外置文件也无技能"时与静态默认逐字节一致,
 * 测试二进制不调 system_prompt_init 也拿得到同样的提示词。 */
#define SYSTEM_PROMPT_CONTENT_DEFAULT \
    "你是 cagent,一个运行在 Windows 上的编程 Agent。回答简洁、直接。" \
    "【工作方式】先读相关文件和项目约定再动手,涉及构建或测试时再看 README、Makefile;不要凭猜测改代码。用户只要求分析、解释、审查或方案时,默认不要修改文件;需要运行命令取证时先说明目的,有副作用的命令按下面的安全规则确认。只改与任务相关的部分,不顺手重构、格式化或删除无关内容。" \
    "【工具】你有五个工具: execute_bash(执行命令)、read_file(读文件)、write_file(写文件)、edit_file(定点替换编辑)、load_skill(按名称加载用户预置的技能说明)。" \
    "【编辑】改已有文件的局部内容优先用 edit_file: old_text 必须与文件原文完全一致(含空白和换行)且在文件中唯一;报告未找到或多处匹配时,先 read_file 看清原文再重试。新建文件或需要整体重写时才用 write_file;覆盖已有文件前先 read_file 确认现有内容。不要用 edit_file 的空 new_text 删除不确定的内容,删除代码前先确认它确实无用。" \
    "【文件与命令】文件工具(read_file、write_file、edit_file)只能访问当前工作目录内,优先用相对路径;确实需要工作目录外的文件时,先说明原因并征得同意,再用 execute_bash 读取或写入。execute_bash 的当前目录就是工作目录,每条命令都是独立的 cmd 进程,cd 不会跨调用保留;需要切换目录时在同一条命令里用 cd /d 目录 && 命令,不确定当前目录时先运行 cd 确认。用 Windows 命令风格:不要 mkdir -p(直接 mkdir),运行当前程序不要 ./ 前缀。列目录用 dir /b,递归搜索内容用 findstr /s /i /c:关键词 *.*,关键词含空格时按 cmd 规则加引号;并尽量限定目录,避免扫描 .git、node_modules 等大目录。read_file 首次只回传约 6KB,大文件按截断提示里的 offset 续读;工具输出超限时不要根据半截内容下结论。" \
    "【安全】调用 execute_bash 前,先用一句话说明要执行的完整命令和目的;涉及高危操作时还必须说明风险,并停下来等用户明确确认,确认前不要调用工具。" \
    "高危包括:删除或覆盖文件(del、rd、rmdir /s、format、diskpart)、改注册表或系统服务(reg、sc、net user、schtasks)、关机重启、批量移动或重命名文件、下载后直接执行外部脚本、改写 git 历史(git push -f、reset --hard)、以及任何写入工作目录之外位置的操作。" \
    "用户确认只对当前这一条命令有效;命令、参数或影响范围变化时必须重新确认。用户拒绝时放弃该做法,改用更安全的方案。" \
    "【输出】需要表格时输出 ASCII 表格(用 + - | 画边框,列用空格对齐),表格前后各留一个空行;不要使用 Markdown 表格,表格内不要混入强调标记。任务不明确时先向用户澄清;任务完成后停止,简要总结改了什么、验证了什么、还有什么风险。"

/* 内置默认系统提示词 (完整 system 消息 JSON): 外置 SYSTEM_PROMPT 文件缺失或无效时的兜底。 */
#define SYSTEM_PROMPT_DEFAULT \
    "{\"role\":\"system\",\"content\":\"" SYSTEM_PROMPT_CONTENT_DEFAULT "\"}"

/* 默认串的静态指针 (与字符串字面量比较是未指定行为, 判定一律走这个指针)。 */
static const char *const g_prompt_default = SYSTEM_PROMPT_DEFAULT;

/* 实际生效的 system 消息 JSON。默认指向内置默认串; 启动时 exe 同目录存在
 * SYSTEM_PROMPT 文件的话, system_prompt_init() 替换为外置文件包装后的堆串
 * (进程生命周期内最多替换一次, 不释放)。所有 messages 偏移都按它计算。
 * 有技能时正文尾部会拼上技能索引 (skill.h 的 g_skills_suffix)。 */
static const char *g_system_prompt = g_prompt_default;

/* messages 缓冲水位线: 接近上限时触发压缩, 防止越界. */
#define MESSAGES_WATERMARK  ((BUFSZ * 3) / 4)

/* ===== 上下文压缩参数 ===== */
/* token 估算: 中文约 3 字节/token, 英文约 4; 取 3 偏保守 (高估 token → 提早压缩, 安全)。 */
#define BYTES_PER_TOKEN_EST        3
/* 摘要硬上限 (字节); 提示词要求模型控制在 2000 字以内, 此处兜底截断。 */
#define COMPACT_SUMMARY_MAX        8192
/* 摘要请求自己的输出预算 (token)。不要沿用主请求的 max_tokens: 摘要只需要一小段,
 * 而且输出预算也占模型窗口, 用 64K 会把本来放得下的旧历史顶成 400。 */
#define COMPACT_SUMMARY_MAX_TOKENS 4096
/* 压缩时尾部原样保留的预算 (字节): 摘要 + 最近原文, 避免刚读过的内容全部丢失。 */
#define COMPACT_TAIL_KEEP_BYTES    (32 * 1024)
/* 400 上下文超限后"压缩并重试"的最大次数, 防止压缩不奏效时无限打转。 */
#define CONTEXT_OVERFLOW_RETRIES   3

static const char *ROLLBACK_HINT = "\r\n(本轮对话已回滚, 不影响后续对话; 已执行的文件改动不会自动撤销)\r\n";

/* ===== 思考过程旁路存档 (reasonings) =====
 *
 * 推理模型的思考过程 (reasoning_content) 有两种"该不该留"的要求, 方向相反:
 *   ① 发给模型: **绝不能带**。DeepSeek 明确要求多轮对话不要回传 reasoning_content;
 *   ② 给用户看: 重载历史会话时**应该能看到** —— 业界惯例如此 (opencode 把 reasoning
 *      作为消息的一等 part 落盘), 否则一段长思考刷新后就凭空消失了。
 * 所以它必须**旁路**: 不进 messages (满足 ①), 但跟着会话文件落盘 (满足 ②)。
 *
 * 数据结构: 与 messages 里的 assistant **消息序号**一一对齐 —— reasonings[i] 是第 i 条
 * assistant 消息的思考正文 (NULL = 这条没有)。用序号对齐而不是存进消息 JSON 里, 是为了
 * 让"回传给模型的那份数据"结构完全不变: messages 照旧是纯净的 OpenAI 格式, 想带上思考
 * 都无从下手。代价是**每次改写 messages 都必须同步维护这个数组**:
 *   - 本轮新增 assistant -> reasonings_push (截断/无思考存 NULL 占位);
 *   - 本轮回滚 (done) -> reasonings_trim_tail;
 *   - 压缩 / 载入历史 -> reasonings_align_to (丢头或补 NULL 占位);
 *   - 落盘与载入在 session.h (session_write_reasonings / session_load_reasonings),
 *     文件里与 messages 同级存放, 数组长度必须等于文件里 assistant 条数。
 *
 * 内存: 只有推理模型才有内容, 非推理模型下 count 恒为 0, 零开销。 */
#define REASONINGS_MAX 512
static char *g_reasonings[REASONINGS_MAX];
static int   g_reasoning_n;      /* 已归档条数 == 历史里 assistant 消息的条数 */

static void reasonings_reset(void) {
    for (int i = 0; i < g_reasoning_n; i++) { free(g_reasonings[i]); g_reasonings[i] = NULL; }
    g_reasoning_n = 0;
}

/* 归档第 g_reasoning_n 条 assistant 消息的思考 (idx 必须正好落在末尾, 保证与消息序号对齐)。
 * text 为空 / 已截断则归档 NULL 占位 —— 占位也要占, 否则后面的下标全错位。 */
static void reasonings_push(const char *text) {
    if (g_reasoning_n >= REASONINGS_MAX) {
        /* 满了只能放弃。任何"丢最老/覆写"都会破坏与 assistant 消息的序号对齐,
         * 错位的存档比没有存档更糟; 保存端检测到 g_reasoning_n != assistant 条数时
         * 也不会落盘 (见 session.h 的 session_write_reasonings), 安全退化为无存档。 */
        log_line("[think] 思考存档达到上限 %d 条, 本条起不再归档 (对齐失效, 不再落盘)", REASONINGS_MAX);
        return;
    }
    g_reasonings[g_reasoning_n++] = (text && text[0]) ? strdup(text) : NULL;
}

/* 只丢尾部 count 条 (回滚用): 回滚总是截掉**最近**的若干条消息。 */
static void reasonings_trim_tail(int count) {
    while (count-- > 0 && g_reasoning_n > 0)
        free(g_reasonings[--g_reasoning_n]);
}

/* 按删掉的 assistant 条数从头部丢 (压缩丢弃早期消息用)。 */
static void reasonings_drop_head(int count) {
    if (count <= 0) return;
    if (count > g_reasoning_n) count = g_reasoning_n;
    for (int i = 0; i < count; i++) free(g_reasonings[i]);
    memmove(g_reasonings, g_reasonings + count,
            (size_t)(g_reasoning_n - count) * sizeof(g_reasonings[0]));
    g_reasoning_n -= count;
}

/* 头部补 count 条 NULL 占位 (载入旧格式 / 对齐用)。 */
static void reasonings_pad_head(int count) {
    if (count <= 0) return;
    if (count > REASONINGS_MAX - g_reasoning_n) count = REASONINGS_MAX - g_reasoning_n;
    if (count > 0) {
        memmove(g_reasonings + count, g_reasonings,
                (size_t)g_reasoning_n * sizeof(g_reasonings[0]));
        memset(g_reasonings, 0, (size_t)count * sizeof(g_reasonings[0]));
        g_reasoning_n += count;
    }
}

/* 把数组长度对齐到"messages 里现有 assistant 条数": 尾部对应关系保持不动,
 * 多的从头部丢 (压缩删了最早的若干 assistant), 少的头部补 NULL 占位
 * (载入了没有思考存档的会话, 这里补齐空位)。
 * **任何改写 messages 的操作 (压缩/载入) 之后都必须调一次** —— 不然后续 push
 * 从错误的位置开始, 存档与消息从此全部错位。 */
static void reasonings_align_to(int assistant_n) {
    if (assistant_n <= 0) { reasonings_reset(); return; }
    if (g_reasoning_n > assistant_n) reasonings_drop_head(g_reasoning_n - assistant_n);
    else                             reasonings_pad_head(assistant_n - g_reasoning_n);
}

/* 数一段对话 JSON 里 assistant 消息的条数 —— 定义挪到 msg_count/msg_bounds/msg_role
 * 之后 (见 msg_role 下方的实现; 这里只有前向声明, 供 reasonings 模块与本文件早前使用)。 */
static int count_assistant_msgs(const char *conv);

/* 初始化 messages 为只含 system prompt 的状态。 */
static void reset_conversation(void) {
    snprintf(messages, BUFSZ, "%s", g_system_prompt);
    reasonings_reset();      /* 会话重置 = 思考存档同步清空 (两者必须同生命周期) */
}

/* ===== 外置系统提示词 (exe 同目录的 SYSTEM_PROMPT 文件) ===== */

/* 外置提示词原文上限 (字节); 超限视为无效, 静默回退默认 */
#define SYSTEM_PROMPT_MAX_RAW (32 * 1024)

static void get_app_path(char *out, size_t cap, const char *filename);   /* 定义在 session.h */

/* 把提示词原文包装成 system 消息 JSON 并提交 (转义由 json_escape 负责,
 * 引号/换行/CRLF 都安全)。正文尾部拼上技能索引 (g_skills_suffix, 无技能时为空)。
 * 空原文或超限返回 0, g_system_prompt 保持原值。 */
static int system_prompt_set_raw(const char *raw) {
    if (!raw || !raw[0] || strlen(raw) > SYSTEM_PROMPT_MAX_RAW) return 0;
    char *cat = NULL;
    size_t slen = strlen(g_skills_suffix);
    if (slen > 0) {
        size_t rlen = strlen(raw);
        cat = (char*)malloc(rlen + slen + 1);
        if (!cat) return 0;
        memcpy(cat, raw, rlen);
        memcpy(cat + rlen, g_skills_suffix, slen + 1);
    }
    char *esc = json_escape_alloc(cat ? cat : raw);
    free(cat);
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

/* 无外置提示词时的兜底: 有技能索引则把内置默认正文 + 索引重新包装;
 * 无技能则保持静态默认 (零开销, 字节相同)。 */
static void system_prompt_apply_default(void) {
    if (g_skills_suffix[0] && g_system_prompt == g_prompt_default)
        system_prompt_set_raw(SYSTEM_PROMPT_CONTENT_DEFAULT);
}

/* 外置提示词的字节归一化 (仅用于缓存友好, 不改变语义):
 * 去 UTF-8 BOM, CRLF/CR -> LF。编辑器保存一次通常就会改变这些字节;
 * 而 system prompt 是每个请求的第 0 个 token, 变一个字节就让整段前缀缓存失效。 */
static void normalize_prompt_text(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    if (n >= 3 && (unsigned char)s[0] == 0xEF &&
                 (unsigned char)s[1] == 0xBB &&
                 (unsigned char)s[2] == 0xBF) {
        memmove(s, s + 3, n - 2);   /* 剩余内容 + NUL */
    }
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '\r') {
            if (r[1] == '\n') r++;   /* CRLF 只输出一个 LF */
            *w++ = '\n';
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

/* 启动时读取 exe 同目录的 SYSTEM_PROMPT 外置提示词。
 * 文件不存在 / 空 / 超限 / 转码后为空 -> 静默保持内置默认。 */
static void system_prompt_init(void) CAGENT_MAYBE_UNUSED;   /* 测试二进制不调用 (保持默认指针) */
static void system_prompt_init(void) {
    char path[MAX_PATH];
    get_app_path(path, sizeof(path), "SYSTEM_PROMPT");
    FILE *f = fopen_utf8(path, "rb");
    if (!f) { log_line("[prompt] 未找到外置提示词, 使用内置默认: %s", path); system_prompt_apply_default(); return; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); log_line("[prompt] 外置提示词读取失败: %s", path); system_prompt_apply_default(); return; }
    long fsz = ftell(f);
    if (fsz <= 0 || fsz > SYSTEM_PROMPT_MAX_RAW) {
        fclose(f);
        log_line("[prompt] 外置提示词无效 (%ld bytes, 上限 %d), 回退默认: %s",
             fsz, SYSTEM_PROMPT_MAX_RAW, path);
        system_prompt_apply_default();
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
    normalize_prompt_text(buf);   /* BOM / CRLF 归一化: 避免编辑器差异破坏前缀缓存 */
    n = strlen(buf);
    if (n == 0 || !is_valid_utf8((const unsigned char*)buf, n)) {
        free(buf);
        log_line("[prompt] 外置提示词转码后为空/仍非法 UTF-8, 回退默认: %s", path);
        system_prompt_apply_default();
        return;
    }
    if (system_prompt_set_raw(buf))
        log_line("[prompt] 使用外置提示词 %s (%zu bytes)", path, n);
    else {
        log_line("[prompt] 外置提示词包装失败, 回退默认: %s", path);
        system_prompt_apply_default();
    }
    free(buf);
}

/* 找出工具输出里"尾部说明"的起点 (以 \n...( 开头的**最后一段**), 没有则返回 NULL。
 *
 * 这类说明 (如 read_file 的"...(已截断: 文件共 N 字节...)") 是**本次调用的协议层信息**:
 * 模型靠它知道"内容被截断了、该用 offset 接着读", 但**用户不需要看到** —— 用户关心的是
 * 内容本身。所以上屏前把它剥掉, 回传模型的 tool_out 保持完整。
 * 取"最后一次出现"且要求它确实是最后一行 —— 文件内容里也可能出现同样的字样。 */
static const char *tool_note_split(const char *s) {
    const char *found = NULL;
    for (const char *p = s; (p = strstr(p, "\n...(")) != NULL; p++) found = p;
    if (!found) return NULL;
    const char *nl = strchr(found + 1, '\n');
    if (nl && nl[1]) return NULL;          /* 后面还有内容, 说明不是尾部说明 */
    return found;
}

/* 前向声明: agent_turn 调用历史保存, 其定义在 session.h (聚合顺序在本文件之后) */
static void history_save(void);

/* 拼接 chat/completions 完整 URL (agent_turn 与上下文压缩共用) */
static void chat_completions_url(char *out, size_t cap) {
    size_t nurl = strlen(g_api_url);
    snprintf(out, cap, "%s%schat/completions", g_api_url,
             (nurl > 0 && g_api_url[nurl - 1] == '/') ? "" : "/");
}

/* 主请求体的 max_tokens 片段 (摘要请求在 compact_via_summary 里单独限制)。
 * 必须显式下发: 缺省时服务端/中转自用保守默认 (常见 1024-4096), 而本模型是
 * 推理模型, 思考 token 又先消耗输出预算 —— 模型刚开始内联大文件就被
 * finish=length 掐断, 这是截断问题的根源。g_max_tokens<=0 时输出空串,
 * 不发该字段, 保持旧行为。 */
static void max_tokens_frag(char *out, size_t cap) {
    if (g_max_tokens > 0) snprintf(out, cap, ",\"max_tokens\":%d", g_max_tokens);
    else out[0] = '\0';
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
    /* 只在对象开头 96 字节内找 (role 恒在最前), 且保证 memcmp 不越过对象尾部:
     * 上限取 len-keylen, 与 96 取小。旧写法 scan 固定 96, 当 len∈[96,104] 时会读到
     * 对象尾部之外最多 9 字节 (对象位于 messages 尾部时即越界读)。 */
    size_t scan = len - keylen;
    if (scan > 96) scan = 96;
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

/* 数一段对话 JSON 里 assistant 消息的条数 (压缩对齐 / 落盘判定用, 定义见上声明)。 */
static int count_assistant_msgs(const char *conv) {
    size_t n = msg_count(conv);
    int cnt = 0;
    for (size_t i = 0; i < n; i++) {
        size_t s, e;
        if (!msg_bounds(conv, i, &s, &e)) break;
        char role[16];
        msg_role(conv + s, e - s, role, sizeof(role));
        if (strcmp(role, "assistant") == 0) cnt++;
    }
    return cnt;
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

/* 拼一条 tool_call 对象进 messages 的 assistant 消息。
 * leading_comma=1 表示它是数组第一项 (不加分隔逗号), 0 则补一个逗号。
 * sig 非空时附带 extra_content.google.thought_signature —— Gemini 3 要求这个签名原样
 * 带回同一条调用, 缺了它下一轮请求直接被 400 拒掉 (服务端只给一句 "missing a
 * thought_signature in functionCall parts", 看不出是序列化写错了还是没收全)。
 * 抽成函数就是为了能单测: 那一串括号、转义、逗号写错一位, 症状与别的原因长得一模一样。 */
static int msg_append_one_tool_call(const char *id, const char *name, const char *args,
                                    const char *sig, int leading_comma) {
    char *ei = json_escape_alloc(id ? id : "");
    char *en = json_escape_alloc(name ? name : "");
    char *ea = json_escape_alloc(args ? args : "");
    char *es = tool_sig_extra_alloc(sig);
    int ok = ei && en && ea && msg_append(
        "%s{\"id\":\"%s\",\"type\":\"function\"%s,"
        "\"function\":{\"name\":\"%s\",\"arguments\":\"%s\"}}",
        leading_comma ? "" : ",", ei, es ? es : "", en, ea);
    free(ei); free(en); free(ea); free(es);
    return ok;
}

/* ===== 超限识别 ===== */

/* 服务端 400 是否为"上下文超限": 各家措辞不一, 宽松匹配常见写法。
 * 大小写不敏感子串查找统一走 net.h 的 ascii_icontains (本文件不再另写一份)。 */
static int is_context_overflow_error(const char *err) {
    if (!err) return 0;
    return ascii_icontains(err, "context length")
        || ascii_icontains(err, "context_length")
        || ascii_icontains(err, "maximum context")
        || ascii_icontains(err, "too many tokens")
        || ascii_icontains(err, "prompt is too long")
        || ascii_icontains(err, "input length exceeds")
        || ascii_icontains(err, "reduce the length");
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

/* 可用于输入的 token 预算。context_tokens 是模型的**总窗口** (输入 + 输出),
 * max_tokens 也占同一窗口; 只按总窗口的 80% 判断会在输出预算较大时先撞 400,
 * 然后被动压缩 —— 既慢又会把好好的前缀缓存打掉。这里显式扣掉输出预算。
 * max_tokens 未配置或配置得比窗口还大时退回总窗口, 避免出现 0/负预算。 */
static long context_input_budget(void) {
    if (g_context_tokens <= 0) return 0;
    if (g_max_tokens > 0 && g_max_tokens < g_context_tokens)
        return g_context_tokens - g_max_tokens;
    return g_context_tokens;
}

/* 是否应当压缩: 配置了模型窗口 (ini 的 context_tokens) 则按输入预算的 90% 触发;
 * 未配置或服务端不发 usage 时退回字节水位 (75% BUFSZ)。
 * 90% 而不是 80%: 前缀缓存下, 压缩会改写前缀、丢掉整个缓存; 能晚一点压就晚一点。 */
static int context_pressure(void) {
    long budget = context_input_budget();
    if (budget > 0 &&
        estimate_prompt_tokens() > budget * 9 / 10) return 1;
    return strlen(messages) > MESSAGES_WATERMARK;
}

/* 压缩目标: 压完至少降到该字节数以下 (配置了窗口按输入预算折算, 否则水位的一半)。
 * 目标取输入预算的 50%: 压缩本身会丢掉旧前缀的缓存, 压完留足空间, 尽量少压下一次。 */
static size_t compact_target_bytes(void) {
    long budget = context_input_budget();
    if (budget > 0) {
        size_t t = (size_t)budget * BYTES_PER_TOKEN_EST / 2;
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

    /* 4. 摘要请求: system + [0,ks) + ask。
     * **带 tools + tool_choice:none**: 服务端通常把 tools 也算进缓存前缀; 摘要请求
     * 和正常请求保持相同前缀形状, 才能命中旧对话的 prefix cache。tool_choice:none
     * 则保证模型不会真的去调工具 (它只需要输出摘要)。 */
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

    size_t bcap = (size_t)rn + strlen(g_model) + strlen(TOOLS_JSON) + 256;
    char *bodybuf = (char*)malloc(bcap);
    if (!bodybuf) { free(req_msgs); return 0; }
    /* 摘要只输出一小段: 单独限制输出预算, 不沿用主请求的 max_tokens。 */
    int smt = (g_max_tokens > 0 && g_max_tokens < COMPACT_SUMMARY_MAX_TOKENS)
                  ? g_max_tokens : COMPACT_SUMMARY_MAX_TOKENS;
    char mt_frag[48];
    snprintf(mt_frag, sizeof(mt_frag), ",\"max_tokens\":%d", smt);
    snprintf(bodybuf, bcap,
             "{\"model\":\"%s\",\"messages\":[%s],\"tools\":%s,\"tool_choice\":\"none\","
             "\"stream\":true%s,\"stream_options\":{\"include_usage\":true}}",
             g_model, req_msgs, TOOLS_JSON, mt_frag);
    free(req_msgs);

    char url[1280];
    chat_completions_url(url, sizeof(url));
    StreamCtx *sctx = (StreamCtx*)calloc(1, sizeof(StreamCtx));
    if (!sctx) { free(bodybuf); return 0; }
    char err[512]; err[0] = '\0';
    if (cagent_stream_begin) cagent_stream_begin();
    int status = http_post_stream(url, g_api_key, bodybuf, strlen(bodybuf), err, sizeof(err), sctx);
    free(bodybuf);

    if (status == 200 && sctx->prompt_tokens > 0) {
        log_line("[compact] prompt=%ld cached=%ld hit=%.1f%%",
                 sctx->prompt_tokens, sctx->cached_tokens,
                 100.0 * (double)sctx->cached_tokens / (double)sctx->prompt_tokens);
    }

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
    stream_ctx_free(sctx);      /* 摘要请求也可能走 reasoning 模型 —— 不许裸 free (泄漏 reasoning_buf) */
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
    emit_note("(上下文已压缩为摘要, 最近的消息保留原文, 继续对话)\r\n");
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

    /* 先在堆上拼好新内容再一次性提交。旧写法先 reset_conversation() 再读 conv 保留段,
     * 而 conv 正指向 messages 内部 —— reset 会覆写 messages 头部, 只因保留段起点 ks>=1
     * 才没踩到源数据, 属隐式不变量。改成"堆上拼装"后与 compact_via_summary 一致, 不再依赖它。 */
    size_t plen = strlen(g_system_prompt);
    size_t tail = total - ks;
    if (plen + 1 + tail + 1 > BUFSZ) return 0;
    char *newbuf = (char*)malloc(plen + 1 + tail + 1);
    if (!newbuf) return 0;
    memcpy(newbuf, g_system_prompt, plen);
    newbuf[plen] = ',';
    memcpy(newbuf + plen + 1, conv + ks, tail);
    newbuf[plen + 1 + tail] = '\0';
    memcpy(messages, newbuf, plen + 1 + tail + 1);
    free(newbuf);
    char note[160];
    snprintf(note, sizeof(note),
             "(摘要压缩失败, 已按整条消息丢弃最早的 %d 条; 早期细节不再在上下文中)\r\n",
             (int)dropped_n);
    emit_note(note);
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
        /* 压缩丢掉最早的若干 assistant 消息, 思考存档跟着丢同样多条 —— 尾部对应关系
         * 不动 (保留段的 assistant 仍配原来的思考)。选数组的头部压缩条数就完成对齐。 */
        reasonings_align_to(count_assistant_msgs(messages + strlen(g_system_prompt)));
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
        emit_note("(上下文接近上限, 正在压缩为摘要...)\r\n");
        int cok = compact_conversation();
        log_line("[compact] 轮首触发 -> %s, msgs_len=%zu", cok ? "成功" : "失败", strlen(messages));
        if (!cok) {
            emit_err("(压缩失败, 已自动另起新会话)\r\n");
            reset_conversation();
            g_history_file[0] = '\0';
        }
    }
    if (messages[0] == '\0') reset_conversation();

    size_t savepoint = strlen(messages);   /* 出错回滚到这里, 丢弃本轮 user message */
    int reason_savepoint = g_reasoning_n;  /* 同上: 回滚时把本轮归档的思考一并丢掉 */
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
        emit_err("(内存不足, 已忽略本轮)\r\n");
        reason = "内存不足 (user 消息转义失败)";
        rolled_back = 1;
        goto done;
    }
    ok = msg_append(",{\"role\":\"user\",\"content\":\"%s\"}", escaped);
    free(escaped);
    if (!ok) {
        emit_err("(输入过长, 已忽略本轮)\r\n");
        reason = "输入过长 (user 消息拼不进 messages)";
        rolled_back = 1;
        goto done;                            /* 截断交给 done 统一做, 这里不再重复 */
    }

    for (;;) {
        if (InterlockedCompareExchange(&g_cancel, 0, 0)) {
            emit_note("(已取消)\r\n");
            reason = "用户取消 (轮次开始前)";
            rolled_back = 1;
            goto done;
        }
        /* 轮中水位检查: 长工具链任务单轮就能涨几十上百 KB, 迭代间隙消息序列完整,
         * 是安全的压缩点。压缩失败不回滚 —— 已完成的工具调用与结论都保留,
         * 本轮提前收束, 用户可继续下一条指令。 */
        if (iter > 0 && context_pressure()) {
            emit_note("(上下文接近上限, 正在压缩为摘要...)\r\n");
            int cok = compact_conversation();
            log_line("[compact] 迭代间隙触发 -> %s, msgs_len=%zu", cok ? "成功" : "失败", strlen(messages));
            if (!cok) {
                emit_err("(压缩失败, 本轮到此暂停; 已完成的步骤都保留在对话中)\r\n");
                reason = "上下文压缩失败 (保留已完成步骤, 不回滚)";
                goto done;
            }
            savepoint = strlen(messages);   /* 历史已改写, 回滚基点随之更新 */
        }
        iter++;

        char mt_frag[48];
        max_tokens_frag(mt_frag, sizeof(mt_frag));
        int blen = snprintf(body, sizeof(body),
            "{\"model\":\"%s\",\"messages\":[%s],\"tools\":%s,\"stream\":true%s,"
            "\"stream_options\":{\"include_usage\":true}}",
            g_model, messages, TOOLS_JSON, mt_frag);
        if (blen < 0 || (size_t)blen >= sizeof(body)) {
            /* BODY_SZ 的余量让这在数学上不可能发生; 真发生了也绝不能把半截 JSON
             * 发出去 —— 服务端只会回一个看不懂的 400。停本轮并留痕, 由用户压缩会话。 */
            log_line("[http] 请求体需要 %d 字节 > 上限 %d, 本轮中止", blen, (int)sizeof(body));
            emit_err("(请求体超出上限, 本轮已中止; 请压缩或另开会话)\r\n");
            reason = "请求体超出上限 (未发送)";
            goto done;
        }
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
                    emit_note("(消息历史含非法 UTF-8 字节, 已就地修复并重发本请求)\r\n");
                    iter--;              /* 这次请求没发出去, 不计入迭代序号 */
                    continue;            /* 重新拼 body (messages 已变) 再发一次 */
                }
            }
            /* 提示要跟开关走: 日志关着时不会有留档, 说"详见 log\"就是假的 */
            emit_err(g_log_enabled
                ? "(请求体含非法 UTF-8 字节, 已本地拦截未发送; 本轮回滚, 详见 log\\)\r\n"
                : "(请求体含非法 UTF-8 字节, 已本地拦截未发送; 本轮回滚)\r\n");
            reason = "请求体含非法 UTF-8, 本地拦截未发送";
            rolled_back = 1;
            goto done;
        }
        log_line("[http] 请求 iter=%d msgs_len=%zu body_len=%zu",
             iter, strlen(messages), strlen(body));

        /* 请求发出 → 状态栏显示"等待响应"。这类"此刻在做什么"的信息不再往输出区打旁白:
         * 它不属于对话内容, 混进去既占地方, 又会被历史保存和回放。 */
        set_status("等待响应…");

        /* ctx 内含 1MB content_buf: 放堆, 不占工作线程的 1MB 栈。
         * 上一迭代已消费的 ctx 在此释放 (每迭代一个, 不能等 done 才释放)。 */
        stream_ctx_free(ctx);
        ctx = (StreamCtx*)calloc(1, sizeof(StreamCtx));
        if (!ctx) {
            emit_err("(内存不足, 本轮回滚)\r\n");
            emit_err(ROLLBACK_HINT);
            reason = "内存不足 (StreamCtx 分配失败)";
            rolled_back = 1;
            goto done;
        }

        char full_url[1280];
        chat_completions_url(full_url, sizeof(full_url));

        if (cagent_stream_begin) cagent_stream_begin();   /* 标记新分段, 供重试时回删 */
        int status = http_post_stream(full_url, g_api_key, body, strlen(body),
                                      resp, BUFSZ, ctx);

        /* 流可能整个结束在思考阶段 (没有 content), 这里兜底闭合那个思考块 */
        close_reasoning_block(ctx);
        if (ctx->reasoning_len > 0)
            log_line("[think] 思考内容 %zu 字节%s (旁路存档, 不写入 messages)%s",
                     ctx->reasoning_len, ctx->content_len ? "" : " (本轮无正文)",
                     ctx->reasoning_trunc ? " — 超存档上限已截断, 本条不归档" : "");

        if (status == -2) {
            emit_note("(已取消)\r\n");
            reason = "用户取消 (流式请求进行中)";
            rolled_back = 1;
            goto done;
        }
        if (status == -3) {
            /* 200 但流未收完整 (重试耗尽): 不能当成功处理半截回复, 也不能当网络失败。
             * 保留已完成步骤、不回滚 —— 与 HTTP 失败路径同一语义, 用户可直接重发。 */
            log_line("[http] 响应流不完整 (重试耗尽) iter=%d data_lines=%d bad_json=%d",
                     iter, ctx->n_data_lines, ctx->n_bad_json);
            emit_err("(响应流中断, 重试后仍未收完整; 本轮未完成, 可直接重发)\r\n");
            reason = "响应流不完整 (重试耗尽, 非完整 200)";
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
                emit_note("(服务端报告上下文超限, 正在压缩后重试...)\r\n");
                stream_ctx_free(ctx);
                ctx = NULL;
                if (compact_conversation()) {
                    log_line("[compact] 400超限触发 -> 成功, msgs_len=%zu", strlen(messages));
                    savepoint = strlen(messages);   /* 历史已改写, 回滚基点随之更新 */
                    continue;
                }
                log_line("[compact] 400超限触发 -> 失败");
                emit_err("(压缩失败)\r\n");
            }
            /* 界面给人看的结论 (额度/鉴权/限流有友好文案), 原始报文已在上面进日志 */
            char fr[512];
            if (http_friendly_error(status, resp, fr, sizeof(fr))) emit_err(fr);
            else emit_err(resp);
            /* 请求阶段失败 = 没收到任何响应内容, messages 状态完整 (此前所有工具往返
             * 都已闭合): 保留已完成步骤, 只结束本轮 —— 学压缩失败的处理, 不让 429/网络
             * 抖动白白吃掉整轮 (文件改动本来也不回滚)。用户取消在上面另行回滚。 */
            emit_note("(本轮未完成: 已完成的工具步骤保留在对话中, 可直接发下一条指令继续)\r\n");
            reason = "HTTP 请求失败 (保留已完成步骤, 不回滚)";
            goto done;
        }

        log_line("[http] 成功 iter=%d finish=%s prompt_tokens=%ld cached_tokens=%ld "
                 "data_lines=%d bad_json=%d msgs_len=%zu",
             iter, ctx->finish[0] ? ctx->finish : "-", ctx->prompt_tokens,
             ctx->cached_tokens, ctx->n_data_lines, ctx->n_bad_json, strlen(messages));

        /* 服务端报告的 prompt_tokens (与请求时的 messages 长度配对, 供水位估算)。
         * cached_tokens 只用于成本/命中率观察: 命中的 token 仍然占模型窗口,
         * 不能从上下文水位里扣掉。 */
        if (ctx->prompt_tokens > 0) {
            g_last_prompt_tokens = ctx->prompt_tokens;
            g_last_prompt_bytes = strlen(messages);
            log_line("[usage] prompt=%ld cached=%ld hit=%.1f%%",
                     ctx->prompt_tokens, ctx->cached_tokens,
                     ctx->prompt_tokens > 0
                         ? 100.0 * (double)ctx->cached_tokens / (double)ctx->prompt_tokens
                         : 0.0);
        }

        if (strcmp(ctx->finish, "length") == 0)
            emit_note("(达到长度上限被截断; 可让模型继续补全)\r\n");
        else if (strcmp(ctx->finish, "content_filter") == 0)
            emit_err("(内容被服务端的过滤策略拦截)\r\n");

        /* content 装不下时补一条截断标记, 否则模型下一轮看不到自己说过什么,
         * 却以为说完了 (界面已完整显示)。 */
        if (ctx->truncated) {
            strncat(ctx->content_buf,
                    "\n...(回复过长, 超出缓冲上限, 上文只记录了前半部分)",
                    CONTENT_TAIL_RESERVE - 1);
            emit_note("(模型回复过长, 上文只记录了前半部分)\r\n");
        }

        if (ctx->n_calls == 0 && ctx->n_dropped == 0) {
            /* 无 tool_calls: content 收尾。流式已逐字显示, 这里只追加换行 + messages。 */
            if (ctx->content_len > 0) {
                if (ctx->content_buf[ctx->content_len - 1] != '\n' &&
                    ctx->content_buf[ctx->content_len - 1] != '\r')
                    emit_role("\r\n", CAGENT_ROLE_AI);   /* 末尾已有换行则不补, 避免双倍空行 */
                char *esc = json_escape_alloc(ctx->content_buf);
                if (esc) {
                    if (!msg_append(",{\"role\":\"assistant\",\"content\":\"%s\"}", esc)) {
                        emit_err("(历史空间不足, 本轮回滚)\r\n");
                        reason = "历史空间不足 (assistant content 拼不进 messages)";
                        rolled_back = 1;
                    } else {
                        /* 消息进历史成功, 它的思考也随之归档 (序号对齐, 见 reasonings[])。
                         * 截断的思考不存 —— 存半截反而与实际不符。 */
                        reasonings_push(ctx->reasoning_trunc ? NULL : ctx->reasoning_buf);
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
                    emit_err("(响应为空且已达长度上限, 本轮回滚)\r\n");
                else
                    emit_err("(响应解析失败)\r\n");
                emit_err(ROLLBACK_HINT);
                reason = "服务端返回空 content (响应解析失败/空回复)";
                rolled_back = 1;
            }
            goto done;
        }

        /* 有 tool_calls: 先把 AI 正文收尾。
         * **必须补换行**: 紧接着就要发 "▸ 工具名", 正文末尾若没有换行, 两段会挤在同一行;
         * 更麻烦的是前端的段落格式(缩进/段间距)按段落生效, 不闭合会让工具行的缩进
         * 把整段 AI 正文一起带上。 */
        if (ctx->content_len > 0 &&
            ctx->content_buf[ctx->content_len - 1] != '\n' &&
            ctx->content_buf[ctx->content_len - 1] != '\r')
            emit_role("\r\n", CAGENT_ROLE_AI);

        /* 转 AgentToolCall 并执行。
         * 调用表 (8 项 ≈ 2MB) 放堆: 放栈会和工作线程默认 1MB 栈争空间。 */
        calls = (AgentToolCall*)calloc(STREAM_CALLS_MAX, sizeof(AgentToolCall));
        if (!calls) {
            emit_err("(内存不足, 本轮回滚)\r\n");
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
            snprintf(c->sig,  sizeof(c->sig),  "%s", ctx->calls[i].sig);   /* 空串=无签名 */
            c->too_big = ctx->calls[i].args_overflow;
            if (c->too_big) {
                snprintf(c->args, sizeof(c->args), "{}");                    /* 残缺 JSON 不回传, 用占位 */
            } else {
                /* BUG-1 修复: arguments 若不是合法 JSON 对象 (典型为 finish=length 截断成
                 * 空串/半截), 不能原样写回 messages —— 服务端会校验历史 tool_calls.arguments
                 * 必须为合法 JSON, 否则下一轮请求直接 400。降级为"未执行"占位。 */
                const char *raw = ctx->calls[i].args;
                JValue *aj = json_parse(raw);   /* 空串/NULL 解析失败 -> 走 bad_json 降级 */
                if (aj && aj->type == J_OBJ) {
                    snprintf(c->args, sizeof(c->args), "%s", raw);           /* 合法: 原样保留 */
                } else {
                    c->bad_json = 1;
                    snprintf(c->args, sizeof(c->args), "{}");                /* 占位, 避免把坏串写进历史 */
                    log_bad_tool_args(c->name, ctx->finish, raw);
                }
                json_free(aj);
            }
        }
        if (n_calls == 0 && ctx->n_dropped == 0) {
            log_line("[http] 200 但 tool_calls 无法还原: finish=%s iter=%d data_lines=%d bad_json=%d sse_head=%s",
                     ctx->finish[0] ? ctx->finish : "-", iter,
                     ctx->n_data_lines, ctx->n_bad_json, ctx->sse_head);
            emit_err("(tool_calls 解析失败)\r\n");
            emit_err(ROLLBACK_HINT);
            reason = "tool_calls 解析失败 (没能还原出可执行的调用)";
            rolled_back = 1;
            goto done;
        }

        for (int i = 0; i < n_calls; i++) {
            if (InterlockedCompareExchange(&g_cancel, 0, 0)) {
                emit_note("(已取消)\r\n");
                reason = "用户取消 (工具执行中)";
                rolled_back = 1;
                goto done;
            }
            AgentToolCall *c = &calls[i];
            if (c->too_big) {
                /* arguments 超上限: 拒绝执行并把原因交回模型 (执行只会得到非法 JSON)。
                 * 界面按"工具名 + 原因"两行显示, 与正常调用的排版一致, 只是换成错误色。 */
                char msg[512];
                snprintf(msg, sizeof(msg), "× %s\r\n", c->name);
                emit_err(msg);
                snprintf(msg, sizeof(msg), "未执行: arguments 超过 %dKB 上限\r\n",
                         (int)(ARGS_MAX / 1024));
                emit_err(msg);
                snprintf(tool_out, sizeof(tool_out),
                         "(未执行: arguments 超过 %dKB 上限, 已拒绝; 大文件请改用 edit_file "
                         "分段修改, 或用 execute_bash 分批写入)", (int)(ARGS_MAX / 1024));
                log_line("[tool] 拒绝执行: name=%s arguments 超过 %dKB 上限",
                         c->name, (int)(ARGS_MAX / 1024));
            } else if (c->bad_json) {
                /* BUG-1 修复: arguments 非合法 JSON (典型 finish=length 截断成空串/半截),
                 * 拒绝执行, 把原因交回模型让它重新生成; arguments 已降级为占位 "{}", 不污染历史。 */
                int truncated = (strcmp(ctx->finish, "length") == 0);
                char msg[512];
                snprintf(msg, sizeof(msg), "× %s\r\n", c->name);
                emit_err(msg);
                snprintf(msg, sizeof(msg), "未执行: arguments 不是合法 JSON%s\r\n",
                         truncated ? " (疑似被长度上限截断)" : "");
                emit_err(msg);
                snprintf(tool_out, sizeof(tool_out),
                         "(未执行: %s 的 arguments 不是合法 JSON%s, 已拒绝; 请完整重新发出该调用 "
                         "(包含全部必需参数, 不要省略或发空对象); 参数过大就分段写, "
                         "改大文件优先用 edit_file 定点补全)",
                         c->name, truncated ? " (疑似被长度上限截断)" : "");
                log_line("[tool] 拒绝执行: name=%s arguments 非法 JSON (疑似截断=%d)",
                         c->name, truncated);
            } else {
                /* 工具调用分两行发: 标题行(工具名) + 参数行。参数行独立成一个角色, 前端才能
                 * 用等宽字体、缩进在标题之下 —— 挤成一行 "[Tool] name(args)" 的话, 名字和
                 * 参数只能共用一种样式, 长参数还会把工具名顶到看不见的地方。
                 * 参数按展示上限堆分配, 不占栈; 执行与回传仍是完整参数。
                 * 摘要必须按字符边界截取 (utf8_safe_copy): 旧的 %.8192s 会切半汉字,
                 * 而且硬编码的 8192 与 ARGS_DISPLAY_MAX 脱钩 —— 改宏它不会跟着变。 */
                size_t cap = ARGS_DISPLAY_MAX + 256;
                char *line = (char*)malloc(cap);
                if (line) {
                    /* 首字符是折叠符号: ▾ = 展开(内容可见), ▸ = 收起。前端的块索引表
                     * 靠它定位工具块, 也只替换这一个字符 —— 见 ui/state.h 的 UiFoldBlock。 */
                    snprintf(line, cap, CAGENT_FOLD_GLYPH_OPEN " 调用工具 %s\r\n", c->name);
                    emit_tool(line);
                    if (c->args[ARGS_DISPLAY_MAX]) {
                        char *prev = (char*)malloc(cap);
                        if (prev) {
                            utf8_safe_copy(c->args, ARGS_DISPLAY_MAX, prev, cap);
                            snprintf(line, cap, "%s…\r\n", prev);
                            free(prev);
                        } else {
                            snprintf(line, cap, "(参数过长, 未显示)\r\n");
                        }
                    } else {
                        snprintf(line, cap, "%s\r\n", c->args);
                    }
                    emit_args(line);
                    free(line);
                }
                {
                    char st[128];
                    snprintf(st, sizeof(st), "正在执行 %s…", c->name);
                    set_status(st);
                }
                dispatch_tool(c->name, c->args, ctx->finish);   /* 错误原因写进 tool_out 交回模型 */
            }
            c->output = strdup(tool_out);
            size_t tl = strlen(tool_out);
            /* 输出正文不再带 "[Output]" 标签: 它紧跟在 "▸ 工具名" 之后, 前端又用等宽+底色
             * 单独成块, 再写个标签是冗余。空输出给一句说明, 免得留一个看不懂的空块。
             *
             * **尾部说明只回传给模型, 不上屏**: "(已截断: 文件共 N 字节, 用 offset=... 继续读)"
             * 这类话属于协议层 —— 模型靠它知道怎么接着读, 但用户关心的是内容本身, 不是
             * "这次读了多少字节"。这条边界要划清: 回传给模型的 ≠ 显示给用户的。 */
            const char *note = tool_note_split(tool_out);
            size_t shown = note ? (size_t)(note - tool_out) : tl;   /* 上屏的只有正文部分 */
            char *oline = (char*)malloc(shown + 64);
            if (oline) {
                if (shown == 0) {
                    snprintf(oline, 64, "(无输出)\r\n");
                } else {
                    int eol = (tool_out[shown-1] == '\n' || tool_out[shown-1] == '\r');
                    snprintf(oline, shown + 64, eol ? "%.*s" : "%.*s\r\n", (int)shown, tool_out);
                }
                emit_output(oline);
                free(oline);
            }
        }

        /* 超上限被丢弃的调用: 界面如实说明"没执行", 与下面回填的 tool 结果一致 */
        for (int i = 0; i < ctx->n_dropped; i++) {
            char line[512];
            snprintf(line, sizeof(line), "× %s\r\n",
                     ctx->dropped[i].name[0] ? ctx->dropped[i].name : "?");
            emit_err(line);
            snprintf(line, sizeof(line),
                     "未执行: 超出单轮 %d 个并行调用上限, 请分批调用\r\n", STREAM_CALLS_MAX);
            emit_err(line);
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
            /* 思考签名跟着这条调用一起回写 (无签名时序列化形状与旧版逐字节一致) */
            ok = msg_append_one_tool_call(c->id, c->name, c->args, c->sig, i == 0);
        }

        /* 超上限被丢弃的调用也回填占位 tool_call: 否则模型以为它们已经执行过了 */
        for (int i = 0; ok && i < ctx->n_dropped; i++) {
            ok = msg_append_one_tool_call(ctx->dropped[i].id,
                                          ctx->dropped[i].name[0] ? ctx->dropped[i].name : "unknown",
                                          "{}", "", i > 0 || n_calls > 0);
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
            emit_err("(历史空间或内存不足, 本轮回滚)\r\n");
            reason = "历史空间/内存不足 (assistant+tool 段拼装失败)";
            rolled_back = 1;
            goto done;
        }
        /* assistant 段拼装成功 = 这条消息真的进历史了, 思考随之归档 (序号对齐)。
         * **必须放在 ok 判定之后**: 半截的 assistant 会被上面截掉, 那时归档就是错位。 */
        reasonings_push(ctx->reasoning_trunc ? NULL : ctx->reasoning_buf);
    }
done:
    /* 兜底释放: 取消/中途出错时这些堆缓冲可能还没释放 */
    stream_ctx_free(ctx);
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
        /* 对话回滚了, 这轮归档的思考也一并丢掉 —— 两者必须同步, 否则存档与消息错位 */
        reasonings_trim_tail(g_reasoning_n - reason_savepoint);
        /* 对话回滚了, 但文件已经改了 —— 说清楚并列出改了哪些, 免得用户以为也还原了 */
        if (g_touched_files[0]) {
            char note[1200];
            snprintf(note, sizeof(note),
                     "(注意: 本轮已改动的文件不会随对话一起还原: %s)\r\n",
                     g_touched_files);
            emit_err(note);
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

