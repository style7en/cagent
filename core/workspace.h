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

/* 把截断长度回退到 UTF-8 字符边界: 避免切出半个字符 (非法 UTF-8 会让整段文本被丢弃) */
static size_t utf8_trim_len(const char *s, size_t len) {
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        size_t need;
        if (c >= 0xF0)      need = 4;
        else if (c >= 0xE0) need = 3;
        else if (c >= 0xC0) need = 2;
        else                need = 1;
        if (need > 1 && i + need > len) break;      /* 该字符被截断 -> 停 */
        int ok = 1;
        for (size_t k = 1; k < need; k++)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) { ok = 0; break; }
        if (!ok) break;                             /* 非法序列 -> 停 */
        i += need;
    }
    return i;
}

/* UTF-8 路径打开文件: 必须走宽字符, 否则非 ASCII 路径 (中文目录) 会打不开 */
static FILE *fopen_utf8(const char *path, const char *mode) {
    wchar_t wp[MAX_PATH], wm[16];
    if (!utf8_to_wide(path, wp, MAX_PATH)) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (n <= 0 || n > 16) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, n);
    return _wfopen(wp, wm);
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

/* 确保 g_workspace 已初始化 (默认 exe 目录) */
static void ensure_workspace(void) {
    if (g_workspace[0]) return;
    get_exe_dir_utf8(g_workspace, sizeof(g_workspace));
}

/* 检查 path 规范化后是否在 g_workspace 内。返回 1 合法, 0 非法。 */
static int path_in_workspace(const char *path) {
    ensure_workspace();
    wchar_t wbase[MAX_PATH], wabs[MAX_PATH], wws[MAX_PATH], wwsfull[MAX_PATH];
    if (path[0] == '\\' || path[0] == '/' || (path[0] && path[1] == ':')) {
        if (!utf8_to_wide(path, wbase, MAX_PATH)) return 0;
    } else {
        char base[MAX_PATH];
        int m = snprintf(base, sizeof(base), "%s\\%s", g_workspace, path);
        if (m < 0 || (size_t)m >= sizeof(base)) return 0;
        if (!utf8_to_wide(base, wbase, MAX_PATH)) return 0;
    }
    DWORD n = GetFullPathNameW(wbase, MAX_PATH, wabs, NULL);
    if (n == 0 || n >= MAX_PATH) return 0;
    if (!utf8_to_wide(g_workspace, wws, MAX_PATH)) return 0;
    n = GetFullPathNameW(wws, MAX_PATH, wwsfull, NULL);
    if (n == 0 || n >= MAX_PATH) return 0;
    size_t wl = wcslen(wwsfull);
    if (wcsncmp(wabs, wwsfull, wl) != 0) return 0;
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
    char full[MAX_PATH];
    int m = snprintf(full, sizeof(full), "%s\\%s", g_workspace, path);
    if (m < 0 || (size_t)m >= sizeof(full)) return 0;
    return utf8_to_wide(full, out, cap);
}

/* ===== 结构化工具 (写入全局 tool_out) ===== */
