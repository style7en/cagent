/*
 * ui/helpers.h - UI 辅助: Edit 读写(UTF-8) 与 RichEdit 分角色追加
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== UI 辅助 ===== */

static void read_edit_utf8(HWND h, char *out, size_t cap) {
    if (cap == 0) return;
    int wlen = GetWindowTextLengthW(h);
    /* 空输入必须显式把 out 清空: 否则用户把某个配置框清空后, 全局仍保留旧值 ——
     * 表现为"清空 Model/Key/工作目录 后仍按旧配置发送", 且退出保存时把旧值又写回。 */
    if (wlen <= 0) { out[0] = '\0'; return; }
    WCHAR *wbuf = (WCHAR*)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    if (!wbuf) { out[0] = '\0'; return; }
    GetWindowTextW(h, wbuf, wlen + 1);
    out[0] = '\0';
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)cap, NULL, NULL);
    free(wbuf);
}

static void set_edit_utf8(HWND h, const char *utf8) {
    /* 空值也要下发: 与 read_edit_utf8 对称, 否则界面无法反映"已清空"的真实状态 */
    if (!utf8 || !*utf8) { SetWindowTextW(h, L""); return; }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) { SetWindowTextW(h, L""); return; }
    WCHAR *wbuf = (WCHAR*)malloc((size_t)wlen * sizeof(WCHAR));
    if (!wbuf) return;
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
    /* **刻意不设 CFM_SIZE / yHeight** —— 字号一律继承控件的默认字体。
     * 踩过的两个坑, 都是"给 run 写死字号"引出来的:
     *   ① CHARFORMAT.yHeight 是 em 高, 而 WM_SETFONT 的 CreateFontW(h) 是**字身格高**;
     *      YaHei 的 格/em ≈ 1.27, 于是写死 yHeight 的字比默认字体大 27%,
     *      而行距是按默认字体的 tmHeight 算的 -> 字被行距削顶(实测削掉 10px)。
     *   ② WM_SETFONT 会让 RichEdit 丢掉 run 上的显式字号 —— 一旦跨显示器触发
     *      WM_DPICHANGED 里那轮 ui_fonts_rebuild, 文字会从"大"跳回"默认", 看着像换了套排版。
     * 缩放输出区字号请走 EM_SETZOOM(见 wndproc.h 的 ui_history_zoom), 它不碰字符格式。 */
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
        int end = i;
        if (emo) {
            while (end + 1 < total && wbuf[end] >= 0xD800 && wbuf[end] <= 0xDBFF &&
                   wbuf[end+1] >= 0xDC00 && wbuf[end+1] <= 0xDFFF) end += 2;
        } else {
            while (end < total && !(wbuf[end] >= 0xD800 && wbuf[end] <= 0xDBFF &&
                                  end + 1 < total && wbuf[end+1] >= 0xDC00 && wbuf[end+1] <= 0xDFFF)) end++;
        }

        CHARFORMAT2W cf = role_cf;
        if (emo) {
            cf.dwMask |= CFM_FACE;
            wcscpy(cf.szFaceName, L"Segoe UI Emoji");
        }
        int len = GetWindowTextLengthW(g_hHistory);
        SendMessageW(g_hHistory, EM_SETSEL, len, len);
        SendMessageW(g_hHistory, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        { WCHAR save = wbuf[end]; wbuf[end] = L'\0';
          SendMessageW(g_hHistory, EM_REPLACESEL, FALSE, (LPARAM)(wbuf + i));
          wbuf[end] = save; }
        i = end;
    }
    SendMessageW(g_hHistory, EM_SCROLLCARET, 0, 0);
    SendMessageW(g_hHistory, WM_VSCROLL, SB_BOTTOM, 0);   /* 强制滚到末尾 */
    if (role == CAGENT_ROLE_AI) g_stream_chars += total;  /* 供重试时回删本分段 */
    free(wbuf);
}
