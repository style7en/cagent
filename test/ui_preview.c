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

    /* 回放自检放最后 —— 它会清空历史框并重置块表, 前面那些检查都还要用当前内容 */
    note_split_selftest();
    replay_selftest();

    ui_fold_reset();
    DestroyWindow(hwnd);
    if (g_hFontHist) DeleteObject(g_hFontHist);
    /* 退出码 = 失败数: make preview 的调用方 (人 / CI) 因此能把自检当门禁。
     * d46e7f3 的教训 —— 当时所有自检都绿, 生产却是坏的。 */
    if (g_pv_fails) printf("preview selftest: %d 项 FAIL\n", g_pv_fails);
    return g_pv_fails ? 1 : 0;
}
