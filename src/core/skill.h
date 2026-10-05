/*
 * core/skill.h - 技能: exe 同目录 skills\<名称>\SKILL.md
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 *
 * 设计 (与 SYSTEM_PROMPT 外置提示词同一套路):
 *   - 启动时递归扫描 <exe>\skills\ 树, 任意深度的 SKILL.md 都算一个技能 (标准
 *     agent 的技能布局: 目录里放 SKILL.md 即成, 不要求固定层级); 回退名取
 *     SKILL.md 所在目录名。环境变量 CAGENT_SKILLS_DIR 可指向别的技能根 (测试用)。
 *   - SKILL.md 可带 frontmatter (--- 包住的 name:/description:), 缺省回退
 *     目录名 / 正文第一个非空行。
 *   - 技能索引拼进系统提示词 (g_skills_suffix, 由 agent.h 的 system_prompt_set_raw
 *     追加到提示词正文), 让模型知道有哪些技能、何时该用 load_skill。
 *   - load_skill 工具按名称返回技能正文; 未知名返回可用列表, 由模型自行纠正。
 * 会话中途不重扫 (技能集进程内固定); 新增技能重启生效。
 */

/* 单个技能元数据; 总量按"个人工具箱"量级设防, 不做动态扩容 */
#define SKILL_MAX        32
#define SKILL_NAME_MAX   64
#define SKILL_DESC_MAX   192
/* SKILL.md 原文上限: 超限不加载 (整段进上下文的东西必须有界)。
 * GBK -> UTF-8 最坏膨胀 1.5 倍, 读入缓冲按 2x 开。 */
#define SKILL_FILE_MAX   (32 * 1024)
/* 技能索引 (追加进系统提示词) 的硬上限 */
#define SKILL_SUFFIX_MAX (16 * 1024)

typedef struct {
    char name[SKILL_NAME_MAX];
    char desc[SKILL_DESC_MAX];
    char path[MAX_PATH];        /* SKILL.md 绝对路径 (UTF-8) */
} Skill;

static Skill g_skills[SKILL_MAX];
static int   g_skill_count = 0;
static char  g_skills_suffix[SKILL_SUFFIX_MAX] = "";

/* 前向声明: get_app_path 定义在 session.h (聚合顺序在本文件之后), 同 agent.h 的做法 */
static void get_app_path(char *out, size_t cap, const char *filename);

/* ===== frontmatter 解析 ===== */

/* 从 text 里解析 --- 包住的 name:/description:。没有 frontmatter 或字段缺失时
 * 对应输出为空串, 由调用方回退 (name -> 目录名, desc -> 正文首行)。 */
static void skill_parse_frontmatter(const char *text, char *name, size_t ncap,
                                    char *desc, size_t dcap) {
    name[0] = '\0'; desc[0] = '\0';
    if (strncmp(text, "---", 3) != 0) return;         /* 首行必须是 --- */
    const char *p = text + 3;
    if (*p == '\r') p++;
    if (*p == '\n') p++;
    const char *end = strstr(p, "\n---");            /* 收尾 --- 行 */
    if (!end) return;

    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        size_t len = eol ? (size_t)(eol - p) : (size_t)(end - p);
        const char *s = p;
        size_t sl = len;
        /* 去行尾 CR 与两端空白 */
        while (sl > 0 && (s[sl-1] == '\r' || s[sl-1] == ' ' || s[sl-1] == '\t')) sl--;
        while (sl > 0 && (*s == ' ' || *s == '\t')) { s++; sl--; }
        if (sl > 5 && strncmp(s, "name:", 5) == 0) {
            s += 5; sl -= 5;
            while (sl > 0 && (*s == ' ' || *s == '\t')) { s++; sl--; }
            utf8_safe_copy(s, sl, name, ncap);
        } else if (sl > 12 && strncmp(s, "description:", 12) == 0) {
            s += 12; sl -= 12;
            while (sl > 0 && (*s == ' ' || *s == '\t')) { s++; sl--; }
            utf8_safe_copy(s, sl, desc, dcap);
        }
        if (!eol) break;
        p = eol + 1;
    }
}

/* ===== 技能正文定位 ===== */

/* 跳过 frontmatter 返回正文指针 (指向 text 内部); 无 frontmatter 原样返回 */
static const char *skill_body(const char *text) {
    if (strncmp(text, "---", 3) != 0) return text;
    const char *p = text + 3;
    if (*p == '\r') p++;
    if (*p == '\n') p++;
    const char *end = strstr(p, "\n---");
    if (!end) return text;
    p = end + 4;                                     /* 越过 "\n---" */
    if (*p == '\r') p++;
    if (*p == '\n') p++;
    return p;
}

/* 读文本文件并保证 UTF-8 (非法时按 GBK/ANSI 转码, 与外置提示词同一策略)。
 * 成功返回堆串 (调用方 free), 失败/超限/空文件返回 NULL。 */
static char *skill_read_utf8(const char *path) {
    size_t n = 0, cap = 0;
    /* 2x 容量: GBK -> UTF-8 转码膨胀余量 (最坏 1.5 倍) */
    char *buf = read_file_alloc(path, SKILL_FILE_MAX, 1, &n, &cap);
    if (!buf) return NULL;
    /* UTF-8 BOM 先剥掉: 不剥的话 strncmp("---",3) 会因 EF BB BF 前缀失配,
     * frontmatter 被静默当成普通正文。GBK 文件不会有这个前缀, 顺序无关。 */
    if (n >= 3 && (unsigned char)buf[0] == 0xEF &&
                  (unsigned char)buf[1] == 0xBB &&
                  (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, n - 2);                /* 剩余内容 + NUL */
        n -= 3;
    }
    if (!is_valid_utf8((const unsigned char*)buf, n)) {
        oem_to_utf8(buf, cap);
    }
    if (!is_valid_utf8((const unsigned char*)buf, strlen(buf))) { free(buf); return NULL; }
    return buf;
}

/* ===== 扫描与索引 ===== */

/* 递归深度上限: 纯防御 (路径受 MAX_PATH 约束实际到不了), 再深视为配置事故 */
#define SKILL_SCAN_DEPTH_MAX 8

/* 加载一个 SKILL.md 并入池 (重名只收先到的)。从 skills_scan_dir 的枚举项调用,
 * 池满与否由调用方在枚举循环顶检查。 */
static void skills_load_file(const char *file) {
    char *text = skill_read_utf8(file);
    if (!text) return;                            /* 超限 / 空文件 / 坏编码: 跳过 */

    Skill *sk = &g_skills[g_skill_count];
    char name[SKILL_NAME_MAX], desc[SKILL_DESC_MAX];
    skill_parse_frontmatter(text, name, sizeof(name), desc, sizeof(desc));

    /* 回退名 = 直接父目录名 ("...\<父>\SKILL.md" 里取 <父>) */
    char parentu8[MAX_PATH] = "";
    const char *sep = strrchr(file, '\\');
    if (sep && sep > file) {
        const char *p2 = sep - 1;
        while (p2 > file && *(p2 - 1) != '\\') p2--;
        size_t pl = (size_t)(sep - p2);
        if (pl >= sizeof(parentu8)) pl = sizeof(parentu8) - 1;
        memcpy(parentu8, p2, pl);
        parentu8[pl] = '\0';
    }

    /* 回退名 (父目录名) 可能长于 SKILL_NAME_MAX: 这里就是要它的前 N 字符做索引, 截断是预期的 */
    if (name[0]) str_copy_into(sk->name, sizeof(sk->name), name);
    else         str_copy_into(sk->name, sizeof(sk->name), parentu8);
    if (!desc[0]) {
        const char *body_text = skill_body(text);  /* 别叫 body: 遮蔽 state.h 的全局 body[BUFSZ] */
        while (*body_text == '\r' || *body_text == '\n') body_text++;
        /* frontmatter 未闭合时 skill_body 拿到的是整个文件: 回退描述跳过 "---"
         * 行与后续键值行, 否则索引里出现 "---"/"name: x" */
        if (strncmp(body_text, "---", 3) == 0) {
            for (;;) {
                if (strncmp(body_text, "---", 3) != 0) {
                    const char *c = body_text;     /* 键值行 "key:" 继续跳, 其他行作描述 */
                    while (*c && (isalnum((unsigned char)*c) || *c == '_' || *c == '-')) c++;
                    if (*c != ':') break;
                }
                const char *eol2 = strchr(body_text, '\n');
                if (!eol2) { body_text += strlen(body_text); break; }
                body_text = eol2 + 1;
                while (*body_text == '\r' || *body_text == '\n') body_text++;
            }
        }
        const char *eol = strchr(body_text, '\n');
        size_t bl = eol ? (size_t)(eol - body_text) : strlen(body_text);
        while (bl > 0 && (body_text[bl-1] == '\r' || body_text[bl-1] == ' ')) bl--;
        utf8_safe_copy(body_text, bl, sk->desc, sizeof(sk->desc));
    } else {
        snprintf(sk->desc, sizeof(sk->desc), "%s", desc);
    }
    snprintf(sk->path, sizeof(sk->path), "%s", file);

    int dup = 0;
    for (int i = 0; i < g_skill_count; i++)
        if (strcmp(g_skills[i].name, sk->name) == 0) { dup = 1; break; }
    if (dup) { free(text); return; }

    g_skill_count++;
    char dprev[160];
    utf8_safe_copy(sk->desc, 120, dprev, sizeof(dprev));
    log_line("[skill] 已加载: %s (%s) <- %s", sk->name, dprev, file);
    free(text);
}

/* 递归扫描 root 树, 发现任意深度的 SKILL.md (与标准 agent 的技能布局一致:
 * 目录里放 SKILL.md 即成技能, 不要求固定层级)。回退名取 SKILL.md 所在目录名。
 * 重名 (含跨根与根互相嵌套导致的重复发现) 先到先得。
 * 跳过: 点开头目录、junction/符号链接 (防环)。 */
static void skills_scan_dir(const char *root, int depth) {
    wchar_t wroot[MAX_PATH];
    if (!utf8_to_wide(root, wroot, MAX_PATH)) return;
    wchar_t pattern[MAX_PATH];
    if (swprintf(pattern, MAX_PATH, L"%ls\\*", wroot) < 0) return;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (g_skill_count >= SKILL_MAX) break;
        if (fd.cFileName[0] == L'.') continue;       /* . 与 .. 及隐藏目录 */

        char nameu8[MAX_PATH], entry[MAX_PATH];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                                nameu8, sizeof(nameu8), NULL, NULL) <= 0) continue;
        int m1 = snprintf(entry, sizeof(entry), "%s\\%s", root, nameu8);
        if (m1 < 0 || (size_t)m1 >= sizeof(entry)) continue;

        if (wcsicmp(fd.cFileName, L"SKILL.md") == 0) {
            skills_load_file(entry);
            continue;
        }
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (depth >= SKILL_SCAN_DEPTH_MAX) continue;
        skills_scan_dir(entry, depth + 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 把技能索引拼进 g_skills_suffix (追加到系统提示词的文本, 会被整体转义) */
static void skills_build_suffix(void) {
    g_skills_suffix[0] = '\0';
    if (g_skill_count == 0) return;
    size_t pos = 0;
    const char *head =
        "\n\n## 可用技能\n"
        "除基础工具外, 你还有一个 load_skill 工具, 可按名称加载用户预置的技能说明。"
        "当任务与下面某个技能相关时, 先调用 load_skill 获取该技能的完整说明, "
        "再严格按说明执行任务。当前可用技能:\n";
    size_t hlen = strlen(head);
    memcpy(g_skills_suffix, head, hlen + 1);
    pos = hlen;
    for (int i = 0; i < g_skill_count; i++) {
        char line[SKILL_NAME_MAX + SKILL_DESC_MAX + 8];
        int n = snprintf(line, sizeof(line), "- %s: %s\n", g_skills[i].name, g_skills[i].desc);
        if (n <= 0 || (size_t)n >= sizeof(line)) continue;
        if (pos + (size_t)n >= sizeof(g_skills_suffix) - 1) break;   /* 放不下就到此为止 */
        memcpy(g_skills_suffix + pos, line, (size_t)n + 1);
        pos += (size_t)n;
    }
}

/* 单个技能根目录解析: 绝对路径原样使用; 相对值按 **exe 目录**解析 (与 ini 的
 * workspace= 同一规矩, 不随进程 CWD 漂移), 并做幂等归一化。
 * "C:\" 这类根形式保留结尾反斜杠 (剥成 "C:" 就成了盘符相对路径, 含义全变)。
 * ini 手写误差容忍: 首尾空白与成对引号剥掉 —— 路径含空格直接写即可,
 * 引号可写可不写; 分隔符是 ';', 路径里出现 ';' 无法转义 (极端路径改名绕开)。 */
static void skills_resolve_one(const char *src0, char *out, size_t cap) {
    const char *src = src0;
    size_t sl = strlen(src);
    while (sl > 0 && (src[0] == ' ' || src[0] == '\t')) { src++; sl--; }
    while (sl > 0 && (src[sl-1] == ' ' || src[sl-1] == '\t')) sl--;
    if (sl >= 2 && src[0] == '"' && src[sl-1] == '"') { src++; sl -= 2; }
    char tok[MAX_PATH];
    if (sl >= sizeof(tok)) sl = sizeof(tok) - 1;
    memcpy(tok, src, sl);
    tok[sl] = '\0';
    int absolute = (tok[0] == '\\' || tok[0] == '/' || (tok[0] && tok[1] == ':'));
    if (absolute) {
        path_copy(out, cap, tok);
    } else {
        char exedir[MAX_PATH], cand[PATHSZ];
        get_exe_dir_utf8(exedir, sizeof(exedir));
        if (!exedir[0]) { path_copy(out, cap, tok); return; }
        path_copy(cand, sizeof(cand), exedir);
        path_append(cand, sizeof(cand), "\\");
        path_append(cand, sizeof(cand), tok);
        wchar_t win[PATHSZ], wout[PATHSZ];
        if (!utf8_to_wide(cand, win, (int)sizeof(win)) ||
            GetFullPathNameW(win, PATHSZ, wout, NULL) == 0 ||
            WideCharToMultiByte(CP_UTF8, 0, wout, -1, out, (int)cap, NULL, NULL) <= 0)
            path_copy(out, cap, cand);          /* 归一化失败: 用未规整的候选值 */
    }
    size_t len = strlen(out);
    while (len > 2 && (out[len-1] == '\\' || out[len-1] == '/') && out[len-2] != ':')
        out[--len] = '\0';
}

/* 启动时调用一次 (GUI 在 system_prompt_init 之前)。缺失目录不是错误, 只是没技能。
 * 技能根可配多个: ini 的 skills_dir / CAGENT_SKILLS_DIR 里用 ';' 分隔
 * (同 Windows PATH 风格), 按顺序扫描, 重名先到先得 (跨目录去重)。 */
static void skills_init(void) CAGENT_MAYBE_UNUSED;   /* 测试二进制按需自行调用 */
static void skills_init(void) {
    g_skill_count = 0;
    g_skills_suffix[0] = '\0';
    const char *env = getenv("CAGENT_SKILLS_DIR");
    const char *list = g_skills_dir[0] ? g_skills_dir : ((env && env[0]) ? env : NULL);
    char root[MAX_PATH];
    if (list) {
        const char *p = list;
        while (*p) {
            const char *semi = strchr(p, ';');
            size_t len = semi ? (size_t)(semi - p) : strlen(p);
            if (len > 0 && len < MAX_PATH) {
                char tok[MAX_PATH];
                memcpy(tok, p, len);
                tok[len] = '\0';
                skills_resolve_one(tok, root, sizeof(root));
                int before = g_skill_count;
                skills_scan_dir(root, 0);
                log_line("[skill] 技能根目录: %s (+%d)", root, g_skill_count - before);
            }
            if (!semi) break;
            p = semi + 1;
        }
    } else {
        get_app_path(root, sizeof(root), "skills");
        skills_scan_dir(root, 0);
        log_line("[skill] 技能根目录: %s", root);
    }
    skills_build_suffix();
    log_line("[skill] 共 %d 个技能", g_skill_count);
}

/* ===== load_skill 工具实现 (写入 tool_out; 分发在 tools.h 的 dispatch_tool) ===== */

static void tool_load_skill(const char *name) {
    if (!name || !name[0]) {
        snprintf(tool_out, BUFSZ, "(参数缺失: load_skill 需要字符串参数 \"name\")");
        return;
    }
    if (g_skill_count == 0) {
        snprintf(tool_out, BUFSZ,
                 "(当前没有可用技能: 技能目录为空或不存在; "
                 "技能是 exe 同目录 skills\\<名称>\\SKILL.md)");
        return;
    }
    int idx = -1;
    for (int i = 0; i < g_skill_count; i++)
        if (strcmp(g_skills[i].name, name) == 0) { idx = i; break; }
    if (idx < 0) {
        /* 未知名: 列出可用技能交回模型, 让它自己纠正, 不静默猜测 */
        char list[1024];
        size_t pos = 0;
        list[0] = '\0';
        for (int i = 0; i < g_skill_count && pos < sizeof(list) - 2; i++) {
            int n = snprintf(list + pos, sizeof(list) - pos, "%s%s",
                             i ? ", " : "", g_skills[i].name);
            if (n < 0 || (size_t)n >= sizeof(list) - pos) break;
            pos += (size_t)n;
        }
        snprintf(tool_out, BUFSZ, "(未找到技能 \"%s\"; 可用技能: %s)", name, list);
        return;
    }
    char *text = skill_read_utf8(g_skills[idx].path);
    if (!text) {
        snprintf(tool_out, BUFSZ, "(技能读取失败: %s)", g_skills[idx].path);
        return;
    }
    const char *body_text = skill_body(text);      /* 别叫 body: 遮蔽 state.h 的全局 body[BUFSZ] */
    /* tool_out (BUFSZ) 是最终边界, 直接写、超长自然截断; blen 必 ≤ 32KB (读入已限) */
    snprintf(tool_out, BUFSZ, "[技能: %s]\n\n%.*s\n\n(以上是技能说明, 请严格按它执行当前任务)",
             g_skills[idx].name, (int)strlen(body_text), body_text);
    free(text);
}
