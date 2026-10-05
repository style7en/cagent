/*
 * core/workspace.h - 工作目录解析、越界限制、UTF-8 文件/路径
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== 工作目录限制 ===== */

/* UTF-8 -> UTF-16, 写入 out (cap 为 wchar 数)。成功返回非 0。 */
static int utf8_to_wide(const char *u8, wchar_t *out, int cap) {
    return MultiByteToWideChar(CP_UTF8, 0, u8, -1, out, cap) > 0;
}

/* 字符边界工具 (utf8_trim_len / utf8_skip_cont / utf8_safe_copy / utf8_seq_len / is_valid_utf8)
 * 已统一收敛到 core/utf8.h, 见那里的说明。 */

/* UTF-8 路径打开文件: 必须走宽字符, 否则非 ASCII 路径 (中文目录) 会打不开 */
static FILE *fopen_utf8(const char *path, const char *mode) {
    wchar_t wp[MAX_PATH], wm[16];
    if (!utf8_to_wide(path, wp, MAX_PATH)) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (n <= 0 || n > 16) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, n);
    return _wfopen(wp, wm);
}

/* 读整个文件到堆缓冲 (NUL 结尾)。会话文件、技能正文、元信息三处曾各写一份完全相同的
 * fseek/ftell/malloc/fread 样板, 收敛到这里。
 * path 需为**绝对路径** (内部走 fopen_utf8, 不做工作目录解析 —— 现有调用方传的都是
 * sessions\ 或 skills\ 下的绝对路径)。
 * 失败返回 NULL: 打不开 / 取长失败 / 空文件 / 超过 max_bytes (>0 时)。
 * grow_2x: 额外预留一倍容量 (cap = 2*fsz+1), 供后续 GBK->UTF-8 原地转码膨胀用 (最坏 1.5x)。
 * out_len 收到实际读入字节数; out_cap 收到分配的容量 (两者可为 NULL)。 */
static char *read_file_alloc(const char *path, long max_bytes, int grow_2x,
                             size_t *out_len, size_t *out_cap) {
    if (out_len) *out_len = 0;
    if (out_cap) *out_cap = 0;
    FILE *f = fopen_utf8(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long fsz = ftell(f);
    if (fsz <= 0 || (max_bytes > 0 && fsz > max_bytes)) { fclose(f); return NULL; }
    rewind(f);
    size_t cap = (size_t)fsz * (grow_2x ? 2u : 1u) + 1;
    char *buf = (char*)malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)fsz, f);
    fclose(f);
    if (n == 0) { free(buf); return NULL; }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    if (out_cap) *out_cap = cap;
    return buf;
}

/* 取 exe 所在目录 (UTF-8, 无结尾反斜杠); 失败回退当前目录 */
static void get_exe_dir_utf8(char *out, size_t cap) {
    wchar_t wexe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, wexe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        wchar_t wcd[MAX_PATH];
        if (GetCurrentDirectoryW(MAX_PATH, wcd) == 0) { out[0] = '\0'; return; }
        WideCharToMultiByte(CP_UTF8, 0, wcd, -1, out, (int)cap, NULL, NULL);
        return;
    }
    wchar_t *slash = wcsrchr(wexe, L'\\');
    if (slash) *slash = L'\0';
    WideCharToMultiByte(CP_UTF8, 0, wexe, -1, out, (int)cap, NULL, NULL);
}

/* 把 g_workspace 归一化成绝对路径并去掉结尾反斜杠 (幂等)。
 *
 * 为什么必须做: path_in_workspace 内部用 GetFullPathNameW 规范化待校验路径, 而它解析
 * **相对路径**时依据的是**当时的进程 CWD**; 这个 CWD 又会被 ui/task.h 的
 * SetCurrentDirectoryW 反复改写 —— ini 里一旦填了相对的 workspace=, "哪些路径算在工作
 * 目录内"就会随历史操作漂移 (校验基准与执行位置分家)。归一化后判据不再依赖 CWD。
 *
 * 相对值按 **exe 目录**解析 (与 ini/log/sessions 的锚点一致), 不按 CWD —— 否则同一个 ini
 * 从不同目录启动会得到不同的工作目录, 那才是最糟的"不稳定"。解析失败保留原值, 不清空配置。 */
static void normalize_workspace(void) {
    if (!g_workspace[0]) return;

    char cand[PATHSZ];
    int absolute = (g_workspace[0] == '\\' || g_workspace[0] == '/' ||
                    (g_workspace[0] && g_workspace[1] == ':'));
    if (absolute) {
        snprintf(cand, sizeof(cand), "%s", g_workspace);
    } else {
        char exedir[MAX_PATH];
        get_exe_dir_utf8(exedir, sizeof(exedir));
        if (!exedir[0]) return;
        snprintf(cand, sizeof(cand), "%s\\%s", exedir, g_workspace);
    }

    wchar_t win[PATHSZ], wout[PATHSZ];
    if (!utf8_to_wide(cand, win, (int)sizeof(win))) return;
    DWORD n = GetFullPathNameW(win, PATHSZ, wout, NULL);
    if (n == 0 || n >= PATHSZ) return;

    char out[PATHSZ];
    if (WideCharToMultiByte(CP_UTF8, 0, wout, -1, out, sizeof(out), NULL, NULL) <= 0) return;

    /* 去掉结尾反斜杠, 与 get_exe_dir_utf8 保持一致。但 "C:\" 不能剥成 "C:" ——
     * 那是"盘符相对路径", 含义完全不同, 故保留长度 2 的根形式。 */
    size_t len = strlen(out);
    while (len > 2 && (out[len-1] == '\\' || out[len-1] == '/')) out[--len] = '\0';

    if (strcmp(out, g_workspace) != 0) {           /* 只在真的变了时记一次 (归一化后即幂等) */
        if (len >= sizeof(g_workspace)) {          /* 归一化结果放不下: 保留原值, 不清空配置 */
            log_line("[ws] workspace 归一化结果超出缓冲上限 (%d 字符), 保留原值: %s",
                     (int)sizeof(g_workspace) - 1, g_workspace);
            return;
        }
        log_line("[ws] workspace 归一化: %s -> %s", g_workspace, out);
        path_copy(g_workspace, sizeof(g_workspace), out);
    }
}

/* 确保 g_workspace 已初始化 (无值则默认 exe 目录) 并已归一化。
 * 归一化是幂等的, 所以在每个入口都调一次是安全的 —— 见 normalize_workspace 的说明。 */
static void ensure_workspace(void) {
    if (!g_workspace[0]) get_exe_dir_utf8(g_workspace, sizeof(g_workspace));
    normalize_workspace();
}

/* 检查 path 规范化后是否在 g_workspace 内。返回 1 合法, 0 非法。
 * 缓冲用 PATHSZ: 工作目录内较深的子路径 (g_workspace + 相对段) 长度可超过 MAX_PATH,
 * 用 MAX_PATH 承接 GetFullPathNameW 会失败, 把合法路径误判为越界。 */
static int path_in_workspace(const char *path) {
    ensure_workspace();
    wchar_t wbase[PATHSZ], wabs[PATHSZ], wws[PATHSZ], wwsfull[PATHSZ];
    if (path[0] == '\\' || path[0] == '/' || (path[0] && path[1] == ':')) {
        if (!utf8_to_wide(path, wbase, PATHSZ)) return 0;
    } else {
        char base[PATHSZ];
        int m = snprintf(base, sizeof(base), "%s\\%s", g_workspace, path);
        if (m < 0 || (size_t)m >= sizeof(base)) return 0;
        if (!utf8_to_wide(base, wbase, PATHSZ)) return 0;
    }
    DWORD n = GetFullPathNameW(wbase, PATHSZ, wabs, NULL);
    if (n == 0 || n >= PATHSZ) return 0;
    if (!utf8_to_wide(g_workspace, wws, PATHSZ)) return 0;
    n = GetFullPathNameW(wws, PATHSZ, wwsfull, NULL);
    if (n == 0 || n >= PATHSZ) return 0;
    size_t wl = wcslen(wwsfull);
    if (_wcsnicmp(wabs, wwsfull, wl) != 0) return 0;   /* 大小写不敏感, 避免盘符大小写不同被误拒 */
    if (wabs[wl] != L'\\' && wabs[wl] != L'\0') return 0;
    return 1;
}

/* 把 path 解析为工作目录内的实际路径: 相对路径在 g_workspace 下拼接,
 * 绝对路径 (盘符或 \ 开头) 原样使用。写入 out (cap 为 wchar 数)。返回 1 成功。
 * 这样文件工具的枚举/打开目标与沙箱校验一致, 避免 CWD≠工作目录时越界。 */
static int resolve_in_workspace(const char *path, wchar_t *out, int cap) {
    ensure_workspace();
    if (path[0] == '\\' || path[0] == '/' || (path[0] && path[1] == ':')) {
        return utf8_to_wide(path, out, cap);
    }
    char full[PATHSZ];
    int m = snprintf(full, sizeof(full), "%s\\%s", g_workspace, path);
    if (m < 0 || (size_t)m >= sizeof(full)) return 0;
    return utf8_to_wide(full, out, cap);
}

/* ===== 结构化工具 (写入全局 tool_out) ===== */
