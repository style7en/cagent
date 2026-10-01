/*
 * ui/helpers.h - UI 辅助: Edit 读写(UTF-8) 与 RichEdit 分角色追加
 *
 * cagent 界面层的一部分, 由 cagent_ui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== UI 辅助 ===== */

static void read_edit_utf8(HWND h, char *out, size_t cap) {
    int wlen = GetWindowTextLengthW(h);
    if (wlen <= 0) return;
    WCHAR *wbuf = (WCHAR*)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    GetWindowTextW(h, wbuf, wlen + 1);
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)cap, NULL, NULL);
    free(wbuf);
}

static void set_edit_utf8(HWND h, const char *utf8) {
    if (!utf8 || !*utf8) return;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return;
    WCHAR *wbuf = (WCHAR*)malloc((size_t)wlen * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wlen);
    SetWindowTextW(h, wbuf);
    free(wbuf);
}

/* 主线程处理: UTF-8 -> UTF-16, LF 补 CRLF, 按角色着色后追加到历史框末尾。
 * emoji 代理对区段显式用 Segoe UI Emoji 字体渲染 (YaHei 无字形, 字体绑定回退会渲染成 '?')。 */
static void do_append(const char *utf8, int role) {
    size_t in_len = strlen(utf8);
    char *norm = (char*)malloc(in_len * 2 + 1);
    if (!norm) return;
    size_t j = 0;
    for (size_t i = 0; i < in_len; i++) {
        char c = utf8[i];
        if (c == '\n' && (i == 0 || utf8[i-1] != '\r')) {
            norm[j++] = '\r';
            norm[j++] = '\n';
        } else {
            norm[j++] = c;
        }
    }
    norm[j] = '\0';

    int wlen = MultiByteToWideChar(CP_UTF8, 0, norm, -1, NULL, 0);
    if (wlen <= 0) { free(norm); return; }
    WCHAR *wbuf = (WCHAR*)malloc((size_t)wlen * sizeof(WCHAR));
    if (!wbuf) { free(norm); return; }
    MultiByteToWideChar(CP_UTF8, 0, norm, -1, wbuf, wlen);
    free(norm);

    /* 当前插入点的字符格式 (字体/字号) 作为普通文本基底 */
    CHARFORMAT2W base;
    memset(&base, 0, sizeof(base));
    base.cbSize = sizeof(base);
    SendMessageW(g_hHistory, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&base);

    /* 按角色叠加颜色: 用户=蓝加粗, 系统/思考提示=灰, AI=默认黑; 不斜体, 字号随基底。
     * 注意必须显式清掉 CFE_AUTOCOLOR —— 它随插入点基底带来, 置位时 RichEdit 会忽略
     * crTextColor, 颜色设置将完全无效 (症状: 所有文字都是系统黑)。同时压掉 ITALIC,
     * 避免插入点周围残留的斜体格式传染给新文本。 */
    CHARFORMAT2W role_cf = base;
    role_cf.dwMask    |= CFM_COLOR | CFM_BOLD | CFM_ITALIC;
    role_cf.dwEffects &= ~(CFE_AUTOCOLOR | CFE_BOLD | CFE_ITALIC);
    role_cf.crTextColor = RGB(0, 0, 0);            /* AI: 默认黑 */
    if (role == CAGENT_ROLE_USER) {
        role_cf.crTextColor = RGB(0, 90, 200);     /* 用户: 蓝 */
        role_cf.dwEffects |= CFE_BOLD;             /* 用户: 加粗 */
    } else if (role == CAGENT_ROLE_SYS) {
        role_cf.crTextColor = RGB(110, 110, 110);  /* 系统/思考: 灰 */
    }

    /* 按 run 插入: 连续 emoji 代理对为一段(用 Segoe UI Emoji), 其余为普通段 */
    int total = lstrlenW(wbuf);
    int i = 0;
    while (i < total) {
        BOOL emo = (wbuf[i] >= 0xD800 && wbuf[i] <= 0xDBFF &&
                    wbuf[i+1] >= 0xDC00 && wbuf[i+1] <= 0xDFFF);
        int j = i;
        if (emo) {
            while (j + 1 < total && wbuf[j] >= 0xD800 && wbuf[j] <= 0xDBFF &&
                   wbuf[j+1] >= 0xDC00 && wbuf[j+1] <= 0xDFFF) j += 2;
        } else {
            while (j < total && !(wbuf[j] >= 0xD800 && wbuf[j] <= 0xDBFF &&
                                  j + 1 < total && wbuf[j+1] >= 0xDC00 && wbuf[j+1] <= 0xDFFF)) j++;
        }

        CHARFORMAT2W cf = role_cf;
        if (emo) {
            cf.dwMask |= CFM_FACE;
            wcscpy(cf.szFaceName, L"Segoe UI Emoji");
        }
        int len = GetWindowTextLengthW(g_hHistory);
        SendMessageW(g_hHistory, EM_SETSEL, len, len);
        SendMessageW(g_hHistory, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        { WCHAR save = wbuf[j]; wbuf[j] = L'\0';
          SendMessageW(g_hHistory, EM_REPLACESEL, FALSE, (LPARAM)(wbuf + i));
          wbuf[j] = save; }
        i = j;
    }
    SendMessageW(g_hHistory, EM_SCROLLCARET, 0, 0);
    SendMessageW(g_hHistory, WM_VSCROLL, SB_BOTTOM, 0);   /* 强制滚到末尾 */
    if (role == CAGENT_ROLE_AI) g_stream_chars += total;  /* 供重试时回删本分段 */
    free(wbuf);
}
