/*
 * test/ui_gap_probe.c - 诊断: 回放 vs 实时 两条路径在同一会话上的排版差异
 *
 * 背景 (用户报告): 加载历史会话后, 输出区里出现大幅空白:
 *   ① "已载入历史会话" 旁白与第一条用户消息之间 ~90px 空白
 *   ② AI 正文与折叠的工具标题行之间 ~152px 空白
 *
 * 这个探针把两条路径都跑一遍, 按段落输出结构 (文本 / 段前距 / y 坐标):
 *   A. 实时路径: 按 agent.h / task.h 的真实发送序列逐条 emit (流式 delta 合成整段)
 *   B. 回放路径: sess_apply_selected 的那一套 (清空 + 旁白 + history_replay)
 * 两条路径面向同一个会话文件, 段落结构一对便知空白是谁引入的。
 *
 * 验证工具, 不进产品二进制。
 *
 * 用法: gcc -Wall -Isrc -o ui_gap_probe.exe test/ui_gap_probe.c \
 *          -lcomctl32 -lwinhttp -lcrypt32 -lshell32 -lole32 -luuid -lgdi32 -luser32
 *       ./ui_gap_probe.exe <session.json>
 */

#define PROBE_MAIN
#include "../src/main_gui.c"

/* ---- 与产品一致的追加入口 (预览程序没有消息循环, 同步直调 do_append) ---- */
static void do_append_wrapper(const char *utf8, int role) { do_append(utf8, role); }

/* 打开 DPI 感知 (与 ui_preview.c 同一份): 否则 GDI 按 96 DPI 虚拟化, 量出来的坐标都失真 */
static void enable_dpi_awareness(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
    PFN_SetCtx f = (PFN_SetCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (f) { f((HANDLE)-4); return; }          /* -4 = PER_MONITOR_AWARE_V2 */
    typedef BOOL (WINAPI *PFN_SetAw)(void);
    PFN_SetAw g = (PFN_SetAw)(void*)GetProcAddress(u, "SetProcessDPIAware");
    if (g) g();
}

/* ---- 32 位 DIB -> BMP (与 ui_preview.c 同一份实现) ---- */
static int save_bmp32(const char *path, int w, int h, const void *bits) {
    BITMAPINFOHEADER ih;
    memset(&ih, 0, sizeof(ih));
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h;
    ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    ih.biSizeImage = (DWORD)w * 4u * (DWORD)h;
    BITMAPFILEHEADER fh;
    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(&fh, 1, sizeof(fh), f);
    fwrite(&ih, 1, sizeof(ih), f);
    fwrite(bits, 1, ih.biSizeImage, f);
    fclose(f);
    return 1;
}

static void render_to_bmp(HWND h, const char *path, int w, int hh) {
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = hh;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits) { printf("CreateDIBSection failed\n"); return; }
    HDC hdcMem = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(hdcMem, hbm);
    HDC hdcSrc = GetDC(h);
    BOOL blt = hdcSrc && BitBlt(hdcMem, 0, 0, w, hh, hdcSrc, 0, 0, SRCCOPY);
    if (hdcSrc) ReleaseDC(h, hdcSrc);
    if (blt && save_bmp32(path, w, hh, bits))
        printf("%s (%dx%d, dpi=%u, %d logical px font)\n", path, w, hh, g_dpi, HIST_LPX_DEF);
    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    DeleteObject(hbm);
}

/* ---- 段落结构转储 ----
 * 逐段落输出: 段前距 (twips/逻辑像素), 首字符 y 坐标, 文本预览 (空段落标 <EMPTY>)。
 * y 来自 EM_POSFROMCHAR —— 控件客户区坐标, 段落之间的"视觉空白"直接由相邻 y 差看出。 */
static void dump_paras(const char *tag, int limit) {
    /* 先回到文档顶部: do_append 每步都滚到底, 不重置的话 POSFROMCHAR 全是负坐标 */
    SendMessageW(g_hHistory, EM_SETSEL, 0, 0);
    SendMessageW(g_hHistory, EM_SCROLLCARET, 0, 0);
    SendMessageW(g_hHistory, WM_VSCROLL, SB_TOP, 0);

    long n = ui_text_cp_len(g_hHistory);
    if (n <= 0) { printf("\n==== %s ====\n(empty)\n", tag); return; }
    WCHAR *buf = ui_get_range(g_hHistory, 0, n);
    if (!buf) { printf("read failed\n"); return; }
    long got = (long)lstrlenW(buf);

    long line_h = dp(g_hist_lpx + HIST_LINE_EXTRA);
    printf("\n==== %s ====\n", tag);
    printf("doc cp=%ld, line_h=%ldpx(@%udpi)\n", n, line_h, g_dpi);

    long cp = 0;
    int idx = 0, empties = 0, total = 0;
    for (long i = 0; i <= got; i++) {
        if (i < got && buf[i] != L'\r') continue;
        /* 段落 [cp, i): EM_GETTEXTRANGE 口径下段落标记是单个 CR, 段界无歧义 */
        {
            PARAFORMAT2 pf;
            memset(&pf, 0, sizeof(pf));
            pf.cbSize = sizeof(pf);
            SendMessageW(g_hHistory, EM_SETSEL, cp, cp);
            SendMessageW(g_hHistory, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
            POINTL p;
            memset(&p, 0, sizeof(p));
            SendMessageW(g_hHistory, EM_POSFROMCHAR, (WPARAM)&p, (LPARAM)cp);

            int is_empty = (i == cp);
            if (is_empty) empties++;
            total++;
            if (limit <= 0 || idx < limit) {
                char prev[96];
                int w = 0;
                for (long k = cp; k < i && w < 70; k++) {
                    WCHAR c = buf[k];
                    if (c == L'\r') { prev[w++] = '\\'; prev[w++] = 'r'; }
                    else if (c == L'\n') { prev[w++] = '\\'; prev[w++] = 'n'; }
                    else if (c >= 32 && c < 127) prev[w++] = (char)c;
                    else if (c < 128) prev[w++] = '?';
                    else prev[w++] = '#';
                }
                prev[w] = '\0';
                printf("  [%3d] y=%5ld spb=%4ld(%4.0fpx) spa=%3ld ind=%3ld %s%s\n",
                       idx, (long)p.y,
                       (long)pf.dySpaceBefore,
                       pf.dySpaceBefore / 1440.0 * g_dpi,
                       (long)pf.dySpaceAfter, (long)pf.dxStartIndent,
                       is_empty ? "<EMPTY> " : "", prev);
            }
            idx++;
        }
        cp = i + 1;
    }
    printf("  --> %d paras total, %d empty\n", total, empties);
    free(buf);
}

/* ---- 路径 A: 实时序列 ----
 * 复刻 agent.h / task.h 对同几条消息的真实发送顺序:
 *   用户回显 "%s\r\n" (task.h; 前导换行已去掉, 分隔由 g_last_no_lf 兜底) ->
 *   AI 流式 delta (net.h) ->
 *   有 tool_calls 时: content 已带换行就不补 (agent.h:771) ->
 *   "▾ 调用工具 X\r\n" + 参数行 -> 输出 (已带换行就不补 \r\n, agent.h:914) */
static void live_first(const char *prev_ai, const char *user_msg,
                       const char *ai_content, const char *tool_name,
                       const char *args, const char *output) {
    /* 上一轮 AI 的收尾回复 (无尾随换行) */
    do_append(prev_ai, CAGENT_ROLE_AI);
    /* 用户发送: task.h 的回显 */
    {
        size_t ul = strlen(user_msg);
        char *uecho = (char*)malloc(ul + 16);
        snprintf(uecho, ul + 16, "%s\r\n", user_msg);
        do_append(uecho, CAGENT_ROLE_USER);
        free(uecho);
    }
    /* AI 流式正文: 按一个 delta 送达 (文本内容与逐 delta 送达一致) */
    do_append(ai_content, CAGENT_ROLE_AI);
    /* agent.h:771 —— content 已带换行, 不补 */
    size_t cl = strlen(ai_content);
    if (cl > 0 && ai_content[cl - 1] != '\n' && ai_content[cl - 1] != '\r') {
        char *lf = (char*)malloc(8);
        snprintf(lf, 8, "\r\n");
        do_append(lf, CAGENT_ROLE_AI);
        free(lf);
    }
    /* 工具标题行 + 参数行 */
    {
        size_t cap = strlen(tool_name) + 64;
        char *line = (char*)malloc(cap);
        snprintf(line, cap, CAGENT_FOLD_GLYPH_OPEN " 调用工具 %s\r\n", tool_name);
        do_append(line, CAGENT_ROLE_TOOL);
        snprintf(line, cap, "%s\r\n", args);
        do_append(line, CAGENT_ROLE_ARGS);
        free(line);
    }
    /* 工具输出: agent.h:914 的 eol 判定 */
    {
        size_t tl = strlen(output);
        char *oline = (char*)malloc(tl + 8);
        if (tl > 0 && (output[tl - 1] == '\n' || output[tl - 1] == '\r'))
            snprintf(oline, tl + 8, "%s", output);
        else
            snprintf(oline, tl + 8, "%s\r\n", output);
        do_append(oline, CAGENT_ROLE_OUTPUT);
        free(oline);
    }
}

/* ---- 路径 A': 实时序列跑整个会话 (与 history_replay 同一份 messages) ----
 * 按 agent.h 的真实规则逐条发送, 得到"如果这段对话是实时聊出来的"排版结果。 */
static void live_full_from_messages(void) {
    size_t skip = strlen(g_system_prompt);
    const char *conv = messages + skip;
    if (*conv == ',') conv++;
    char *arr = (char*)malloc(strlen(conv) + 3);
    if (!arr) return;
    sprintf(arr, "[%s]", conv);
    JValue *root = json_parse(arr);
    free(arr);
    if (!root || root->type != J_ARR) { if (root) json_free(root); return; }

    for (size_t i = 0; i < root->arr.n; i++) {
        const JValue *m = root->arr.items[i];
        if (m->type != J_OBJ) continue;
        const JValue *role = json_obj_get(m, "role");
        const JValue *c = json_obj_get(m, "content");
        const char *r  = (role && role->type == J_STR) ? role->str : "";
        const char *ct = (c && c->type == J_STR) ? c->str : "";
        if (strcmp(r, "user") == 0) {
            if (!*ct) continue;
            size_t ul = strlen(ct);
            char *ue = (char*)malloc(ul + 16);
            snprintf(ue, ul + 16, "%s\r\n", ct);
            do_append(ue, CAGENT_ROLE_USER);
            free(ue);
        } else if (strcmp(r, "assistant") == 0) {
            if (*ct) {
                do_append(ct, CAGENT_ROLE_AI);
                size_t cl = strlen(ct);
                if (ct[cl - 1] != '\n' && ct[cl - 1] != '\r') do_append("\r\n", CAGENT_ROLE_AI);
            }
            const JValue *tcs = json_obj_get(m, "tool_calls");
            if (tcs && tcs->type == J_ARR) {
                for (size_t k = 0; k < tcs->arr.n; k++) {
                    const JValue *fn = json_obj_get(json_arr_at(tcs, k), "function");
                    if (!fn) continue;
                    const char *nm = json_as_str(json_obj_get(fn, "name"));
                    const char *ag = json_as_str(json_obj_get(fn, "arguments"));
                    if (!nm || !*nm) continue;
                    size_t al = ag ? strlen(ag) : 0;
                    size_t cap = strlen(nm) + (al > ARGS_DISPLAY_MAX ? ARGS_DISPLAY_MAX : al) + 64;
                    char *line = (char*)malloc(cap);
                    if (!line) continue;
                    snprintf(line, cap, CAGENT_FOLD_GLYPH_OPEN " 调用工具 %s\r\n", nm);
                    do_append(line, CAGENT_ROLE_TOOL);
                    if (al > ARGS_DISPLAY_MAX) {
                        char *prev = (char*)malloc(cap);
                        if (prev) {
                            utf8_safe_copy(ag, ARGS_DISPLAY_MAX, prev, cap);
                            snprintf(line, cap, "%s\xE2\x80\xA6\r\n", prev);   /* … */
                            free(prev);
                        } else {
                            snprintf(line, cap, "(\xe5\x8f\x82\xe6\x95\xb0\xe8\xbf\x87\xe9\x95\xbf, \xe6\x9c\xaa\xe6\x98\xbe\xe7\xa4\xba)\r\n");
                        }
                    } else {
                        snprintf(line, cap, "%s\r\n", ag);
                    }
                    do_append(line, CAGENT_ROLE_ARGS);
                    free(line);
                }
            }
        } else if (strcmp(r, "tool") == 0) {
            if (!*ct) continue;
            /* 实时路径先剥掉尾部说明 (tool_note_split), 再按 eol 决定补不补 \r\n */
            const char *note = tool_note_split(ct);
            size_t shown = note ? (size_t)(note - ct) : strlen(ct);
            if (shown == 0) { do_append("(\xe6\x97\xa0\xe8\xbe\x93\xe5\x87\xba)\r\n", CAGENT_ROLE_OUTPUT); continue; }
            char *oline = (char*)malloc(shown + 8);
            if (ct[shown - 1] == '\n' || ct[shown - 1] == '\r')
                snprintf(oline, shown + 8, "%.*s", (int)shown, ct);
            else
                snprintf(oline, shown + 8, "%.*s\r\n", (int)shown, ct);
            do_append(oline, CAGENT_ROLE_OUTPUT);
            free(oline);
        }
    }
    json_free(root);
}

/* 锚点滚动: 找到指定文本, 把光标放到那里再滚过去 (EM_FINDTEXTEX + EM_SCROLLCARET)。
 * 两条路径内容若真的一致, 锚点行会分别贴在各自视口顶部, 渲染结果逐像素相同。 */
static void scroll_to_anchor(const WCHAR *needle) {
    FINDTEXTEXW ft;
    memset(&ft, 0, sizeof(ft));
    ft.chrg.cpMin = 0;
    ft.chrg.cpMax = -1;
    ft.lpstrText = needle;
    long cp = (long)SendMessageW(g_hHistory, EM_FINDTEXTEXW, FR_DOWN, (LPARAM)&ft);
    if (cp < 0) { printf("anchor not found: %ls\n", needle); return; }
    /* 不用 EM_SCROLLCARET: 它只做"最小滚动", 落点取决于光标当前是否已部分可见,
     * 两条路径各滚各的会对不齐。EM_POSFROMCHAR 给的是**视口相对**坐标 (锚点在当前
     * 视图上方时为负), 叠上当前滚动量才是文档绝对 y, 再显式 SETSCROLLPOS 到同一相对位置。 */
    POINTL pt; memset(&pt, 0, sizeof(pt));
    SendMessageW(g_hHistory, EM_POSFROMCHAR, (WPARAM)&pt, (LPARAM)cp);
    POINT cur; memset(&cur, 0, sizeof(cur));
    SendMessageW(g_hHistory, EM_GETSCROLLPOS, 0, (LPARAM)&cur);
    long doc_y = (long)pt.y + cur.y;
    POINT sp;
    sp.x = 0;
    sp.y = doc_y > 120 ? doc_y - 120 : 0;
    SendMessageW(g_hHistory, EM_SETSCROLLPOS, 0, (LPARAM)&sp);
    memset(&sp, 0, sizeof(sp));
    SendMessageW(g_hHistory, EM_GETSCROLLPOS, 0, (LPARAM)&sp);
    printf("anchor cp=%ld doc_y=%ld -> scroll y=%ld\n", cp, doc_y, (long)sp.y);
}

/* 每次开新段落前: 复刻 sess_apply_selected 对历史框的重置 */
static void hist_reset(void) {
    SetWindowTextW(g_hHistory, L"");
    g_last_role = -1;
    g_last_no_lf = 0;
    ui_fold_reset();
    ui_history_parafmt(g_hHistory);
    ui_history_zoom_apply(g_hHistory);
}

int main(int argc, char **argv) {
    const char *session = argc > 1 ? argv[1] : "sessions/history_test.json";
    enable_dpi_awareness();
    LoadLibraryW(L"Msftedit.dll");
    dpi_init();
    g_dpi = dpi_system();

    int W = dp(720), H = dp(500);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"cagent_probe_wnd";
    if (!RegisterClassW(&wc)) { printf("RegisterClass failed\n"); return 1; }

    /* 必须真的显示出来: 未显示的窗口里 RichEdit 不做排版, BitBlt 也只能抓到全黑 */
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"cagent gap probe", WS_POPUP,
                              40, 40, W, H, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("CreateWindow failed\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    int status_h = dp(22);
    g_hHistory = CreateWindowExW(0, L"RICHEDIT50W", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, W, H - status_h, hwnd, (HMENU)(LONG_PTR)ID_HISTORY, NULL, NULL);
    if (!g_hHistory) { printf("CreateWindow RichEdit failed\n"); return 1; }
    g_hStatus = CreateWindowW(L"STATIC", L"ready", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_STATUS, NULL, NULL);
    MoveWindow(g_hStatus, dp(8), H - status_h, W - dp(16), status_h, TRUE);
    g_oldHistoryProc = (WNDPROC)SetWindowLongPtrW(g_hHistory, GWLP_WNDPROC, (LONG_PTR)HistoryProc);

    ui_fonts_rebuild(hwnd);
    SendMessageW(g_hHistory, EM_SETBKGNDCOLOR, 0, (LPARAM)(DWORD)HIST_BG);
    ui_history_parafmt(g_hHistory);
    ui_history_pad(g_hHistory, W, H - status_h);

    /* 先读会话文件, 取出探针要原样重放的几条消息原文 */
    char *raw = read_file_alloc(session, 0, 0, NULL, NULL);
    if (!raw) { printf("cannot read %s\n", session); return 1; }
    JValue *sj = json_parse(raw);
    if (!sj || sj->type != J_OBJ) { printf("bad session json\n"); return 1; }
    const JValue *msgs = json_obj_get(sj, "messages");
    if (!msgs || msgs->type != J_ARR) { printf("no messages\n"); return 1; }
    const JValue *m0 = json_arr_at(msgs, 0);
    const JValue *m1 = json_arr_at(msgs, 1);
    const JValue *m2 = json_arr_at(msgs, 2);
    const JValue *mlast = json_arr_at(msgs, msgs->arr.n - 1);
    const char *u0   = json_as_str(json_obj_get(m0, "content"));
    const char *a1   = json_as_str(json_obj_get(m1, "content"));
    const JValue *tc1 = json_obj_get(m1, "tool_calls");
    const char *tn1  = json_as_str(json_obj_get(json_obj_get(json_arr_at(tc1, 0), "function"), "name"));
    const char *ta1  = json_as_str(json_obj_get(json_obj_get(json_arr_at(tc1, 0), "function"), "arguments"));
    const char *o2   = json_as_str(json_obj_get(m2, "content"));
    const char *alast= json_as_str(json_obj_get(mlast, "content"));

    cagent_emit = do_append_wrapper;

    /* ===== 路径 A: 实时序列 (第一个屏幕的内容) ===== */
    hist_reset();
    live_first(alast ? alast : "", u0 ? u0 : "", a1 ? a1 : "",
               tn1 ? tn1 : "", ta1 ? ta1 : "", o2 ? o2 : "");
    dump_paras("LIVE (real-time path, first screen)", 200);
    render_to_bmp(g_hHistory, "probe_live.bmp", W, H - status_h);

    /* ===== 路径 B: 回放 (sess_apply_selected 原样) ===== */
    hist_reset();
    if (!history_load_from_file(session)) { printf("history_load_from_file failed\n"); return 1; }
    /* 注意: history_load_from_file 已把 messages 重建成 "system + ,{...}", 与产品一致 */
    emit_note("(loaded history, workspace: ");
    emit_note(g_workspace);
    emit_note(")\r\n");
    history_replay();
    dump_paras("REPLAY (history_replay, first screen)", 14);
    render_to_bmp(g_hHistory, "probe_replay.bmp", W, H - status_h);

    /* ===== 路径 A': 实时序列跑完整会话 ===== */
    hist_reset();
    live_full_from_messages();
    dump_paras("LIVE FULL (whole session, live rules)", 0);
    /* 滚到底再渲染: 收尾的那块 (最后一个工具块的完整输出 + 最终回复) 是最能看出
     * "回放有没有比实时少内容" 的一屏, 两条路径滚到底应当逐像素一致。 */
    {
        long len = ui_text_cp_len(g_hHistory);
        SendMessageW(g_hHistory, EM_SETSEL, len, len);
        SendMessageW(g_hHistory, EM_SCROLLCARET, 0, 0);
        SendMessageW(g_hHistory, WM_VSCROLL, SB_BOTTOM, 0);
    }
    render_to_bmp(g_hHistory, "probe_live_bottom.bmp", W, H - status_h);
    /* 内容锚点: 最终回复的首行。两条路径各自滚到同一段文字, 视口内容应逐像素相同 */
    scroll_to_anchor(L"\u76ee\u5f55\u7684\u6574\u4f53\u60c5\u51b5");
    render_to_bmp(g_hHistory, "probe_live_anchor.bmp", W, H - status_h);

    /* ===== 路径 B': 回放跑完整会话 ===== */
    hist_reset();
    if (!history_load_from_file(session)) return 1;
    emit_note("(loaded history)\r\n");
    history_replay();
    dump_paras("REPLAY FULL (whole session)", 0);
    {
        long len = ui_text_cp_len(g_hHistory);
        SendMessageW(g_hHistory, EM_SETSEL, len, len);
        SendMessageW(g_hHistory, EM_SCROLLCARET, 0, 0);
        SendMessageW(g_hHistory, WM_VSCROLL, SB_BOTTOM, 0);
    }
    render_to_bmp(g_hHistory, "probe_replay_bottom.bmp", W, H - status_h);
    scroll_to_anchor(L"\u76ee\u5f55\u7684\u6574\u4f53\u60c5\u51b5");
    render_to_bmp(g_hHistory, "probe_replay_anchor.bmp", W, H - status_h);

    free(raw);
    json_free(sj);
    return 0;
}
