/*
 * md_test.c - Markdown 渲染器 (ui/md.h) 的边界单测 (无 UI)
 *
 * 独立 TU: 桩掉 SendMessageW, 不建任何窗口。拦截 EM_REPLACESEL / EM_SETCHARFORMAT
 * / EM_SETPARAFORMAT / EM_GETTEXTLENGTHEX, 用一个假文档模型记录每次插入的
 * 区间 + 字符样式 + 段落属性, 据此断言:
 *   ① 不变式: 拼回来的文档 == 输入 (纯加样式, 绝不增删字符)
 *   ② 标记字符淡化(DIM 灰), 行内/行级内容拿到正确语义样式
 *   ③ 流式: 标记被 delta 劈开时, 行缓冲凑齐整行再解析, 结果与整段喂入一致
 *   ④ CommonMark 边界: 反斜杠转义 / 等长反引号代码 span / *** 粗斜
 *   ⑤ 接线: do_append 里"切角色先落地半行、围栏跨过 NOTE、TOOL 复位"
 *
 * 编译/运行: make mdtest   (退出码 = 失败用例数)
 */

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "core/base.h"      /* windows.h 已在上面; 角色常量与宿主钩子声明 */

/* ===== 桩 SendMessageW =====
 * 必须在任何调用它的 UI 头文件之前完成接管。 */
#undef SendMessageW

static long  g_doclen;                 /* 假文档的 cp 长度 */
static WCHAR g_doctext[1 << 16];       /* 假文档的文本 (逐字还原, 校验字符守恒) */
static int   g_doclost;                /* 文本超出数组 (测试失败标志) */

typedef struct {
    long s;                            /* 插入起点 */
    int  n;                            /* 码元数 */
    int  bold, italic, underline, mono;
    COLORREF fg;
    int  sp_before, sp_after, indent;  /* 逻辑像素 (twips / 15) */
} Run;

static Run g_runs[8192];
static int g_runc;

static CHARFORMAT2W g_cf;              /* 最近一次 EM_SETCHARFORMAT */
static int g_sp_before, g_sp_after, g_indent;   /* 最近一次 EM_SETPARAFORMAT */

/* twips -> 逻辑像素: ui_twips(px) = px * dpi/96 * 1440 / dpi = px*15, 与 DPI 无关 */
static int twips_to_px(int tw) { return tw / 15; }

static void record_run(const WCHAR *t, int n) {
    if (g_runc < 8192) {
        Run *r = &g_runs[g_runc++];
        r->s = g_doclen;
        r->n = n;
        r->bold      = (g_cf.dwEffects & CFE_BOLD) ? 1 : 0;
        r->italic    = (g_cf.dwEffects & CFE_ITALIC) ? 1 : 0;
        r->underline = (g_cf.dwEffects & CFE_UNDERLINE) ? 1 : 0;
        r->mono      = (wcsncmp(g_cf.szFaceName, L"Consolas", 8) == 0) ? 1 : 0;
        r->fg        = g_cf.crTextColor;
        r->sp_before = g_sp_before;   /* 先记旧值, EM_SETPARAFORMAT 回来后再补 */
        r->sp_after  = g_sp_after;
        r->indent    = g_indent;
    }
    if (g_doclen + n < (long)(sizeof(g_doctext) / sizeof(g_doctext[0]))) {
        memcpy(g_doctext + g_doclen, t, (size_t)n * sizeof(WCHAR));
        g_doclen += n;
        g_doctext[g_doclen] = L'\0';
    } else {
        g_doclost = 1;
        g_doclen += n;
    }
}

LRESULT WINAPI MySendMessageW(HWND h, UINT msg, WPARAM w, LPARAM l) {
    (void)h;
    switch (msg) {
    case EM_GETTEXTLENGTHEX:
        return (LRESULT)g_doclen;
    case EM_GETCHARFORMAT: {
        /* 固定基底: 只给字体族, 不给字号/效果 —— 与真实控件"插入点格式"等效 */
        CHARFORMAT2W *b = (CHARFORMAT2W*)l;
        memset(b, 0, sizeof(*b));
        b->cbSize = sizeof(*b);
        b->dwMask = CFM_FACE;
        wcscpy(b->szFaceName, L"Microsoft YaHei UI");
        return (LRESULT)sizeof(*b);
    }
    case EM_SETCHARFORMAT:
        if (w == SCF_SELECTION && l) g_cf = *(const CHARFORMAT2W*)l;
        return 0;
    case EM_SETPARAFORMAT: {
        const PARAFORMAT2 *pf = (const PARAFORMAT2*)l;
        if (pf->dwMask & PFM_SPACEBEFORE) g_sp_before = twips_to_px(pf->dySpaceBefore);
        if (pf->dwMask & PFM_SPACEAFTER)  g_sp_after  = twips_to_px(pf->dySpaceAfter);
        if (pf->dwMask & PFM_STARTINDENT) g_indent    = twips_to_px(pf->dxStartIndent);
        /* ui_insert_ex 在 EM_REPLACESEL 之后才设段落格式 —— 回填到刚记的 run 上 */
        if (g_runc > 0) {
            g_runs[g_runc - 1].sp_before = g_sp_before;
            g_runs[g_runc - 1].sp_after  = g_sp_after;
            g_runs[g_runc - 1].indent    = g_indent;
        }
        return 0;
    }
    case EM_REPLACESEL: {
        const WCHAR *t = (const WCHAR*)l;
        int n = t ? lstrlenW(t) : 0;
        if (n > 0) record_run(t, n);
        return 0;
    }
    default:
        return 0;   /* EM_SETSEL / EM_SCROLLCARET / WM_VSCROLL / WM_SETREDRAW ... */
    }
}
#define SendMessageW MySendMessageW

/* 聚合产品代码: 角色样式表 / 插入骨架 / Markdown 渲染器 */
#include "ui/state.h"
#include "ui/dpi.h"
#include "ui/helpers.h"
#include "ui/md.h"

/* ===== 测试框架 ===== */
static int g_fail;

static void reset_test(void) {
    g_runc = 0;
    g_doclen = 0;
    g_doctext[0] = L'\0';
    g_doclost = 0;
    g_sp_before = g_sp_after = g_indent = 0;
    memset(&g_cf, 0, sizeof(g_cf));
    md_reset();
    g_stream_chars = 0;
    g_last_role = -1;
    g_last_no_lf = 0;
}

static void feed(const WCHAR *w) { md_feed(w); }

/* 文档必须与期望逐字相同 (字符守恒不变式) */
static void doc_is(const WCHAR *want, const char *name) {
    if (g_doclost) {
        printf("  FAIL [%s] 假文档溢出\n", name);
        g_fail++;
        return;
    }
    if (wcscmp(g_doctext, want) != 0) {
        printf("  FAIL [%s] 文档不一致:\n    got  <%ls>\n    want <%ls>\n",
               name, g_doctext, want);
        g_fail++;
    }
}

/* 精确断言第 idx 个 run */
static void run_is(int idx, long s, int n, int bold, int italic, int und,
                   int mono, COLORREF fg, int ind, const char *name) {
    if (idx >= g_runc) {
        printf("  FAIL [%s] run %d 缺失 (共 %d 个)\n", name, idx, g_runc);
        g_fail++;
        return;
    }
    Run *r = &g_runs[idx];
    if (r->s != s || r->n != n || r->bold != bold || r->italic != italic ||
        r->underline != und || r->mono != mono || r->fg != fg || r->indent != ind) {
        printf("  FAIL [%s] run %d: got s=%ld n=%d b=%d i=%d u=%d m=%d fg=%u ind=%d; "
               "want s=%ld n=%d b=%d i=%d u=%d m=%d fg=%u ind=%d\n",
               name, idx, r->s, r->n, r->bold, r->italic, r->underline, r->mono,
               (unsigned)r->fg, r->indent, s, n, bold, italic, und, mono,
               (unsigned)fg, ind);
        g_fail++;
    }
}

static void para_is(int idx, int sp_before, int sp_after, int ind, const char *name) {
    if (idx >= g_runc) {
        printf("  FAIL [%s] run %d 缺失 (共 %d 个)\n", name, idx, g_runc);
        g_fail++;
        return;
    }
    Run *r = &g_runs[idx];
    if (r->sp_before != sp_before || r->sp_after != sp_after || r->indent != ind) {
        printf("  FAIL [%s] run %d 段落属性: got sp=%d/%d ind=%d; want sp=%d/%d ind=%d\n",
               name, idx, r->sp_before, r->sp_after, r->indent,
               sp_before, sp_after, ind);
        g_fail++;
    }
}

/* 该 run 的样式种类 (粗斜同时置位归 BOTH) */
static int kind_of(int i) {
    Run *r = &g_runs[i];
    if (r->bold && r->italic) return 3;   /* BOTH */
    if (r->bold)   return 1;              /* BOLD */
    if (r->italic) return 2;              /* ITALIC */
    return 0;                             /* NORMAL */
}

/* AI 正文色 / 标记淡化色 (与 md.h 的常量对得上) */
#define AI_FG    RGB(26, 26, 26)
#define W(x) L##x

int main(void) {
    /* ---------------------------------------------------------------
     * 1) 字符守恒不变式: 喂进去什么, 文档里就有什么
     * ------------------------------------------------------------- */
    reset_test(); feed(W("**bold**\r\n"));            doc_is(W("**bold**\r\n"), "inv-bold");
    reset_test(); feed(W("*it*\r\n"));                doc_is(W("*it*\r\n"), "inv-italic");
    reset_test(); feed(W("***bi***\r\n"));            doc_is(W("***bi***\r\n"), "inv-triple");
    reset_test(); feed(W("`code`\r\n"));              doc_is(W("`code`\r\n"), "inv-code");
    reset_test(); feed(W("[t](u)\r\n"));              doc_is(W("[t](u)\r\n"), "inv-link");
    reset_test(); feed(W("# H\r\n"));                 doc_is(W("# H\r\n"), "inv-heading");
    reset_test(); feed(W("## T ##\r\n"));             doc_is(W("## T ##\r\n"), "inv-heading-close");
    reset_test(); feed(W("> q\r\n"));                 doc_is(W("> q\r\n"), "inv-quote");
    reset_test(); feed(W("- i\r\n1. j\r\n"));         doc_is(W("- i\r\n1. j\r\n"), "inv-list");
    reset_test(); feed(W("---\r\n"));                 doc_is(W("---\r\n"), "inv-rule");
    reset_test(); feed(W("```\r\nx=1\r\n```\r\n"));   doc_is(W("```\r\nx=1\r\n```\r\n"), "inv-fence");
    reset_test(); feed(W("\\*esc\\*\r\n"));           doc_is(W("\\*esc\\*\r\n"), "inv-escape");
    reset_test(); feed(W("**a** *b* `c`\r\n"));       doc_is(W("**a** *b* `c`\r\n"), "inv-mix");
    reset_test(); feed(W("中文**粗体**与`代码`\r\n")); doc_is(W("中文**粗体**与`代码`\r\n"), "inv-cjk");
    reset_test(); feed(W("hi \xD83D\xDE00\r\n"));     doc_is(W("hi \xD83D\xDE00\r\n"), "inv-emoji");
    reset_test(); feed(W("``a`b``\r\n"));             doc_is(W("``a`b``\r\n"), "inv-codelong");
    /* 空文档时行首空行被 ui_insert_ex 吃掉 (与旧行为一致), 其余原样 */
    reset_test(); feed(W("\r\nx\r\n"));               doc_is(W("x\r\n"), "inv-leadblank");

    /* ---------------------------------------------------------------
     * 2) 样式落点: 标记 DIM(灰110), 内容拿语义样式
     *    半行用 md_flush 落地 (不带行尾), run 边界最干净
     * ------------------------------------------------------------- */
    reset_test(); feed(W("**bold**")); md_flush();
    run_is(0, 0, 2, 0, 0, 0, 0, MD_DIM_FG, 0, "bold");
    run_is(1, 2, 4, 1, 0, 0, 0, AI_FG,    0, "bold");
    run_is(2, 6, 2, 0, 0, 0, 0, MD_DIM_FG, 0, "bold");

    reset_test(); feed(W("*it*")); md_flush();
    run_is(0, 0, 1, 0, 0, 0, 0, MD_DIM_FG, 0, "italic");
    run_is(1, 1, 2, 0, 1, 0, 0, AI_FG,    0, "italic");
    run_is(2, 3, 1, 0, 0, 0, 0, MD_DIM_FG, 0, "italic");

    reset_test(); feed(W("`code`")); md_flush();
    run_is(0, 0, 1, 0, 0, 0, 0, MD_DIM_FG,  0, "code");
    run_is(1, 1, 4, 0, 0, 0, 1, MD_CODE_FG, 0, "code");
    run_is(2, 5, 1, 0, 0, 0, 0, MD_DIM_FG,  0, "code");

    reset_test(); feed(W("[t](u)")); md_flush();
    run_is(0, 0, 1, 0, 0, 0, 0, MD_DIM_FG,  0, "link");   /* [ */
    run_is(1, 1, 1, 0, 0, 1, 0, MD_LINK_FG, 0, "link");   /* t */
    run_is(2, 2, 4, 0, 0, 0, 0, MD_DIM_FG,  0, "link");   /* ](u) */

    reset_test(); feed(W("# H")); md_flush();
    run_is(0, 0, 2, 0, 0, 0, 0, MD_DIM_FG, 0, "heading");  /* # 前置标记 */
    run_is(1, 2, 1, 1, 0, 0, 0, AI_FG,    0, "heading");  /* H 加粗 */
    para_is(1, 16 + 4, 0, 0, "heading");                  /* h1 段前距 +4 */

    reset_test(); feed(W("## T ##")); md_flush();
    run_is(0, 0, 3, 0, 0, 0, 0, MD_DIM_FG, 0, "head-close");  /* "## " */
    run_is(1, 3, 2, 1, 0, 0, 0, AI_FG,    0, "head-close");  /* "T " */
    run_is(2, 5, 2, 0, 0, 0, 0, MD_DIM_FG, 0, "head-close");  /* "##" 收尾序列 */

    reset_test(); feed(W("### deep")); md_flush();
    para_is(0, 16 + 2, 0, 0, "head-h3");                 /* h3 段前距 +2 */

    reset_test(); feed(W("> q")); md_flush();
    run_is(0, 0, 2, 0, 0, 0, 0, MD_DIM_FG, MD_QUOTE_INDENT, "quote");  /* "> " */
    run_is(1, 2, 1, 0, 0, 0, 0, MD_QUOTE_FG, MD_QUOTE_INDENT, "quote");
    para_is(1, 16, 0, MD_QUOTE_INDENT, "quote");         /* 引用整段缩进 */

    reset_test(); feed(W("- i")); md_flush();
    run_is(0, 0, 2, 0, 0, 0, 0, MD_DIM_FG, 0, "list");
    run_is(1, 2, 1, 0, 0, 0, 0, AI_FG,    0, "list");

    reset_test(); feed(W("3. j")); md_flush();
    run_is(0, 0, 3, 0, 0, 0, 0, MD_DIM_FG, 0, "olist");  /* "3. " */
    run_is(1, 3, 1, 0, 0, 0, 0, AI_FG,    0, "olist");

    reset_test(); feed(W("---")); md_flush();
    run_is(0, 0, 3, 0, 0, 0, 0, MD_DIM_FG, 0, "rule");

    /* 嵌套: **粗体套 `行内码`** */
    reset_test(); feed(W("**b `c`**")); md_flush();
    run_is(0, 0, 2, 0, 0, 0, 0, MD_DIM_FG,  0, "nest");    /* ** */
    run_is(1, 2, 2, 1, 0, 0, 0, AI_FG,      0, "nest");    /* "b " 粗体 */
    run_is(2, 4, 1, 0, 0, 0, 0, MD_DIM_FG,  0, "nest");    /* 反引号标记 */
    run_is(3, 5, 1, 1, 0, 0, 1, MD_CODE_FG, 0, "nest");    /* c 等宽(外层粗体保留) */
    run_is(4, 6, 1, 0, 0, 0, 0, MD_DIM_FG,  0, "nest");
    run_is(5, 7, 2, 0, 0, 0, 0, MD_DIM_FG,  0, "nest");    /* ** */

    /* ---------------------------------------------------------------
     * 3) 围栏代码块: 标记行 DIM 等宽, 内容等宽深灰且**段距全 0**,
     *    块内不做行内解析 (星号反引号按字面)
     * ------------------------------------------------------------- */
    reset_test(); feed(W("```\r\nx=1\r\n```\r\n"));
    run_is(0, 0,  5, 0, 0, 0, 1, MD_DIM_FG,  0, "fence");   /* ``` 开 */
    run_is(1, 5,  5, 0, 0, 0, 1, MD_CODE_FG, 0, "fence");   /* x=1 */
    run_is(2, 10, 5, 0, 0, 0, 1, MD_DIM_FG,  0, "fence");   /* ``` 关 */
    para_is(0, 16, 0, 0, "fence");        /* 标记行用角色默认段前距 */
    para_is(1, 0,  0, 0, "fence");        /* 内容行 0 —— 多行块不能被拉散 */
    para_is(2, 0,  0, 0, "fence");

    reset_test(); feed(W("```\r\n**not bold**\r\n```\r\n"));
    run_is(1, 5, 14, 0, 0, 0, 1, MD_CODE_FG, 0, "fence-raw");  /* 粗体位不置 */

    /* 更长反引号的围栏: 内部 3 个反引号不关围栏 */
    reset_test(); feed(W("````\r\n```\r\ninner\r\n````\r\n"));
    run_is(1, 6, 5, 0, 0, 0, 1, MD_CODE_FG, 0, "fence-len");   /* "```" 当代码行 */
    run_is(2, 11, 7, 0, 0, 0, 1, MD_CODE_FG, 0, "fence-len");  /* "inner\r\n" */

    /* ---------------------------------------------------------------
     * 4) 流式: 标记被 delta 劈开 —— 凑齐整行再解析, 与整段喂入同结果
     * ------------------------------------------------------------- */
    reset_test(); feed(W("**bo")); feed(W("ld**")); feed(W("\r\n"));
    run_is(0, 0, 2, 0, 0, 0, 0, MD_DIM_FG, 0, "stream-bold");
    run_is(1, 2, 4, 1, 0, 0, 0, AI_FG,    0, "stream-bold");
    run_is(2, 6, 2, 0, 0, 0, 0, MD_DIM_FG, 0, "stream-bold");
    doc_is(W("**bold**\r\n"), "stream-bold-doc");

    reset_test(); feed(W("# He")); feed(W("ading\r\n"));
    run_is(0, 0, 2, 0, 0, 0, 0, MD_DIM_FG, 0, "stream-head");
    run_is(1, 2, 7, 1, 0, 0, 0, AI_FG,    0, "stream-head");   /* "Heading" */
    run_is(2, 9, 2, 0, 0, 0, 0, MD_DIM_FG, 0, "stream-head");   /* 行尾 CRLF 单独淡化 */

    reset_test(); feed(W("``")); feed(W("`\r\n")); feed(W("if (x) {\r\n"));
    feed(W("```\r\n"));
    run_is(0, 0, 5, 0, 0, 0, 1, MD_DIM_FG,  0, "stream-fence");
    run_is(1, 5, 10, 0, 0, 0, 1, MD_CODE_FG, 0, "stream-fence");  /* "if (x) {\r\n" */
    run_is(2, 15, 5, 0, 0, 0, 1, MD_DIM_FG, 0, "stream-fence");

    reset_test(); feed(W("line1\r\nli")); feed(W("ne2\r\n"));
    doc_is(W("line1\r\nline2\r\n"), "stream-lines");

    /* 行缓冲状态机 */
    reset_test(); feed(W("abc"));
    if (!md_pending()) { printf("  FAIL [pending] 半行应挂起\n"); g_fail++; }
    long got = md_flush();
    if (got != 3) { printf("  FAIL [flush] 插入 %ld, 期望 3\n", got); g_fail++; }
    if (md_pending()) { printf("  FAIL [pending] flush 后应清空\n"); g_fail++; }
    doc_is(W("abc"), "flush-doc");

    reset_test(); feed(W("abc")); md_reset(); feed(W("x\r\n"));
    doc_is(W("x\r\n"), "reset-drop");    /* 丢弃的半行不许再出现 */

    /* ---------------------------------------------------------------
     * 5) CommonMark 边界
     * ------------------------------------------------------------- */
    reset_test(); feed(W("\\*esc\\*")); md_flush();
    run_is(0, 0, 7, 0, 0, 0, 0, AI_FG, 0, "escape");      /* 含反斜杠共 7 码元, 全普通 */

    reset_test(); feed(W("\\`not code\\`")); md_flush();
    run_is(0, 0, 12, 0, 0, 0, 0, AI_FG, 0, "escape-code");

    reset_test(); feed(W("\\# not heading")); md_flush();
    run_is(0, 0, 14, 0, 0, 0, 0, AI_FG, 0, "escape-head");

    reset_test(); feed(W("``a`b``")); md_flush();
    run_is(0, 0, 2, 0, 0, 0, 0, MD_DIM_FG,  0, "codelong");
    run_is(1, 2, 3, 0, 0, 0, 1, MD_CODE_FG, 0, "codelong");   /* a`b */
    run_is(2, 5, 2, 0, 0, 0, 0, MD_DIM_FG,  0, "codelong");

    reset_test(); feed(W("***bi***")); md_flush();
    run_is(0, 0, 3, 0, 0, 0, 0, MD_DIM_FG, 0, "triple");
    run_is(1, 3, 2, 1, 1, 0, 0, AI_FG,    0, "triple");
    if (kind_of(1) != 3) { printf("  FAIL [triple] 内部应同时粗斜\n"); g_fail++; }
    run_is(2, 5, 3, 0, 0, 0, 0, MD_DIM_FG, 0, "triple");

    reset_test(); feed(W("***x")); md_flush();
    run_is(0, 0, 4, 0, 0, 0, 0, AI_FG, 0, "triple-open");   /* 无闭合: 全普通 */

    reset_test(); feed(W("**a** *b*")); md_flush();
    run_is(1, 2, 1, 1, 0, 0, 0, AI_FG, 0, "mix");
    run_is(5, 7, 1, 0, 1, 0, 0, AI_FG, 0, "mix");

    /* 未闭合的行内码 run 不抹掉内容 */
    reset_test(); feed(W("`unclosed")); md_flush();
    run_is(0, 0, 9, 0, 0, 0, 0, AI_FG, 0, "code-open");

    /* 未闭合的强调: 星号按字面, 不当斜体 */
    reset_test(); feed(W("a*b")); md_flush();
    run_is(0, 0, 3, 0, 0, 0, 0, AI_FG, 0, "star-unclosed");

    /* ---------------------------------------------------------------
     * 5b) 空白治理: 空行不落地 (块分隔符) / 段前距只给块首行
     *     空行一旦落成空段落, 它会吃满"段前距 16 + 整行行高", 而下一块还各带
     *     一份段前距 —— 模型一句 "\n\n" 就是几十上百像素的空白 (截图实录)。
     * ------------------------------------------------------------- */
    /* 空行不进文档, 前后两块都带段前距 (间距只由下一块表达) */
    reset_test(); feed(W("a\r\n\r\nb\r\n"));
    doc_is(W("a\r\nb\r\n"), "blank-skip");
    run_is(0, 0, 3, 0, 0, 0, 0, AI_FG, 0, "blank-skip");   /* "a\r\n" */
    run_is(1, 3, 3, 0, 0, 0, 0, AI_FG, 0, "blank-skip");   /* "b\r\n" */
    para_is(0, 16, 0, 0, "blank-skip");
    para_is(1, 16, 0, 0, "blank-skip");   /* 空行后仍是块首, 段前距不丢 */

    /* 连续多个空行同理: 一个都不落地 */
    reset_test(); feed(W("a\r\n\r\n\r\nb\r\n"));
    doc_is(W("a\r\nb\r\n"), "blank-multi");

    /* 软换行续行: 同一段落的第二行段前距 0 */
    reset_test(); feed(W("a\r\nb\r\n"));
    para_is(0, 16, 0, 0, "softwrap");
    para_is(1, 0,  0, 0, "softwrap");

    /* 列表相邻项紧凑; 段落后接列表 = 列表打断段落, 块边界段前距回来 */
    reset_test(); feed(W("- x\r\n- y\r\n"));
    para_is(0, 16, 0, 0, "list-tight");
    para_is(2, 0,  0, 0, "list-tight");
    reset_test(); feed(W("text\r\n- x\r\n"));
    para_is(0, 16, 0, 0, "list-break");
    para_is(1, 16, 0, 0, "list-break");

    /* 引用相邻行紧凑; 段落后接引用 = 块边界 */
    reset_test(); feed(W("> a\r\n> b\r\n"));
    para_is(0, 16, 0, MD_QUOTE_INDENT, "quote-tight");
    para_is(2, 0,  0, MD_QUOTE_INDENT, "quote-tight");
    reset_test(); feed(W("text\r\n> a\r\n"));
    para_is(1, 16, 0, MD_QUOTE_INDENT, "quote-break");

    /* 标题/分割线之后都是块边界 (标题还叠标题加量) */
    reset_test(); feed(W("text\r\n## H\r\n"));
    para_is(1, 16 + 4, 0, 0, "head-break");
    reset_test(); feed(W("text\r\n---\r\n"));
    para_is(1, 16, 0, 0, "rule-break");

    /* 围栏内的空行是**代码内容**, 照常按字面上屏 (不能被当块分隔符吞掉) */
    reset_test(); feed(W("```\r\nx\r\n\r\ny\r\n```\r\n"));
    doc_is(W("```\r\nx\r\n\r\ny\r\n```\r\n"), "fence-blank");
    run_is(2, 8, 2, 0, 0, 0, 1, MD_CODE_FG, 0, "fence-blank");

    /* 围栏闭合之后的第一行是块首 (段前距回来), 不继承围栏前的段落状态 */
    reset_test(); feed(W("p\r\n```\r\nc\r\n```\r\nafter\r\n"));
    para_is(4, 16, 0, 0, "fence-after");

    /* ---------------------------------------------------------------
     * 6) 接线 (do_append): 切角色先落地半行 / 围栏跨 NOTE / TOOL 复位
     * ------------------------------------------------------------- */
    /* NOTE 是流式插播: 围栏状态跨过它, 半行按代码样式落地, 兜底换行落在半行之后 */
    reset_test();
    do_append("```\r\nx", CAGENT_ROLE_AI);
    do_append("(note)\r\n", CAGENT_ROLE_NOTE);
    do_append("y\r\n```\r\n", CAGENT_ROLE_AI);
    doc_is(W("```\r\nx\r\n(note)\r\ny\r\n```\r\n"), "wire-note-doc");
    run_is(4, 16, 3, 0, 0, 0, 1, MD_CODE_FG, 0, "wire-note");  /* "y\r\n" 仍是代码 */
    if (g_stream_chars != 14) {   /* "```\r\n"(5)+"x"(1)+"y\r\n"(3)+"```\r\n"(5) */
        printf("  FAIL [wire-note] g_stream_chars=%ld, 期望 14\n", g_stream_chars);
        g_fail++;
    }

    /* TOOL 意味着 AI 分段结束: 未闭合的围栏复位, 续行按正文解析 */
    reset_test();
    do_append("```\r\nx", CAGENT_ROLE_AI);
    do_append("read_file\r\n", CAGENT_ROLE_TOOL);
    do_append("**b**\r\n", CAGENT_ROLE_AI);
    run_is(4, 19, 2, 0, 0, 0, 0, MD_DIM_FG, 0, "wire-tool");
    run_is(5, 21, 1, 1, 0, 0, 0, AI_FG,    0, "wire-tool");   /* b 是粗体不是代码 */

    /* 角色切换的兜底换行落在半行之后 (而不是之前) */
    reset_test();
    do_append("half", CAGENT_ROLE_AI);
    do_append("tail\r\n", CAGENT_ROLE_ERR);
    doc_is(W("half\r\ntail\r\n"), "wire-eol");

    /* ---------------------------------------------------------------
     * 汇总
     * ------------------------------------------------------------- */
    if (g_fail == 0) printf("md_test: ALL PASS\n");
    else             printf("md_test: %d FAIL\n", g_fail);
    return g_fail;
}
