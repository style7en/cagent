/*
 * test/ui_preview.c - 输出区排版与折叠的离屏预览 (make preview)
 *
 * 为什么需要它: 排版改动(角色配色 / 缩进 / 段间距)和折叠交互在代码里看不出好坏, 而界面
 * 又没法像核心逻辑那样写断言。这里把输出区控件建在一个临时窗口里, 灌入一段覆盖全部角色
 * 的示例对话, 渲染到内存 DIB 存成 BMP —— 人眼一看就知道对不对; 折叠的正确性则用
 * "收起再展开、全文逐字比对"来判定 (见 fold_selftest)。
 *
 * 它是**验证工具, 不进产品二进制**: 和 test/ 下其它东西一样独立编译。
 *
 * 直接 include 整个 main_gui.c, 而不是自己拼一遍 UI 模块的 include 链 —— 这样预览跑的
 * 就是产品代码本身 (字体、行距、角色样式、折叠逻辑的取值不可能和真实界面跑偏)。
 * 入口换成下面的 main(): 产品的 WinMain 在 ui/main.h 里, 名字不同不冲突, 只是不会被调用,
 * 由此产生的 unused-function 告警在 Makefile 的 preview 目标里显式关掉。
 *
 * 渲染是 1:1 的 (DIB 尺寸 = 控件物理像素), 所以任何 DPI 下看到的都是真实像素。
 *
 * 用法: make preview   -> 生成 preview_out.bmp (展开态) 与 preview_folded.bmp (收起态)
 */

#include <windows.h>   /* 下面的 PostMessage 拦截要先于 main_gui.c (那头也有, 有 guard) */

/* ===== 保序拦截 PostMessageW (回放自检专用) =====
 * 生产的输出投递**全是 PostMessage 异步追加** (ui/hooks.h: gui_emit / gui_turn_end),
 * 回放自检要的就是这个消息泵的 FIFO 语义。这里把投递只**记进队列、不执行**, 由
 * pq_pump (见 main_gui.c 之后) 按入队顺序逐条处理, 动作与 wndproc.h 的两条分支
 * 一致; 队列满则转给真身。
 *
 * 拦截器本身**不引用任何产品符号** (g_hHistory / do_append 都在 main_gui.c 里),
 * hwnd 与消息类的过滤推迟到 pq_pump —— 否则定义点会早于声明点。
 *
 * 为什么必须保真: 旧自检把 cagent_emit 换成同步直调 do_append 的桩, 于是"回放之后
 * 直接调 ui_turn_tail"这种异步 ordering bug 在测试里永远测不出来 —— d46e7f3 正是
 * 这么漏的 (真机上最后一个工具块照旧展开, 而自检全绿)。 */
#define PQ_MAX 512
static struct { HWND h; UINT msg; WPARAM wp; char *txt; } g_pq[PQ_MAX];
static int g_pq_n;
static BOOL (WINAPI *g_real_post)(HWND, UINT, WPARAM, LPARAM) = PostMessageW;
#define PostMessageW MyPostMessageW
static BOOL WINAPI MyPostMessageW(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_pq_n < PQ_MAX) {
        g_pq[g_pq_n].h = h; g_pq[g_pq_n].msg = m;
        g_pq[g_pq_n].wp = w; g_pq[g_pq_n].txt = (char*)l;
        g_pq_n++;
        return TRUE;
    }
    return g_real_post(h, m, w, l);
}

#include "../src/main_gui.c"

/* 消息泵: 按 FIFO 处理队列里投给 g_hHistory 的消息, 动作与 wndproc.h 一致
 * (APPEND -> do_append 后释放字符串; FOLDEND -> ui_turn_tail)。别处的消息
 * (如状态栏) 与本测试无关; WM_APP_STREAM 在预览里本来就没有消息循环处理, 丢弃。 */
static void pq_pump(void) {
    int n = g_pq_n;              /* 截断: 泵过程中新投递的不再本轮处理 */
    for (int i = 0; i < n; i++) {
        if (g_pq[i].h != g_hHistory) continue;
        if (g_pq[i].msg == WM_APP_APPEND) {
            do_append(g_pq[i].txt, (int)g_pq[i].wp);
            free(g_pq[i].txt);
        } else if (g_pq[i].msg == WM_APP_FOLDEND) {
            ui_turn_tail();
        }
    }
    g_pq_n = 0;
}

/* 自检失败计数: main 末尾据此给非零退出码, make preview 才能当门禁用。
 * 教训来自 d46e7f3 —— "自检全绿但生产是坏的"最难防, 退出码是最低限度的兜底。 */
static int g_pv_fails;

/* 示例对话: 覆盖全部角色, 一张图就能看出配色、缩进、留白是否成体系。
 * 文案贴近真实运行时的样子 (含思考块、工具调用的两行结构、拒绝执行、错误提示),
 * 末尾附一段正式答案的 Markdown 渲染 (行级/行内标记、围栏块 —— 只有 AI 角色参与解析)。 */
static const struct { const char *text; int role; } kSample[] = {
    { "\r\n分析当前项目\r\n",                                          CAGENT_ROLE_USER },
    { "思考中…\r\n",                                                  CAGENT_ROLE_NOTE },
    { CAGENT_THINK_TITLE "\r\n",                                      CAGENT_ROLE_THINK_HEAD },
    { "用户想让我分析项目结构。先看 Makefile 弄清构建方式, 再看 src 下的模块划分。\r\n", CAGENT_ROLE_THINK },
    { "这个目录里还有 .superpowers 和 .workbuddy, 像是工具生成的, 先跳过。\r\n",        CAGENT_ROLE_THINK },
    { "项目里有 5 个顶层条目, 核心代码在 src/ 下, 分 core 与 ui 两层。\r\n",           CAGENT_ROLE_AI },
    { CAGENT_FOLD_GLYPH_OPEN " read_file\r\n",                        CAGENT_ROLE_TOOL },
    { "{\"path\": \"Makefile\"}\r\n",                                 CAGENT_ROLE_ARGS },
    { "CC = gcc\r\nCFLAGS = -Wall -Wextra\r\n",                       CAGENT_ROLE_OUTPUT },
    { CAGENT_FOLD_GLYPH_OPEN " read_file\r\n",                        CAGENT_ROLE_TOOL },
    { "{\"path\": \"src/ui/state.h\"}\r\n",                           CAGENT_ROLE_ARGS },
    { "typedef struct { COLORREF fg; } UiRoleStyle;\r\n",             CAGENT_ROLE_OUTPUT },
    { CAGENT_FOLD_GLYPH_OPEN " execute_bash\r\n",                     CAGENT_ROLE_TOOL },
    { "{\"command\": \"git diff --stat | tail -25\"}\r\n",            CAGENT_ROLE_ARGS },
    { "(请求限流, 等待后重试...)\r\n",                                 CAGENT_ROLE_NOTE },
    { "(请求限流, 等待后重试...)\r\n",                                 CAGENT_ROLE_NOTE },
    { "(请求限流, 等待后重试...)\r\n",                                 CAGENT_ROLE_NOTE },
    { ".superpowers\r\n.workbuddy\r\nsrc\r\ntest\r\n",                CAGENT_ROLE_OUTPUT },
    { "× edit_file\r\n",                                              CAGENT_ROLE_ERR },
    { "未执行: arguments 超过 8KB 上限\r\n",                           CAGENT_ROLE_ERR },
    { "(请求限流, 等待后重试...)\r\n",                                 CAGENT_ROLE_NOTE },
    /* ---- Markdown 渲染: 只有正式答案 (AI) 参与解析, 旁白/工具调用不参与 ---- */
    { "## 构建方式\r\n",                                            CAGENT_ROLE_AI },
    /* 这些 "\r\n" 空行条目在界面上**什么都看不见** —— 空行是 CommonMark 的块分隔符,
     * 不落地 (曾按空段落落地, 每个空行吃满"段前距 16 + 整行行高", 一片一片的白) */
    { "\r\n",                                                      CAGENT_ROLE_AI },
    /* 两行一段、中间无空行: 第二行是软换行续行, 段前距 0, 与第一行贴紧 */
    { "项目用 **MinGW-w64 + make** 构建, *单编译单元*, 零第三方依赖,\r\n产物是单个 cagent.exe, exe 所在目录即数据根。\r\n", CAGENT_ROLE_AI },
    { "关键命令是 `make` 与 `make test`, 见 [仓库说明](https://example.com)。\r\n", CAGENT_ROLE_AI },
    { "字面星号写作 \\*esc\\*, 不当斜体; ``a`b`` 反引号按 CommonMark 配对。\r\n", CAGENT_ROLE_AI },
    { "\r\n",                                                      CAGENT_ROLE_AI },
    { "> 引用: 输出区底色 RGB(247,247,247) 上, 标记灰不得浅于 110。\r\n", CAGENT_ROLE_AI },
    { "1. 折叠靠删文本/插回模拟\r\n2. 位置一律用 cp\r\n",              CAGENT_ROLE_AI },
    { "\r\n",                                                      CAGENT_ROLE_AI },
    { "---\r\n",                                                   CAGENT_ROLE_AI },
    { "```make\r\nall: cagent.exe\r\n\t$(MAKE) -C src test\r\n```\r\n", CAGENT_ROLE_AI },
    { "\r\n**粗体套 `代码`** 与 ***粗斜*** 也要分清。\r\n",           CAGENT_ROLE_AI },
};

/* 32 位 DIB -> BMP 文件。DIB section 用**正高度**(自下而上), 与 BMP 文件的内存布局
 * 一致, 所以直接把像素写进文件即可, 不用翻转行序。 */
static int save_bmp32(const char *path, int w, int h, const void *bits) {
    BITMAPINFOHEADER ih;
    memset(&ih, 0, sizeof(ih));
    ih.biSize        = sizeof(ih);
    ih.biWidth       = w;
    ih.biHeight      = h;              /* 正数: 自下而上 */
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage   = (DWORD)w * 4u * (DWORD)h;

    BITMAPFILEHEADER fh;
    memset(&fh, 0, sizeof(fh));
    fh.bfType    = 0x4D42;             /* "BM" */
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize    = fh.bfOffBits + ih.biSizeImage;

    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bits, ih.biSizeImage, 1, f);
    fclose(f);
    return 1;
}

/* 抓控件当前像素存成 BMP。
 * 走 BitBlt 而不是 PrintWindow / WM_PRINT: RichEdit 对后两者不予响应(实测直接返回失败),
 * 而它已经真实显示在屏幕上, 直接拷窗口 DC 的像素最省事也最保真。 */
static int render_to_bmp(HWND h, const char *path, int w, int hh) {
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = hh;   /* 正高度: 内存首行 = 图像末行, 与 BMP 布局一致 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits) { printf("CreateDIBSection failed\n"); return 0; }
    HDC hdcMem = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(hdcMem, hbm);

    HDC hdcSrc = GetDC(h);
    BOOL blt = hdcSrc && BitBlt(hdcMem, 0, 0, w, hh, hdcSrc, 0, 0, SRCCOPY);
    if (hdcSrc) ReleaseDC(h, hdcSrc);

    int ok = 0;
    if (!blt) {
        printf("BitBlt failed\n");
    } else if (save_bmp32(path, w, hh, bits)) {
        printf("%s %dx%d (dpi=%u, 字号=%d 逻辑像素)\n", path, w, hh, g_dpi, HIST_LPX_DEF);
        ok = 1;
    } else {
        printf("save %s failed\n", path);
    }

    SelectObject(hdcMem, old);
    DeleteDC(hdcMem);
    DeleteObject(hbm);
    return ok;
}

/* 读全文 (供折叠自检逐字比对)。容量按 cp 数留一倍余量 —— WM_GETTEXT 会把每个段落标记
 * 展开成 CRLF, 实际写出的字符数可能接近 cp 数的两倍。 */
static WCHAR *dump_all(HWND h, long *out_n) {
    long n = ui_text_cp_len(h);
    if (n <= 0) return NULL;
    long cap = n * 2 + 4;
    WCHAR *buf = (WCHAR*)malloc((size_t)cap * sizeof(WCHAR));
    if (!buf) return NULL;
    int got = GetWindowTextW(h, buf, (int)cap);
    if (got < 0) { free(buf); return NULL; }
    buf[got] = L'\0';
    if (out_n) *out_n = got;
    return buf;
}

/* 折叠自检: 对**每一个块**收起再展开, 全文必须逐字还原。
 * 这是位置索引表是否正确唯一的硬证据 —— 收起时删掉多少、展开时插回多少、后续块的 cp
 * 有没有跟着平移、多段正文(工具块 = 参数行 + 输出块)重放时样式段对不对, 任何一处算错
 * 都会在比对里露出来。逐个测还能顺带覆盖"折叠前面的块会让后面的块位置平移"。 */
static void fold_selftest(void) {
    if (g_fold_n <= 0) { printf("fold selftest: 跳过 (没有可折叠块)\n"); return; }
    long n0 = 0;
    WCHAR *before = dump_all(g_hHistory, &n0);
    if (!before) { printf("fold selftest: 读取全文失败\n"); return; }

    int blocks = g_fold_n, all_ok = 1;
    for (int i = 0; i < blocks; i++) {
        const char *kind = (g_folds[i].kind == UI_FOLD_TOOL) ? "tool" : "think";
        long n1 = 0, n2 = 0;

        ui_fold_toggle(i);                       /* 收起 */
        WCHAR *folded = dump_all(g_hHistory, &n1);
        WCHAR head[2] = {0, 0};
        TEXTRANGEW tr;
        tr.chrg.cpMin = g_folds[i].cp_head;
        tr.chrg.cpMax = g_folds[i].cp_head + 1;
        tr.lpstrText = head;
        SendMessageW(g_hHistory, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
        int ok_fold = (folded && n1 > 0 && n1 < n0 && head[0] == L'▸');

        ui_fold_toggle(i);                       /* 展开回去 */
        WCHAR *after = dump_all(g_hHistory, &n2);
        int ok_back = (after && n2 == n0 && wcscmp(after, before) == 0);

        if (!ok_fold || !ok_back) g_pv_fails++;
        printf("  block %d (%s): 收起 %ld->%ld 字符 [%s], 展开还原 [%s]\n",
               i, kind, n0, n1, ok_fold ? "OK" : "FAIL", ok_back ? "OK" : "FAIL");
        all_ok &= ok_fold && ok_back;
        free(folded); free(after);
    }
    if (!all_ok) g_pv_fails++;
    printf("fold selftest: %d 个块 [%s]\n", blocks, all_ok ? "OK" : "FAIL");
    free(before);
}

/* 打开 DPI 感知, 否则 GDI 按 96 DPI 虚拟化, 抓出来的字比真实界面小一半。
 * 产品是把感知声明写在 res/app.manifest 里由系统在建进程前应用; 这里没有清单,
 * 只能运行时调 —— 越早越好, 必须在建任何窗口之前。 */
static void enable_dpi_awareness(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
    PFN_SetCtx f = (PFN_SetCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (f) { f((HANDLE)-4); return; }          /* -4 = PER_MONITOR_AWARE_V2 */
    typedef BOOL (WINAPI *PFN_SetAw)(void);
    PFN_SetAw g = (PFN_SetAw)(void*)GetProcAddress(u, "SetProcessDPIAware");
    if (g) g();                                 /* 老系统回退 (Win8.1 及更早) */
}

/* ---- 模拟一次推理模型的响应, 检查 core 侧的思考块时序 ----
 * 真实 API 要有 key 才能打, 而这里要验的恰恰是"没 key 也能验"的那部分: 标题行只发一次、
 * 思考正文按序上屏、正式回答之前思考块必须已经闭合。 */
static char g_cap_buf[4096];
static int  g_cap_len;
static void cap_emit(const char *utf8, int role) {
    int left = (int)sizeof(g_cap_buf) - g_cap_len - 1;
    if (left <= 8) return;
    int n = snprintf(g_cap_buf + g_cap_len, (size_t)left, "[%d]%s", role, utf8);
    if (n > 0) g_cap_len += (n < left ? n : left - 1);
}

static void reasoning_flow_selftest(void) {
    void (*saved)(const char*, int) = cagent_emit;
    cagent_emit = cap_emit;
    g_cap_len = 0;
    g_cap_buf[0] = '\0';

    StreamCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    on_reasoning_delta(&ctx, "先看目录");
    on_reasoning_delta(&ctx, "，再读 Makefile。");
    on_content_delta(&ctx, "项目结构如下");

    cagent_emit = saved;

    int titles = 0;
    for (const char *p = g_cap_buf; (p = strstr(p, CAGENT_THINK_TITLE)) != NULL; p++) titles++;
    /* 标题只发一次 / 正文按序拼上 / 正式回答前思考块已闭合 (reasoning_open 归零) */
    int ok = (titles == 1) && strstr(g_cap_buf, "先看目录") && strstr(g_cap_buf, "再读 Makefile。")
             && strstr(g_cap_buf, "项目结构如下") && !ctx.reasoning_open
             && ctx.reasoning_len == strlen("先看目录") + strlen("，再读 Makefile。");
    if (!ok) g_pv_fails++;
    printf("reasoning flow: 标题 %d 次 [%s], reasoning_len=%zu\n",
           titles, ok ? "OK" : "FAIL", ctx.reasoning_len);
}

/* 读 cp 处一个字符的字符格式。 */
static void read_fmt(long cp, CHARFORMAT2W *cf) {
    memset(cf, 0, sizeof(*cf));
    cf->cbSize = sizeof(*cf);
    SendMessageW(g_hHistory, EM_SETSEL, cp, cp + 1);
    SendMessageW(g_hHistory, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)cf);
}

/* 折叠再展开后, **输出段的字符格式必须原样恢复**。
 * 只比对文本内容是不够的: 参数行和输出块都是等宽字体, 但一个没底色、一个有暖灰底,
 * 分段重放时若把两段刷成同一个角色, 文本比对照样通过而界面已经错了。 */
static void style_selftest(void) {
    int ti = -1;
    for (int i = 0; i < g_fold_n; i++) if (g_folds[i].kind == UI_FOLD_TOOL) ti = i;
    if (ti < 0) { printf("style selftest: 跳过 (没有工具块)\n"); return; }
    UiFoldBlock *b = &g_folds[ti];

    long pos = b->cp_head + b->head_cp, probe = -1;
    for (int s = 0; s < b->n_segs; s++) {
        if (b->segs[s].role == CAGENT_ROLE_OUTPUT) { probe = pos; break; }
        pos += b->segs[s].len;
    }
    if (probe < 0) { printf("style selftest: 跳过 (工具块没有输出段)\n"); return; }

    CHARFORMAT2W f1, f2;
    read_fmt(probe, &f1);
    ui_fold_toggle(ti);            /* 收起 */
    ui_fold_toggle(ti);            /* 再展开 */
    read_fmt(probe, &f2);

    int ok = (f1.crTextColor == f2.crTextColor) &&
             (f1.crBackColor == f2.crBackColor) &&
             (wcscmp(f1.szFaceName, f2.szFaceName) == 0) &&
             ((f1.dwEffects & (CFE_BOLD | CFE_ITALIC)) == (f2.dwEffects & (CFE_BOLD | CFE_ITALIC)));
    if (!ok) g_pv_fails++;
    printf("style selftest: 输出段 字色 %06lX->%06lX 底色 %06lX->%06lX 字体 %ls [%s]\n",
           (unsigned long)f1.crTextColor, (unsigned long)f2.crTextColor,
           (unsigned long)f1.crBackColor, (unsigned long)f2.crBackColor,
           f2.szFaceName, ok ? "OK" : "FAIL");
}

/* 每个块的标题行在折叠/展开前后, **段落缩进必须始终是该角色的值**。
 * 标题行不该有缩进 (TOOL / THINK_HEAD 的 indent=0); 参数行与输出块才缩进 16。
 * 标题行被缩进 = 它的段落格式被改成了别的角色 —— 而 ui_insert_styled 是按"插入点所在段落"
 * 设段落格式的, 所以出现这个症状说明位置索引算偏了, 插到了标题行所在段落里面。 */
static void head_indent_selftest(void) {
    int bad = 0;
    for (int i = 0; i < g_fold_n; i++) {
        long ind_folded = -1, ind_open = -1;
        for (int pass = 0; pass < 2; pass++) {
            ui_fold_toggle(i);                         /* pass 0 收起, pass 1 展开 */
            PARAFORMAT2 pf;
            memset(&pf, 0, sizeof(pf));
            pf.cbSize = sizeof(pf);
            SendMessageW(g_hHistory, EM_SETSEL, g_folds[i].cp_head, g_folds[i].cp_head);
            SendMessageW(g_hHistory, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
            if (pass == 0) ind_folded = (long)pf.dxStartIndent;
            else           ind_open   = (long)pf.dxStartIndent;
        }
        int ok = (ind_folded == 0 && ind_open == 0);
        if (!ok) bad++;
        printf("  block %d (%s) 标题行缩进: 收起=%ld 展开=%ld twips %s\n",
               i, g_folds[i].kind == UI_FOLD_TOOL ? "tool" : "think",
               ind_folded, ind_open, ok ? "[OK]" : "[FAIL]");
    }
    if (bad) g_pv_fails++;
    printf("head indent selftest: %s\n", bad ? "FAIL" : "OK");
}

/* 滚动之后 EM_POSFROMCHAR 必须返回**客户区坐标** (含滚动偏移) —— 否则对话一长、控件一滚,
 * 点击位置和标题行坐标就对不上, 表现正是"点了没反应 / 要试好几次才中"。
 * 判据分两层: ① 坐标随滚动变化 (说明不是文档坐标); ② 滚动后命中测试仍然准。 */
static void scroll_hit_selftest(void) {
    if (g_fold_n <= 0) { printf("scroll hit selftest: 跳过 (没有块)\n"); return; }
    /* 先撑长文档, 保证滚得动 (普通正文不会新建折叠块) */
    for (int i = 0; i < 40; i++) do_append("填充行, 用来把文档撑到可滚动。\r\n", CAGENT_ROLE_AI);

    /* do_append 每次都把视图滚到底, 所以要先回到顶部, 往下才有得滚 */
    SendMessageW(g_hHistory, WM_VSCROLL, SB_TOP, 0);
    POINTL p0, p1;
    memset(&p0, 0, sizeof(p0));
    SendMessageW(g_hHistory, EM_POSFROMCHAR, (WPARAM)&p0, (LPARAM)g_folds[0].cp_head);
    SendMessageW(g_hHistory, EM_LINESCROLL, 0, 2);
    memset(&p1, 0, sizeof(p1));
    SendMessageW(g_hHistory, EM_POSFROMCHAR, (WPARAM)&p1, (LPARAM)g_folds[0].cp_head);

    if (p0.y == p1.y) g_pv_fails++;
    printf("scroll hit: 滚 2 行, 标题行 y %ld -> %ld (坐标随滚动%s)\n",
           (long)p0.y, (long)p1.y, p0.y != p1.y ? "变化 [OK]" : "不变 [FAIL]");
    int hit = ui_fold_hit(p1);
    if (hit != 0) g_pv_fails++;
    printf("  滚动后命中 (%ld,%ld) -> 块 %d (期望 0) [%s]\n",
           (long)p1.x, (long)p1.y, hit, hit == 0 ? "OK" : "FAIL");
    SendMessageW(g_hHistory, WM_VSCROLL, SB_BOTTOM, 0);
}

/* 点击折叠/展开时视图不该跳: 切换前后滚动位置必须一致。
 * 用户的视线落在刚点的那一行上, 内容一伸缩 RichEdit 会自己挪滚动条, 于是"点一下整个
 * 视图都滚了"。要先把文档撑到可滚并滚到一个"不在顶也不在底"的位置 —— 文档不够长时
 * 滚动位置恒为 0, 这个检查会假通过。 */
static void scroll_anchor_selftest(void) {
    if (g_fold_n <= 0) { printf("scroll anchor selftest: 跳过 (没有块)\n"); return; }
    for (int i = 0; i < 40; i++) do_append("填充行, 用来把文档撑到可滚动。\r\n", CAGENT_ROLE_AI);
    SendMessageW(g_hHistory, WM_VSCROLL, SB_TOP, 0);
    SendMessageW(g_hHistory, EM_LINESCROLL, 0, 5);

    RECT rc;
    GetClientRect(g_hHistory, &rc);
    int tested = 0, bad = 0;
    for (int i = 0; i < g_fold_n; i++) {
        POINTL p;
        memset(&p, 0, sizeof(p));
        SendMessageW(g_hHistory, EM_POSFROMCHAR, (WPARAM)&p, (LPARAM)g_folds[i].cp_head);
        if (p.y < 0 || p.y >= rc.bottom) continue;    /* 不在可视区, 跳过 */
        POINT s0, s1;
        memset(&s0, 0, sizeof(s0));
        memset(&s1, 0, sizeof(s1));
        SendMessageW(g_hHistory, EM_GETSCROLLPOS, 0, (LPARAM)&s0);
        ui_fold_toggle(i);
        SendMessageW(g_hHistory, EM_GETSCROLLPOS, 0, (LPARAM)&s1);
        ui_fold_toggle(i);                            /* 还原 */
        tested++;
        if (s0.y != s1.y) {
            printf("  块 %d 折叠时视图跳动: 滚动 y %ld -> %ld\n", i, (long)s0.y, (long)s1.y);
            bad++;
        }
    }
    if (bad) g_pv_fails++;
    printf("scroll anchor selftest: 测了 %d 个块 %s\n", tested, bad ? "[FAIL]" : "[OK]");
}

/* 逐行打印段落缩进与首字符 —— 定位"格式与内容错位"用。
 * 首字符只打码点: 中文用 %ls 打印在这个控制台里不可靠。 */
static void dump_line_indents(void) {
    int lines = (int)SendMessageW(g_hHistory, EM_GETLINECOUNT, 0, 0);
    printf("逐行缩进 (%d 行, twips / 首字符码点):\n", lines);
    for (int i = 0; i < lines; i++) {
        long cp = (long)SendMessageW(g_hHistory, EM_LINEINDEX, (WPARAM)i, 0);
        if (cp < 0) continue;
        PARAFORMAT2 pf;
        memset(&pf, 0, sizeof(pf));
        pf.cbSize = sizeof(pf);
        SendMessageW(g_hHistory, EM_SETSEL, cp, cp);
        SendMessageW(g_hHistory, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
        WCHAR ch[2] = {0, 0};
        TEXTRANGEW tr;
        tr.chrg.cpMin = cp;
        tr.chrg.cpMax = cp + 1;
        tr.lpstrText = ch;
        SendMessageW(g_hHistory, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
        printf("  %2d indent=%4ld 首字符=%04X\n", i, (long)pf.dxStartIndent, (unsigned)ch[0]);
    }
}

/* 历史回放自检: 载入含工具调用的会话后, 必须能重建出可折叠的工具块。
 * 以前 history_replay 只回放 tool 结果、不回放 assistant 的 tool_calls, 于是历史会话里
 * 只剩一堆没头没尾的输出块 —— 看不出是哪次调用, 也没有标题行可点。
 * 终态还必须和实时一致: 回放结束也要收尾 (gui_turn_end -> WM_APP_FOLDEND),
 * 最后一个块默认收起 —— 漏了它, "加载历史会话, 最后一个调用工具总是展开"。
 *
 * **驱动的是真实的 sess_apply_selected** (读文件 -> 清空 -> 回放 -> 收尾一条龙),
 * 全部投递经过上面拦截的 PostMessage 队列, 再 pq_pump 按 FIFO 泵一遍 —— 与生产的
 * 消息循环同构。d46e7f3 的教训: 收尾当时是 history_replay() 之后**直接调**
 * ui_turn_tail, 而回放内容全是 PostMessage 异步追加 (hooks.h 的 gui_emit) ——
 * 收尾赶在正文入队之前, 对着 g_fold_n==0 的空气栈收尾, 最后一个工具块照旧展开;
 * 而用同步直调桩的旧自检全绿。现在: 直接调 / 漏投递 / 投递顺序错, 任一复发都 FAIL。 */
static void replay_selftest(void) {
    /* 会话文件是自描述 JSON (workspace + messages), 见 core/session.h 的
     * load_messages_from_text; 末尾那条 assistant 让会话以"工具调用之后还有正文"
     * 收尾 —— 此时最后一个折叠块仍是工具块, 正是收尾要管的对象。 */
    const char *path = "preview_replay_session.json";
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs("{\"workspace\":\".\",\"messages\":["
              "{\"role\":\"user\",\"content\":\"看下项目结构\"}"
              ",{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"c1\","
              "\"type\":\"function\",\"function\":{\"name\":\"read_file\","
              "\"arguments\":\"{\\\"path\\\": \\\"Makefile\\\"}\"}}]}"
              ",{\"role\":\"tool\",\"content\":\"CC = gcc\\nCFLAGS = -Wall\\n\"}"
              ",{\"role\":\"assistant\",\"content\":\"Makefile 里用的是 gcc。\"}]}", f);
        fclose(f);
    }

    g_active_ws[0] = '\0';   /* 空工作区 -> sess_apply_selected 跳过 history_save */
    g_pq_n = 0;              /* 只数这一轮投递出来的消息 */
    cagent_emit = gui_emit;  /* 真实钩子: PostMessage 入队, 不是同步直调桩 */

    sess_apply_selected(path);
    pq_pump();               /* FIFO: 载入旁白 -> 回放正文 -> 轮次收尾 */

    remove(path);
    cagent_emit = NULL;

    int ok = (g_fold_n == 1) && g_folds[0].collapsed;
    if (!ok) g_pv_fails++;
    printf("replay selftest: 重建了 %d 个工具块 (期望 1, 默认收起) [%s]\n",
           g_fold_n, ok ? "OK" : "FAIL");
}

/* tool_note_split 的检查: 认得出尾部说明 (好把它从上屏内容里剥掉), 但**不能误认文件内容里
 * 的同样字样** (读一个恰好包含 "...(已截断" 的文件是完全可能的)。判据是"它必须是最后一行"。 */
static void note_split_selftest(void) {
    const char *tail  = "line1\nline2\n...(已截断: 文件共 100 字节, 本次返回第 1~50 字节)";
    const char *inner = "line1\n...(已截断: 这是文件内容里的一行)\nline3\n";
    const char *none  = "just a normal output\n";
    int ok = (tool_note_split(tail) != NULL)      /* 尾部说明: 认出来 */
          && (tool_note_split(inner) == NULL)     /* 内文里的: 后面还有内容, 不认 */
          && (tool_note_split(none) == NULL);
    if (!ok) g_pv_fails++;
    printf("note split selftest: [%s]\n", ok ? "OK" : "FAIL");
}

static const wchar_t *pv_wstrstr(const wchar_t *hay, const wchar_t *needle);  /* 定义在下面 */

/* 读回历史框全文 (调用方 free)。 */
static WCHAR *pv_doc_text(void) {
    int n = GetWindowTextLengthW(g_hHistory);
    WCHAR *w = (WCHAR*)calloc((size_t)n + 8, sizeof(WCHAR));
    if (w) GetWindowTextW(g_hHistory, w, n + 1);
    return w;
}

/* 折叠块索引表**满了之后, 最新的块必须仍然可折**。
 *
 * 真实场景: 一个 150 条消息、67 次工具调用的会话, 加载后**末尾几个块全都折不动** ——
 * 因为 ui_fold_new 在 g_fold_n >= UI_FOLD_MAX 时直接 return, 新块压根不进索引表。
 * 界面上标题行照常显示 (看着一切正常), 点上去却没反应, 而且是从某个长度开始**静默**
 * 失效 —— 会话越往后聊, 折不动的块越多, 用户看到的就是"最新的全都没折叠"。
 *
 * 所以断言不能只看"表里的块收起了没" (旧写法下表里那批照样全收起, 照样通过):
 * 必须证明**文档末尾那个块**在表里 —— 给它一个只会在这里出现的输出文本, 断言它被
 * 折掉了 (文档里搜不到), 再展开又能搜到。 */
static void fold_cap_selftest(void) {
    const char *path = "preview_foldcap_session.json";
    int nblk = UI_FOLD_MAX + 8;                 /* 必定溢出, 逼出"表满"这条路径 */
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs("{\"workspace\":\".\",\"messages\":[{\"role\":\"user\",\"content\":\"跑很多工具\"}", f);
        for (int i = 0; i < nblk; i++)
            fprintf(f, ",{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":[{\"id\":\"c%d\","
                       "\"type\":\"function\",\"function\":{\"name\":\"tool_%d\",\"arguments\":\"{}\"}}]}"
                       ",{\"role\":\"tool\",\"content\":\"out %d\"}", i, i, i);
        fputs(",{\"role\":\"assistant\",\"content\":\"done\"}]}", f);
        fclose(f);
    } else {
        /* 写不出临时文件就提前收场: 否则会走到"缺文件静默早退", 把 IO 故障误诊为
         * 折叠机制失效 (断言虽不崩溃, 报告却会指错方向)。 */
        g_pv_fails++;
        printf("fold cap selftest: 会话文件写不出 (%s) [FAIL]\n", path);
        return;
    }

    g_active_ws[0] = '\0';
    g_pq_n = 0;
    cagent_emit = gui_emit;
    sess_apply_selected(path);
    pq_pump();
    cagent_emit = NULL;
    remove(path);

    char marker[32];
    snprintf(marker, sizeof(marker), "out %d", nblk - 1);      /* 只属于最后一块 */
    WCHAR wm[32];
    memset(wm, 0, sizeof(wm));
    utf8_to_wide(marker, wm, (int)(sizeof(wm) / sizeof(WCHAR)));

    int last = g_fold_n - 1;
    int last_collapsed = (last >= 0) ? g_folds[last].collapsed : -1;   /* 先取: 下面要展开它 */
    int ok = (g_fold_n == UI_FOLD_MAX) && (last >= 0) && g_folds[last].collapsed;
    WCHAR *doc = pv_doc_text();
    int hidden = doc ? (pv_wstrstr(doc, wm) == NULL) : 0;       /* 收起 = 正文不在文档里 */
    free(doc);
    ok &= hidden;
    ui_fold_toggle(last);                                        /* 还能点开 = 真在表里 */
    doc = pv_doc_text();
    int reshown = doc ? (pv_wstrstr(doc, wm) != NULL) : 0;
    free(doc);
    ok &= reshown;
    if (!ok) g_pv_fails++;
    printf("fold cap selftest: %d 个块进表 (上限 %d), 最新块 收起=%d 正文隐藏=%d 可再展开=%d [%s]\n",
           g_fold_n, UI_FOLD_MAX, last_collapsed, hidden, reshown, ok ? "OK" : "FAIL");
}

/* 宽字符子串查找 (MinGW 的 wcsstr 在没引 <wchar.h> 时未必可见, 自己写三行更稳)。 */
static const wchar_t *pv_wstrstr(const wchar_t *hay, const wchar_t *needle) {
    if (!*needle) return hay;
    for (const wchar_t *p = hay; *p; p++) {
        const wchar_t *a = p, *b = needle;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*b) return p;
    }
    return NULL;
}

/* 空串 delta 不得把 AI 半行切断 (换模型后"正式回答显示很怪"的那个症状)。
 * 有的服务端每片都同时带 reasoning_content 与 content, 没在输出的那个给 ""; 那次空上屏
 * 带的是非 AI 角色, do_append 见角色切换会 md_flush() 把正在攒的半行落地、再补一个换行
 * —— 一句话因此被拆成两行, 通篇看下来就是每行都从句子中间断开。
 * 走的是真实链路 (gui_emit 投递 + pq_pump 泵一遍), 断言的是**界面上的文本**: 半行与
 * 后续内容必须是连续的一整句, 中间没有断行。 */
static void empty_delta_selftest(void) {
    cagent_emit = gui_emit;                 /* 真实钩子: PostMessage 入队 */
    g_pq_n = 0;
    long cp0 = ui_text_cp_len(g_hHistory);
    cagent_emit("这是半句", CAGENT_ROLE_AI);
    cagent_emit("", CAGENT_ROLE_THINK);     /* 服务端"本片没内容"的空串 */
    cagent_emit("，后面接着说。\r\n", CAGENT_ROLE_AI);
    pq_pump();
    cagent_emit = NULL;

    long cp1 = ui_text_cp_len(g_hHistory);
    long n = cp1 - cp0;
    if (n < 0) n = 0;
    WCHAR *w = (WCHAR*)calloc((size_t)n + 4, sizeof(WCHAR));
    int ok = 0;
    if (w) {
        TEXTRANGEW tr;
        tr.chrg.cpMin = cp0;
        tr.chrg.cpMax = cp1;
        tr.lpstrText = w;
        SendMessageW(g_hHistory, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
        WCHAR want[64];
        memset(want, 0, sizeof(want));
        utf8_to_wide("这是半句，后面接着说。", want, (int)(sizeof(want) / sizeof(WCHAR)));
        ok = (pv_wstrstr(w, want) != NULL);
        if (!ok) {
            printf("  切成了: ");
            for (long i = 0; i < n && i < 80; i++) printf("%04X ", (unsigned)w[i]);
            printf("\n");
        }
        free(w);
    }
    if (!ok) g_pv_fails++;
    printf("empty delta selftest: 空 THINK 未切断 AI 半行 [%s]\n", ok ? "OK" : "FAIL");
}

/* 插话队列 (inbox) 不变量: FIFO、8 条封顶、超长拒绝、take 失败不丢消息。
 * 这是排队语义的地基 —— gui_on_done 的 drain 指着它, 它坏了症状是"点了没反应"
 * (取不到) 或用户的指令被静默吞掉 (take 失败却把消息划走了)。 */
static void inbox_selftest(void) {
    char buf[INBOX_SLOT];
    static char big[INBOX_SLOT + 1];       /* 静态: 64KB+ 不往线程栈上压 */
    int ok = 1;
    /* 分阶段结果: FAIL 时打印, 一眼看出坏在哪一环 (初始化成 0 = 未执行) */
    int ph_fifo = 0, ph_empty = 0, ph_cap = 0, ph_len = 0, ph_fill = 0;

    g_inbox_n = 0;
    /* FIFO: 依次入队, 取出必须原序 */
    {
        ph_fifo = inbox_push("第一条") && inbox_push("第二条") && inbox_push("第三条")
               && (g_inbox_n == 3);
        static const char *want[3] = { "第一条", "第二条", "第三条" };
        for (int i = 0; i < 3; i++) {
            if (!inbox_take(buf, sizeof(buf))) { ph_fifo = 0; break; }
            if (strcmp(buf, want[i]) != 0) ph_fifo = 0;
        }
        ok &= ph_fifo;
    }
    /* 空队列 take 不到: 返回 0, 且不碰状态 */
    ph_empty = (inbox_take(buf, sizeof(buf)) == 0) && (g_inbox_n == 0);
    ok &= ph_empty;

    /* cap 太小也 take 不到, 消息必须原样留在队里 (绝不静默丢弃) */
    g_inbox_n = 0;
    ph_cap = inbox_push("第一条") && (inbox_take(buf, 4) == 0) && (g_inbox_n == 1)
          && (strcmp(g_inbox[0], "第一条") == 0);
    ok &= ph_cap;

    /* 长度边界: INBOX_SLOT-1 可入 (要含 NUL), == INBOX_SLOT 拒绝 */
    g_inbox_n = 0;
    memset(big, 'A', sizeof(big));
    big[INBOX_SLOT - 1] = '\0';            /* 长度 INBOX_SLOT-1 */
    ph_len = inbox_push(big) && (g_inbox_n == 1) && inbox_take(buf, sizeof(buf))
          && (strlen(buf) == (size_t)INBOX_SLOT - 1);
    big[INBOX_SLOT - 1] = 'A';             /* 先把上一个用例截掉的那格补回来 */
    big[INBOX_SLOT] = '\0';                /* 长度 INBOX_SLOT: 没有 NUL 的位置了 */
    ph_len &= (inbox_push(big) == 0);
    ok &= ph_len;

    /* 封顶: 第 INBOX_MAX+1 条拒绝 */
    g_inbox_n = 0;
    ph_fill = 1;
    for (int i = 0; i < INBOX_MAX; i++) ph_fill &= inbox_push("x");
    ph_fill &= (g_inbox_n == INBOX_MAX) && (inbox_push("溢出") == 0)
            && (g_inbox_n == INBOX_MAX);   /* 拒绝不动已有队列 */

    /* 空串不入队 */
    g_inbox_n = 0;
    ph_fill &= (inbox_push("") == 0) && (inbox_push(NULL) == 0);
    ok &= ph_fill;

    g_inbox_n = 0;
    if (!ok) g_pv_fails++;
    printf("inbox selftest: FIFO/封顶 %d/超长拒绝/take 不丢 [%s] (fifo=%d empty=%d cap=%d len=%d fill=%d)\n",
           INBOX_MAX, ok ? "OK" : "FAIL", ph_fifo, ph_empty, ph_cap, ph_len, ph_fill);
}

/* drain 投递顺序: ui_drain_inbox 必须先投 FOLDEND (折上一轮答案的尾块), 再投插话回显 ——
 * 反了用户会看到"自己的插话插在上一轮答案上面"; 最后还要投一条 WM_APP_INBOX 让输入框的
 * 占位清单少掉这一条 (漏了它, 排队条目会留在输入框上挂着不消失)。
 * 两条正文都走 PostMessage, 由上面的 MyPostMessageW 记进 g_pq, 逐条断言类型 / 角色 / 文本。
 * gui_on_done 剩下的部分是线程编排 (原地再开一轮、g_running 保持 1), 由 e2e T022
 * 走真实二进制守护; 这里锁死的是消息契约 —— d46e7f3 的教训: 收尾消息赶在正文入队前,
 * 同步直调的桩测不出来, 必须像现在这样把投递原样记下来按序查。 */
static void drain_order_selftest(void) {
    char buf[INBOX_SLOT];
    int ok = 1;

    g_pq_n = 0;
    g_inbox_n = 0;
    ok &= inbox_push("插一句话");
    ok &= ui_drain_inbox(buf, sizeof(buf));
    ok &= (strcmp(buf, "插一句话") == 0);
    ok &= (g_pq_n == 3);                                   /* 恰好三条投递 */
    if (ok && g_pq[0].msg != WM_APP_FOLDEND) ok = 0;       /* 先折尾块 */
    if (ok && g_pq[1].msg != WM_APP_APPEND)  ok = 0;       /* 再回显 */
    if (ok && g_pq[1].wp  != (WPARAM)CAGENT_ROLE_USER) ok = 0;
    if (ok && (!g_pq[1].txt || strcmp(g_pq[1].txt, "插一句话\r\n") != 0)) ok = 0;
    if (ok && g_pq[2].msg != WM_APP_INBOX)   ok = 0;       /* 最后刷占位清单 */
    if (ok && g_pq[2].h != g_hInput)         ok = 0;       /* 投给输入框, 不是输出区 */
    /* txt 是 gui_emit strdup 出来的: 不 pump 的自检要自己放回去 (pq_pump 会 free) */
    for (int i = 0; i < g_pq_n; i++) if (g_pq[i].txt) free(g_pq[i].txt);
    g_pq_n = 0;

    /* 队列空: 一条投递都不该有 (空转不能污染输出区) */
    ok &= (ui_drain_inbox(buf, sizeof(buf)) == 0);
    ok &= (g_pq_n == 0);

    if (!ok) g_pv_fails++;
    printf("drain order selftest: FOLDEND -> APPEND(USER) -> INBOX [%s]\n", ok ? "OK" : "FAIL");
}

/* 数控件当前像素里"深色" (R < 200) 的个数, 只统计横向条带 y ∈ [y0, y1); y1 < 0 = 到底。
 * 判据与 test/ui_cue_probe.c 一致 (占位是灰字 + 白底, 画没画出来一数就知道)。
 *
 * 走 BitBlt 到内存 DIB 再数, 与下面的 render_to_bmp 同一条路径 —— **不要用 GetPixel**:
 * 控件刚显示出来时窗口 DC 上读到的是未更新区域, 实测恒 0 (图里明明有字), 会把
 * "占位没画"误判成失败。bmp_path 非空则顺带存图给人眼复核。 */
static int pv_dark_band(HWND h, int y0, int y1, const char *bmp_path) {
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);

    RECT rc;
    GetClientRect(h, &rc);
    int w = rc.right, hh = rc.bottom;
    if (w <= 0 || hh <= 0) return 0;
    if (y1 < 0 || y1 > hh) y1 = hh;
    if (y0 < 0) y0 = 0;

    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = hh;      /* 正高度: 内存首行 = 图像末行 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbm || !bits) return 0;
    HDC mem = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(mem, hbm);
    HDC src = GetDC(h);
    int n = 0;
    if (src && BitBlt(mem, 0, 0, w, hh, src, 0, 0, SRCCOPY)) {
        /* DIB 是自下而上的: 内存行 r 对应客户区 y = hh-1-r */
        for (int y = y0; y < y1; y++) {
            const unsigned char *row = (const unsigned char*)bits + (size_t)(hh - 1 - y) * (size_t)w * 4u;
            for (int x = 0; x < w; x++)
                if (row[x * 4] < 200) n++;      /* [0]=B [1]=G [2]=R */
        }
        if (bmp_path) save_bmp32(bmp_path, w, hh, bits);
    }
    if (src) ReleaseDC(h, src);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(hbm);
    if (bmp_path) printf("%s %dx%d 条带 y[%d,%d) 深色像素 %d\n", bmp_path, w, hh, y0, y1, n);
    return n;
}

/* 宽字符相等 (MinGW 下 wcscmp 未必可见, 与上面的 pv_wstrstr 同理由自己写) */
static int pv_weq(const wchar_t *a, const wchar_t *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

/* 输入框占位提示 ("后续任务" 清单, hooks.h 的 ui_inbox_tip / wndproc.h 的 ui_input_draw_tip):
 *   ① 文本契约: 队列空没有占位; 有队列时格式为 "后续任务:\\n· 每条一行";
 *     每条按**字符数**截到 INBOX_TIP_MAXCH, 超出补 "..." (正好等于上限则不补);
 *     多行插话只取首行, 后面还有内容也要补 "..."
 *   ② 真控件渲染: 建一个与产品同款的多行 EDIT + 挂真实 InputProc, 数像素证明
 *     空时画了灰字、**聚焦时照样画** (原生 cue 是聚焦即隐, 我们刻意不那样)、
 *     有内容时不再画 (否则会和正文重叠)
 * 顺带出 preview_input.bmp 给人眼看: 一行一条折没折行、省略号位置好不好看。 */
static void input_tip_selftest(void) {
    WCHAR tip[INBOX_TIP_LEN];
    int ok = 1, ph_empty = 0, ph_fmt = 0, ph_multiline = 0, ph_bound = 0;

    /* ---- ① 文本契约 ---- */
    g_inbox_n = 0;
    ph_empty = (ui_inbox_tip(tip, INBOX_TIP_LEN) == 0);

    char long_ascii[64];
    memset(long_ascii, 'a', 40);                 /* 40 字符: 必然被截断 */
    long_ascii[40] = '\0';

    g_inbox_n = 0;
    ok &= inbox_push("看下 Makefile");
    ok &= inbox_push(long_ascii);
    ok &= inbox_push("第一行\r\n第二行不该出现在占位里");
    int got = ui_inbox_tip(tip, INBOX_TIP_LEN);

    /* 期望串: 标题 + 每条一行 "· " + 内容; 40 字符那条按上限截 28 再补 "..." */
    char expect[512];
    snprintf(expect, sizeof(expect), "后续任务：\n· 看下 Makefile\n· %.28s...\n· 第一行...", long_ascii);
    WCHAR want[INBOX_TIP_LEN];
    utf8_to_wide(expect, want, INBOX_TIP_LEN);
    ph_fmt = (got == 1) && pv_weq(tip, want);
    if (!ph_fmt) {
        printf("  got : ");
        for (int i = 0; tip[i] && i < 120; i++) printf("%04X ", (unsigned)tip[i]);
        printf("\n  want: ");
        for (int i = 0; want[i] && i < 120; i++) printf("%04X ", (unsigned)want[i]);
        printf("\n");
    }
    /* 多行插话只占一行: 第二条正文一个字符都不许出现在占位里 */
    ph_multiline = (pv_wstrstr(tip, L"第二行") == NULL);

    /* 长度边界: 正好 MAXCH 不补 "..."; MAXCH+1 补 */
    {
        char exact[INBOX_TIP_MAXCH + 1], over[INBOX_TIP_MAXCH + 2];
        memset(exact, 'b', INBOX_TIP_MAXCH);     exact[INBOX_TIP_MAXCH] = '\0';
        memset(over,  'b', INBOX_TIP_MAXCH + 1); over[INBOX_TIP_MAXCH + 1] = '\0';
        WCHAR line[INBOX_TIP_MAXCH + 8];
        int n_exact = inbox_tip_line(exact, line, INBOX_TIP_MAXCH + 8);
        int n_over  = inbox_tip_line(over,  line, INBOX_TIP_MAXCH + 8);
        ph_bound = (n_exact == INBOX_TIP_MAXCH) && (n_over == INBOX_TIP_MAXCH + 3);
    }
    ok &= ph_empty && ph_fmt && ph_multiline && ph_bound;

    /* ---- ② 真控件渲染 (g_hInput 由 main 建好并挂了真实 InputProc) ---- */
    int line_h = dp(16);
    {
        HDC dc = GetDC(g_hInput);
        if (dc) {
            HFONT of = (HFONT)SelectObject(dc, g_hFont);
            TEXTMETRICW tm;
            if (GetTextMetricsW(dc, &tm)) line_h = tm.tmHeight + tm.tmExternalLeading;
            SelectObject(dc, of);
            ReleaseDC(g_hInput, dc);
        }
    }
    int px_plain = pv_dark_band(g_hInput, 0, -1, "preview_input.bmp");
    /* 聚焦: 原生 cue 会消失, 我们必须照旧显示 (用户正一边打字一边看排队条目) */
    SetFocus(g_hInput);
    int px_focus = pv_dark_band(g_hInput, 0, -1, NULL);
    /* 有内容: 占位必须让位 —— 第二行往下一个深色像素都不该有 (单字符文本只占第一行)。
     * 分界线用控件自己报的第二行行首位置 (EM_POSFROMCHAR 取文本末位), 不靠字号估算。
     * 容忍 ≤4 像素: 字号换算下字形下沿会越过行线 1 像素 (实测 y_line2 处那一行), 而占位
     * 真没让位时这里是几千像素 —— 两种情况的量级差三个数量级, 不靠这个阈值区分不出来。 */
    SetWindowTextW(g_hInput, L"x");
    int y_line2 = line_h;
    {
        POINTL p;
        memset(&p, 0, sizeof(p));
        if (SendMessageW(g_hInput, EM_POSFROMCHAR, (WPARAM)&p, 1) != -1 && p.y > 0)
            y_line2 = (int)p.y;
    }
    int px_text_top = pv_dark_band(g_hInput, 0, y_line2, NULL);
    int px_text_low = pv_dark_band(g_hInput, y_line2, -1, NULL);
    SetWindowTextW(g_hInput, L"");       /* 还原成空, 不给后面的自检留状态 */

    ok &= (px_plain > 0) && (px_focus > 0) && (px_text_top > 0) && (px_text_low <= 4);

    /* 善后: 队列清空、假投递队列清空 (后面还有自检要用) */
    g_inbox_n = 0;
    for (int i = 0; i < g_pq_n; i++) if (g_pq[i].txt) free(g_pq[i].txt);
    g_pq_n = 0;

    if (!ok) g_pv_fails++;
    printf("input tip selftest: 空态=%d 格式=%d 多行只取首行=%d 截断边界=%d"
           " | 像素 空=%d 聚焦=%d 有正文首行=%d 次行=%d [%s]\n",
           ph_empty, ph_fmt, ph_multiline, ph_bound,
           px_plain, px_focus, px_text_top, px_text_low, ok ? "OK" : "FAIL");
}

int main(void) {
    enable_dpi_awareness();
    LoadLibraryW(L"Msftedit.dll");     /* 注册 RICHEDIT50W 控件类 */
    dpi_init();
    g_dpi = dpi_system();              /* 与真实运行一致: 按本机主屏 DPI 渲染 */

    /* 客户区 720x780 逻辑像素 (WS_POPUP: 窗口区 = 客户区)。
     * 尺寸按逻辑像素写, 物理尺寸随本机 DPI 走 —— 200% 屏上就是 1440x1560 真实像素,
     * 于是这张图看到的就是用户在 200% 下看到的密度。
     * 高度要够装下整段示例 (含末尾的 Markdown 板块): 装不下的部分会被滚出可视区,
     * 预览就看不全了。 */
    int W = dp(720), H = dp(820);

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"cagent_preview_wnd";
    if (!RegisterClassW(&wc)) { printf("RegisterClass failed\n"); return 1; }

    /* 必须真的显示出来: 未显示的窗口里 RichEdit 不做排版, 抓出来是一片空白。
     * 放在屏幕左上角, 跑完立刻销毁 —— 一闪而过。 */
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"cagent preview", WS_POPUP,
                              40, 40, W, H, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { printf("CreateWindow failed\n"); return 1; }

    /* 输出区让出底部状态栏的高度; 状态栏要在 ui_fonts_rebuild 之前建好, 否则拿不到字体 */
    int status_h = dp(22);
    g_hHistory = CreateWindowExW(0, L"RICHEDIT50W", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        0, 0, W, H - status_h, hwnd, (HMENU)(LONG_PTR)ID_HISTORY, NULL, NULL);
    if (!g_hHistory) { printf("CreateWindow RichEdit failed\n"); return 1; }

    g_hStatus = CreateWindowW(L"STATIC", L"就绪", WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_STATUS, NULL, NULL);
    MoveWindow(g_hStatus, dp(8), H - status_h, W - dp(16), status_h, TRUE);

    /* 挂上真实的历史框子类过程 —— 折叠的命中测试与点击切换都在它里面,
     * 不挂就等于只测了函数、没测链路。 */
    g_oldHistoryProc = (WNDPROC)SetWindowLongPtrW(g_hHistory, GWLP_WNDPROC, (LONG_PTR)HistoryProc);

    /* 字体与行距走和主窗口完全相同的两条路, 否则预览的排版没有参考价值 */
    ui_fonts_rebuild(hwnd);
    SendMessageW(g_hHistory, EM_SETBKGNDCOLOR, 0, (LPARAM)(DWORD)HIST_BG);
    ui_history_parafmt(g_hHistory);
    ui_history_pad(g_hHistory, W, H - status_h);   /* 与主窗口同一份内边距 */

    for (size_t i = 0; i < sizeof(kSample) / sizeof(kSample[0]); i++)
        do_append(kSample[i].text, kSample[i].role);

    /* 默认折叠: 每遇到新块就收起上一个, 所以除最后一个外都该是收起态 */
    {
        int folded = 0;
        for (int i = 0; i < g_fold_n; i++) if (g_folds[i].collapsed) folded++;
        int ok = (folded == (g_fold_n > 0 ? g_fold_n - 1 : 0));
        if (!ok) g_pv_fails++;
        printf("默认折叠: %d/%d 个块收起 (期望 %d) [%s]\n", folded, g_fold_n,
               g_fold_n > 0 ? g_fold_n - 1 : 0, ok ? "OK" : "FAIL");
    }
    /* 预览图要展示的是**全部展开**的排版 (看配色/缩进/留白), 所以先展开再渲染。
     * 展开走 ui_fold_toggle 会置 user_toggled, 之后的自动折叠就不会再来打扰。 */
    for (int i = 0; i < g_fold_n; i++)
        if (g_folds[i].collapsed) ui_fold_toggle(i);

    dump_line_indents();

    /* 展开之后每个标题行的缩进都必须还是 0 —— 展开会在段边界插入内容, RichEdit 可能把
     * 相邻段的缩进带跑 (实测第 N 块展开后第 N+1 块标题行 indent 从 0 变 240)。
     * 这是 ui_fold_refresh_heads 那道兜底的回归检查。 */
    {
        int bad = 0;
        for (int i = 0; i < g_fold_n; i++) {
            PARAFORMAT2 pf;
            memset(&pf, 0, sizeof(pf));
            pf.cbSize = sizeof(pf);
            SendMessageW(g_hHistory, EM_SETSEL, g_folds[i].cp_head, g_folds[i].cp_head);
            SendMessageW(g_hHistory, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
            if (pf.dxStartIndent != 0) {
                printf("  标题行缩进异常: 块 %d = %ld twips (期望 0)\n",
                       i, (long)pf.dxStartIndent);
                bad++;
            }
        }
        printf("展开后标题行缩进: %s\n", bad ? "FAIL" : "OK");
        if (bad) g_pv_fails++;
    }

    /* 置顶显示, 免得被别的窗口盖住 —— 抓屏取的是屏幕上真实画出来的像素 */
    SetWindowPos(hwnd, HWND_TOPMOST, 40, 40, W, H, SWP_SHOWWINDOW);
    UpdateWindow(hwnd);

    /* 展开全部块之后滚位停在"收起态文档"的底部 (ui_fold_toggle 恢复的是展开前的滚位),
     * 末尾几行会滚出可视区。出图前滚到底, 预览才看得全。 */
    SendMessageW(g_hHistory, WM_VSCROLL, SB_BOTTOM, 0);

    render_to_bmp(g_hHistory, "preview_out.bmp", W, H);

    /* 诊断: cp_head 到底指向哪段文本 —— 命中测试的 x 坐标在块 2/3 上是 33 而不是 1,
     * 但段落缩进读出来又是 0, 两者矛盾, 说明索引位置本身不对。 */
    for (int i = 0; i < g_fold_n; i++) {
        int line = (int)SendMessageW(g_hHistory, EM_EXLINEFROMCHAR, 0, (LPARAM)g_folds[i].cp_head);
        long lstart = (long)SendMessageW(g_hHistory, EM_LINEINDEX, (WPARAM)line, 0);
        if (lstart != g_folds[i].cp_head)
            printf("  索引异常: 块 %d cp_head=%ld 但行首是 %ld\n",
                   i, g_folds[i].cp_head, lstart);
    }

    /* 命中测试: 由标题行首字符的位置反查它属于哪个块, 再用**真实的鼠标消息**打过去 ——
     * 走的是"点击 -> 命中测试 -> 切换"的完整链路, 而不是绕过交互直接调 ui_fold_toggle。 */
    for (int i = 0; i < g_fold_n; i++) {
        POINTL pt;
        memset(&pt, 0, sizeof(pt));
        SendMessageW(g_hHistory, EM_POSFROMCHAR, (WPARAM)&pt, (LPARAM)g_folds[i].cp_head);
        int hit = ui_fold_hit(pt);
        if (hit != i) g_pv_fails++;
        printf("hit test: 块 %d 标题行 (%ld,%ld) -> %d (期望 %d)%s\n",
               i, (long)pt.x, (long)pt.y, hit, i, hit == i ? "" : "  <-- FAIL");
        if (hit != i) continue;
        /* 点一下收起, 再点一下展开 —— 既验证链路, 也把状态还原给后面的自检 */
        SendMessageW(g_hHistory, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
        int collapsed = g_folds[i].collapsed;
        SendMessageW(g_hHistory, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
        if (!(collapsed == 1 && !g_folds[i].collapsed)) g_pv_fails++;
        printf("  click toggle: 收起=%d -> 展开=%d %s\n",
               collapsed, g_folds[i].collapsed, (collapsed == 1 && !g_folds[i].collapsed) ? "[OK]" : "[FAIL]");
    }

    fold_selftest();
    head_indent_selftest();
    style_selftest();
    reasoning_flow_selftest();

    /* 收起**工具块**, 再出一张图 —— 这是用户实际会点的那个: 工具输出经常几十上百行,
     * 而思考块只有推理模型才产生。收完立刻还原, 免得影响后面的 DPI 检查。 */
    if (g_fold_n > 0) {
        int tool_idx = -1;
        for (int i = 0; i < g_fold_n; i++) if (g_folds[i].kind == UI_FOLD_TOOL) tool_idx = i;
        if (tool_idx < 0) tool_idx = g_fold_n - 1;
        ui_fold_toggle(tool_idx);
        render_to_bmp(g_hHistory, "preview_folded.bmp", W, H);
        ui_fold_toggle(tool_idx);
    }

    /* 滚动测试放在出图之后 —— 它要往文档里灌 40 行填充内容, 会把图撑乱 */
    scroll_hit_selftest();
    scroll_anchor_selftest();

    /* 顺带验一把 DPI 切换时的段落重算: 缩放前后读回第一段的段前距, 应当正好翻倍。
     * 这是"跨显示器拖动后留白乱掉"那个坑的回归检查 —— 放在渲染之后做, 免得改了间距
     * 影响上面那两张图。 */
    {
        PARAFORMAT2 p1, p2;
        memset(&p1, 0, sizeof(p1)); p1.cbSize = sizeof(p1);
        memset(&p2, 0, sizeof(p2)); p2.cbSize = sizeof(p2);
        SendMessageW(g_hHistory, EM_SETSEL, 0, 0);
        SendMessageW(g_hHistory, EM_GETPARAFORMAT, 0, (LPARAM)&p1);
        ui_para_reflow(g_hHistory, 96, 192);
        SendMessageW(g_hHistory, EM_SETSEL, 0, 0);
        SendMessageW(g_hHistory, EM_GETPARAFORMAT, 0, (LPARAM)&p2);
        printf("para reflow 96->192: 段前距 %ld -> %ld twips (期望 2 倍), 缩进 %ld -> %ld\n",
               (long)p1.dySpaceBefore, (long)p2.dySpaceBefore,
               (long)p1.dxStartIndent, (long)p2.dxStartIndent);
    }

    /* 输入框 (产品里由 WM_CREATE 建, 这里补上 —— 占位提示的自检要一个真控件, 且
     * drain 的投递契约也要有目标窗口)。建在所有出图之后, 免得挡住 preview_out.bmp。 */
    g_hInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_INPUT, NULL, NULL);
    if (!g_hInput) { printf("CreateWindow input EDIT failed\n"); return 1; }
    SendMessageW(g_hInput, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    g_oldInputProc = (WNDPROC)SetWindowLongPtrW(g_hInput, GWLP_WNDPROC, (LONG_PTR)InputProc);
    SetWindowPos(g_hInput, HWND_TOP, dp(8), dp(8), dp(420), dp(96), SWP_SHOWWINDOW);

    /* 回放自检放最后 —— 它会清空历史框并重置块表, 前面那些检查都还要用当前内容。
     * inbox/drain 是纯逻辑与投递契约, 不碰文档内容, 排在它前面。 */
    note_split_selftest();
    empty_delta_selftest();
    inbox_selftest();
    drain_order_selftest();
    input_tip_selftest();      /* 占位提示: 文本契约 + 真控件渲染 (顺带出 preview_input.bmp) */
    replay_selftest();
    fold_cap_selftest();

    ui_fold_reset();
    DestroyWindow(hwnd);
    if (g_hFontHist) DeleteObject(g_hFontHist);
    /* 退出码 = 失败数: make preview 的调用方 (人 / CI) 因此能把自检当门禁。
     * d46e7f3 的教训 —— 当时所有自检都绿, 生产却是坏的。 */
    if (g_pv_fails) printf("preview selftest: %d 项 FAIL\n", g_pv_fails);
    return g_pv_fails ? 1 : 0;
}
