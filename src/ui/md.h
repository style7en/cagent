/*
 * ui/md.h - AI 正文的简单 Markdown 渲染 (纯样式层)
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 * 依赖 helpers.h 的 ui_insert_ex (显式样式覆盖插入), 故必须排在它之后 include。
 *
 * 四条设计原则 (旧的 feature/markdown-rendering 分支验证过思路, 这里是按当前
 * 角色架构重写的版本):
 *
 * ① **只渲染正式答案** (CAGENT_ROLE_AI)。旁白/工具调用/工具输出/思考过程各有
 *    自己的角色样式, 掺进 Markdown 解析只会互相干扰 —— 工具输出里的 ` * #
 *    按字面处理才对。调用方 (do_append) 在切到别的角色前先 md_flush() 落地半行。
 *
 * ② **只认完整行**。流式 delta 会把一行劈成几段、也会把标记劈开 —— 半行留在
 *    行缓冲里, 凑齐整行再解析, 标记永远不会被 delta 边界截断。
 *
 * ③ **纯样式层: 一个字符都不增删**。所有 Markdown 标记字符原样插入, 只是染成
 *    浅灰"淡化"(CommonMark 规定 \* 的 \ 不渲染、[链接](地址) 只渲染文字,
 *    这里都不做删除)。这条是硬约束: 重试回删按 g_stream_chars (实际插入的 cp
 *    数) 定位, 解析时一旦吞字符, 计数就与文本脱节, 回删会留下残尾。
 *
 * ④ CRLF 整条插入。RichEdit 把孤立的 \r 和孤立的 \n 各当成一个段落标记,
 *    拆两次插入会让行距翻倍、后续 run 的样式整体串位 (旧分支实测)。
 *    做法是行尾也进行缓冲, 与行内容作为同一段文本一次插入。
 *
 * 支持范围 ("简单"的边界):
 *   行级  ``` 围栏代码块 / # 标题 / > 引用 / - * + 与 N. 列表 / --- 分割线
 *   行内  **粗体** *斜体* ***粗斜*** `行内码` [文字](链接) \ 反斜杠转义
 * 不做的: 表格 / 任务列表 / 嵌套强调的完整 CommonMark 定界符规则 / HTML 标签。
 */

/* ===== 颜色 =====
 * 灰的下限是铁律: 输出区底色 RGB(247,247,247) 上, 浅于 RGB(110,110,110) 就跌破
 * WCAG AA 的 4.5:1 (见 ui/state.h 的说明)。标记想"更淡"只能靠字号/留白, 不能靠调浅。 */
#define MD_DIM_FG   RGB(110,110,110)   /* Markdown 标记字符: 淡化灰 (与旁白同级) */
#define MD_CODE_FG  RGB( 32, 32, 32)   /* 代码 (围栏内 / 行内码): 深灰 */
#define MD_QUOTE_FG RGB(110,110,110)   /* 引用内容: 灰 */
#define MD_LINK_FG  RGB(  0, 90,200)   /* 链接文字: 蓝 + 下划线 */

/* 引用内容的左缩进 (逻辑像素)。与参数/输出块的缩进同值, 视觉上从属于前文。 */
#define MD_QUOTE_INDENT 16

/* 标题段的额外段前距 (逻辑像素)。h1/h2 多一点, h3 以下少一点。 */
#define MD_HEAD_SP_HI 4
#define MD_HEAD_SP_LO 2

/* 行缓冲上限 (UTF-16 码元)。超过就把已缓冲部分**不解析**直接落地再重新起算 ——
 * 病态的"一行几万字符"(压缩 JSON、长 URL)不该把缓冲撑爆, 代价只是该行的
 * Markdown 解析在截断点处作废, 字符一个不丢。 */
#define MD_LINE_CAP 65536

/* ===== 渲染状态 ===== */
static WCHAR *md_buf;        /* 行缓冲 (只增不缩, 跨回合复用; 进程退出时交还 OS) */
static int    md_len;        /* 已缓冲码元数 */
static int    md_cap;        /* 缓冲容量 */
static int    md_in_fence;   /* 正处于 ``` 围栏代码块内 */
static int    md_fence_len;  /* 开围栏那行的反引号个数 —— 关围栏要求不少于它 */

/* ===== 行内/行级样式 =====
 * 不直接碰 CHARFORMAT: 解析器只描述"这段跑什么样式", 落到 ui_insert_ex 的
 * 显式覆盖 (UiStyleOv) 时统一换算, 两边不重复知道对方细节。 */
enum { MD_F_BOLD = 1, MD_F_ITALIC = 2, MD_F_CODE = 4, MD_F_LINK = 8 };

typedef struct {
    COLORREF fg;
    int bold, italic, mono, underline;
} MdStyle;

/* 标记字符的样式: 淡化灰, 且**一律清零**粗斜等下划线 —— 否则标题里的 ** 会跟着
 * 内容一起加粗, "淡化"就淡了个寂寞。 */
static MdStyle md_dim_style(void) {
    MdStyle s;
    s.fg = MD_DIM_FG;
    s.bold = 0; s.italic = 0; s.mono = 0; s.underline = 0;
    return s;
}

/* 在行级基底上叠加行内标志。code/link 会改文字色 (代码深灰 / 链接蓝),
 * 粗斜只加不清 (行级基底若本身是粗体 —— 标题行, 就保持粗)。 */
static MdStyle md_style_of(const MdStyle *base, int fl) {
    MdStyle s = *base;
    if (fl & MD_F_BOLD)   s.bold = 1;
    if (fl & MD_F_ITALIC) s.italic = 1;
    if (fl & MD_F_CODE) { s.mono = 1; s.fg = MD_CODE_FG; }
    if (fl & MD_F_LINK) { s.fg = MD_LINK_FG; s.underline = 1; }
    return s;
}

/* ===== 落地 =====
 * 把行缓冲的 [s, s+n) 按样式上屏。行缓冲是**可写**堆内存, 就地截断成 C 串再插
 * (与旧分支同法); 调用方保证 s+n 在缓冲范围内。 */
static void md_put(WCHAR *line, int s, int n, const MdStyle *st,
                   int sp_before, int sp_after, int indent) {
    if (n <= 0) return;
    UiStyleOv ov;
    memset(&ov, 0, sizeof(ov));
    ov.has_cf = 1;
    ov.fg = st->fg;
    ov.bold = st->bold; ov.italic = st->italic;
    ov.mono = st->mono; ov.underline = st->underline;
    ov.has_para = 1;
    ov.sp_before = sp_before; ov.sp_after = sp_after; ov.indent = indent;
    WCHAR save = line[s + n];
    line[s + n] = L'\0';
    ui_insert_ex(g_hHistory, line + s, CAGENT_ROLE_AI, -1, &ov);
    line[s + n] = save;
}

/* 缓冲涨爆 / 分配失败时的兜底: 不解析, 按当前状态直接落地。
 * 等宽代码样式在围栏内, 否则正文字体; 段距: 围栏内 0 (多行块), 否则角色默认。
 * 分段用栈缓冲, 不分配内存 —— 兜底路径自己不能再失败。 */
static void md_emit_raw(const WCHAR *w, int n) {
    if (n <= 0) return;
    const UiRoleStyle *rs = ui_role_style(CAGENT_ROLE_AI);
    MdStyle st;
    st.fg = rs->fg; st.bold = rs->bold; st.italic = rs->italic;
    st.mono = rs->mono; st.underline = 0;
    int sp_before = rs->sp_before;
    if (md_in_fence) { st.mono = 1; st.fg = MD_CODE_FG; sp_before = 0; }
    UiStyleOv ov;
    memset(&ov, 0, sizeof(ov));
    ov.has_cf = 1;
    ov.fg = st.fg;
    ov.bold = st.bold; ov.italic = st.italic;
    ov.mono = st.mono; ov.underline = st.underline;
    ov.has_para = 1;
    ov.sp_before = sp_before; ov.sp_after = rs->sp_after; ov.indent = rs->indent;
    WCHAR tmp[512];
    int off = 0;
    while (off < n) {
        int k = n - off;
        if (k > 511) k = 511;
        /* 别把 emoji 代理对劈开: 段尾若是高代理, 少取一个码元 */
        if (off + k < n && w[off + k - 1] >= 0xD800 && w[off + k - 1] <= 0xDBFF) k--;
        memcpy(tmp, w + off, (size_t)k * sizeof(WCHAR));
        tmp[k] = L'\0';
        ui_insert_ex(g_hHistory, tmp, CAGENT_ROLE_AI, -1, &ov);
        off += k;
    }
}

/* ===== 行缓冲 =====
 * 容量恒比 md_len 多至少 1 个码元 —— md_put 要在 [md_len] 处就地写 NUL 做截断
 * (见那里), 不留这一槽就是缓冲末尾越界写。 */
static int md_buf_reserve(int need) {
    if (need <= md_cap) return 1;
    int cap = md_cap ? md_cap : 256;
    while (cap < need) cap *= 2;
    WCHAR *nb = (WCHAR*)realloc(md_buf, (size_t)cap * sizeof(WCHAR));
    if (!nb) return 0;
    md_buf = nb;
    md_cap = cap;
    return 1;
}

/* 追加进行缓冲。超限/分配失败时把**已缓冲**部分原样落地 (字符不丢), 新段由
 * 调用方决定去向 —— md_feed 对超长单段会再走一遍 md_emit_raw。 */
static void md_buf_push(const WCHAR *w, int n) {
    if (n <= 0) return;
    if (md_len + n + 1 > MD_LINE_CAP || !md_buf_reserve(md_len + n + 1)) {
        if (md_len > 0) {
            md_emit_raw(md_buf, md_len);
            md_len = 0;
        }
        if (!md_buf_reserve(n + 1)) { md_emit_raw(w, n); return; }
    }
    memcpy(md_buf + md_len, w, (size_t)n * sizeof(WCHAR));
    md_len += n;
}

/* 复位: 清空行缓冲与围栏状态。**重试回删 / 新分段 / 清空历史框之后必须调** ——
 * 缓冲里是没上屏的半行, 删了文档不清缓冲, 下一段文本会接在它的后面。 */
static void md_reset(void) {
    md_len = 0;
    md_in_fence = 0;
    md_fence_len = 0;
}

/* 行缓冲里有没有未落地的半行 */
static int md_pending(void) { return md_len > 0; }

/* ===== 行级识别的小工具 ===== */

/* 行首空白 (制表符也放行: 模型输出的代码块里常见, 一律当行首處理) */
static int md_skip_ws(const WCHAR *line, int p, int len) {
    while (p < len && (line[p] == L' ' || line[p] == L'\t')) p++;
    return p;
}

/* 空行: 只有空白 (行尾 CRLF 计入缓冲后也在这里放行) */
static int md_is_blank(const WCHAR *line, int len) {
    for (int i = 0; i < len; i++) {
        WCHAR c = line[i];
        if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') return 0;
    }
    return 1;
}

/* 分割线: 行首起 3 个以上同一字符 (- * _), 之间只允许空白。
 * CommonMark 里它的优先级高于列表与强调 (--- 不是缺省的列表, *** 不是粗斜),
 * 所以必须赶在行内扫描之前判。 */
static int md_is_rule(const WCHAR *line, int len) {
    int p = md_skip_ws(line, 0, len);
    if (p >= len) return 0;
    WCHAR c = line[p];
    if (c != L'-' && c != L'*' && c != L'_') return 0;
    int n = 0;
    for (int i = p; i < len; i++) {
        if (line[i] == c) n++;
        else if (line[i] != L' ' && line[i] != L'\t' &&
                 line[i] != L'\r' && line[i] != L'\n') return 0;
    }
    return n >= 3;
}

/* 围栏标记行: 行首起 >= 3 个反引号。返回反引号 run 的起点与长度。 */
static int md_fence_at(const WCHAR *line, int len, int *pp, int *pn) {
    int p = md_skip_ws(line, 0, len);
    int n = 0;
    while (p + n < len && line[p + n] == L'`') n++;
    if (n < 3) return 0;
    *pp = p;
    *pn = n;
    return 1;
}

/* [from, len) 只剩空白 (含行尾 CRLF) —— 关围栏的行尾条件 */
static int md_rest_blank(const WCHAR *line, int from, int len) {
    for (int i = from; i < len; i++) {
        WCHAR c = line[i];
        if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n') return 0;
    }
    return 1;
}

/* 反斜杠后可转义的字符: ASCII 标点 (CommonMark)。两字符都当普通文本,
 * 连 \ 本身也原样渲染 —— 纯样式层的代价, 见文件头第 ③ 条。 */
static int md_escapable(WCHAR c) {
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) ||
           (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

/* ===== 行内扫描 =====
 * 对 [s, e) 做一遍扫描: 普通字符累积成 pending run, 遇到标记就先把 pending
 * 按当前样式落地, 再依次插"标记(淡化) -> 内容(语义样式) -> 闭合标记(淡化)"。
 * **必须按文档顺序插入**: RichEdit 只按调用先后追加, 先把内容插回去再插标记,
 * 文字会串成 "bi***" (旧分支实测)。 */
static void md_scan_inline(WCHAR *line, int s, int e, const MdStyle *base,
                           int flags, int sp_before, int sp_after, int indent) {
    int pend = s;
    int q = s;
    while (q < e) {
        WCHAR c = line[q];

        /* 反斜杠转义: 跳过这两个字符, 不当任何标记 */
        if (c == L'\\' && q + 1 < e && md_escapable(line[q + 1])) {
            q += 2;
            continue;
        }

        /* 链接 [text](url): 文字一段 (蓝 + 下划线), 括号与地址淡化 */
        if (c == L'[') {
            int close = -1, mid = -1, end = -1;
            for (int k = q + 1; k < e; k++) {
                if (line[k] == L']' && close < 0) close = k;
                else if (line[k] == L'(' && close >= 0 && mid < 0) mid = k;
                else if (line[k] == L')' && mid >= 0) { end = k; break; }
            }
            if (end > 0) {
                if (pend < q) {
                    MdStyle cur = md_style_of(base, flags);
                    md_put(line, pend, q - pend, &cur, sp_before, sp_after, indent);
                }
                MdStyle d = md_dim_style();
                MdStyle lk = md_style_of(base, flags | MD_F_LINK);
                md_put(line, q, 1, &d, sp_before, sp_after, indent);                  /* [ */
                md_put(line, q + 1, close - q - 1, &lk, sp_before, sp_after, indent); /* text */
                md_put(line, close, end - close + 1, &d, sp_before, sp_after, indent);/* ](url) */
                q = end + 1;
                pend = q;
                continue;
            }
        }

        /* 行内码 `code`: CommonMark 用"等长反引号 run"闭合 —— ``a`b`` 里
         * 单个反引号属于内容。闭合找不到时整条 run 都当普通字符。 */
        if (c == L'`') {
            int op = 1;
            while (q + op < e && line[q + op] == L'`') op++;
            int k = q + op, found = 0;
            while (k < e) {
                if (line[k] == L'`') {
                    int r = 1;
                    while (k + r < e && line[k + r] == L'`') r++;
                    if (r == op) { found = 1; break; }
                    k += r;
                    continue;
                }
                k++;
            }
            if (found) {
                if (pend < q) {
                    MdStyle cur = md_style_of(base, flags);
                    md_put(line, pend, q - pend, &cur, sp_before, sp_after, indent);
                }
                MdStyle d = md_dim_style();
                MdStyle cd = md_style_of(base, flags | MD_F_CODE);
                md_put(line, q, op, &d, sp_before, sp_after, indent);        /* 开标记 */
                md_put(line, q + op, k - q - op, &cd, sp_before, sp_after, indent); /* code */
                md_put(line, k, op, &d, sp_before, sp_after, indent);        /* 闭标记 */
                q = k + op;
                pend = q;
                continue;
            }
            q += op;   /* 无等长闭合: 整条反引号 run 当普通字符 */
            continue;
        }

        /* 强调 *** 粗斜 / ** 粗体 / * 斜体 (CommonMark: 等长星号 run 闭合) */
        if (c == L'*') {
            /* 三连优先: ***x*** */
            if (q + 2 < e && line[q + 1] == L'*' && line[q + 2] == L'*') {
                int k = q + 3, close = 0;
                while (k + 2 < e) {
                    if (line[k] == L'*' && line[k + 1] == L'*' && line[k + 2] == L'*') {
                        close = 1;
                        break;
                    }
                    k++;
                }
                if (close) {
                    if (pend < q) {
                        MdStyle cur = md_style_of(base, flags);
                        md_put(line, pend, q - pend, &cur, sp_before, sp_after, indent);
                    }
                    MdStyle d = md_dim_style();
                    md_put(line, q, 3, &d, sp_before, sp_after, indent);
                    /* 内部递归扫描: 支持 **粗体套 `行内码`** 这类一层嵌套 */
                    md_scan_inline(line, q + 3, k, base,
                                   flags | MD_F_BOLD | MD_F_ITALIC,
                                   sp_before, sp_after, indent);
                    md_put(line, k, 3, &d, sp_before, sp_after, indent);
                    q = k + 3;
                    pend = q;
                    continue;
                }
                /* 没有 *** 闭合: 退化成普通字符, 交回下面的 ** / * 逻辑 */
            }
            int dbl = (q + 1 < e && line[q + 1] == L'*');
            int open = dbl ? 2 : 1;
            int k = q + open, crun = 0;
            while (k < e) {
                if (line[k] == L'*') {
                    int r = 1;
                    while (k + r < e && line[k + r] == L'*') r++;
                    if (r == open) { crun = r; break; }
                    k += r;
                    continue;
                }
                k++;
            }
            if (crun > 0) {
                if (pend < q) {
                    MdStyle cur = md_style_of(base, flags);
                    md_put(line, pend, q - pend, &cur, sp_before, sp_after, indent);
                }
                MdStyle d = md_dim_style();
                md_put(line, q, open, &d, sp_before, sp_after, indent);
                /* 同上: 内部递归, 让嵌套的行内码/链接在强调里也生效 */
                md_scan_inline(line, q + open, k, base,
                               flags | (dbl ? MD_F_BOLD : MD_F_ITALIC),
                               sp_before, sp_after, indent);
                md_put(line, k, open, &d, sp_before, sp_after, indent);
                q = k + open;
                pend = q;
                continue;
            }
            q++;    /* 无闭合: 当一个普通星符 */
            continue;
        }

        q++;
    }
    if (pend < e) {
        MdStyle cur = md_style_of(base, flags);
        md_put(line, pend, e - pend, &cur, sp_before, sp_after, indent);
    }
}

/* ===== 单行发射 =====
 * line 是行缓冲, len 是整行长度 (含行尾 CRLF); tail_len 是行尾占的码元数
 * (0 = 半行落地, 1 = "\n", 2 = "\r\n")。行内容区间 = [0, len - tail_len)。
 * 行尾**不单独成 run** —— 它是缓冲里最后几个字符, 由本行最后一个 run 一并
 * 插入 (必须与内容同一次 EM_REPLACESEL, 见文件头第 ④ 条)。 */
static void md_emit_line(WCHAR *line, int len, int tail_len) {
    const UiRoleStyle *st = ui_role_style(CAGENT_ROLE_AI);
    MdStyle base;
    base.fg = st->fg;
    base.bold = st->bold;
    base.italic = st->italic;
    base.mono = st->mono;
    base.underline = 0;
    MdStyle dim = md_dim_style();
    int sp_before = st->sp_before, sp_after = st->sp_after, indent = st->indent;

    /* ---- 围栏代码块内 ---- */
    if (md_in_fence) {
        int fp = 0, fn = 0;
        if (md_fence_at(line, len, &fp, &fn) && fn >= md_fence_len &&
            md_rest_blank(line, fp + fn, len)) {
            /* 关围栏标记行: 等宽淡化灰; 段距 0 (它属于这个多行块) */
            md_in_fence = 0;
            md_fence_len = 0;
            MdStyle s = dim;
            s.mono = 1;
            md_put(line, 0, len, &s, 0, 0, indent);
        } else {
            /* 代码行: 等宽深灰, **段距全 0** —— 多行块每行都带段前距会被拉散
             * (同 OUTPUT 角色, 见 ui/state.h 的段距说明); 不行内解析。 */
            MdStyle s = base;
            s.mono = 1;
            s.fg = MD_CODE_FG;
            md_put(line, 0, len, &s, 0, 0, indent);
        }
        return;
    }

    /* ---- 开围栏 ---- */
    {
        int fp = 0, fn = 0;
        if (md_fence_at(line, len, &fp, &fn)) {
            md_in_fence = 1;
            md_fence_len = fn;
            MdStyle s = dim;
            s.mono = 1;
            md_put(line, 0, len, &s, sp_before, sp_after, indent);
            return;
        }
    }

    /* ---- 空行 ---- */
    if (md_is_blank(line, len)) {
        md_put(line, 0, len, &base, sp_before, sp_after, indent);
        return;
    }

    /* ---- 分割线 ---- */
    if (md_is_rule(line, len)) {
        md_put(line, 0, len, &dim, sp_before, sp_after, indent);
        return;
    }

    int p = md_skip_ws(line, 0, len - tail_len);

    /* ---- 标题 # ~ ###### ---- */
    if (p < len - tail_len && line[p] == L'#') {
        int h = 0;
        while (p + h < len - tail_len && line[p + h] == L'#') h++;
        if (h <= 6 && (p + h >= len - tail_len ||
                       line[p + h] == L' ' || line[p + h] == L'\t')) {
            int cs = p + h;
            if (cs < len - tail_len && (line[cs] == L' ' || line[cs] == L'\t')) cs++;
            int ce = len - tail_len;
            /* 结尾的 closing sequence (### 后面那串 #): 前面是空白才认, 一并淡化 */
            int t = ce;
            while (t > cs && (line[t-1] == L' ' || line[t-1] == L'\t')) t--;
            int hs = t;
            while (hs > cs && line[hs-1] == L'#') hs--;
            if (hs < t && (hs == cs || line[hs-1] == L' ' || line[hs-1] == L'\t')) ce = hs;
            int sb = sp_before + (h <= 2 ? MD_HEAD_SP_HI : MD_HEAD_SP_LO);
            MdStyle hs_st = base;
            hs_st.bold = 1;
            /* 前缀从 0 起 (含行首空白): 行级标记与空白一起淡化, 字符才守恒 */
            md_put(line, 0, cs, &dim, sb, sp_after, indent);
            md_scan_inline(line, cs, ce, &hs_st, 0, sb, sp_after, indent);
            /* 收尾 # 序列 + 行尾一起淡化。**行尾必须跟着内容走** (文件头第 ④ 条):
             * 单独插一次 CRLF 会让行距翻倍、后续 run 样式串位。 */
            md_put(line, ce, len - ce, &dim, sb, sp_after, indent);
            return;
        }
    }

    /* ---- 引用 > ---- */
    if (p < len - tail_len && line[p] == L'>') {
        int cs = p + 1;
        if (cs < len - tail_len && (line[cs] == L' ' || line[cs] == L'\t')) cs++;
        MdStyle qs = base;
        qs.fg = MD_QUOTE_FG;
        md_put(line, 0, cs, &dim, sp_before, sp_after, MD_QUOTE_INDENT);
        md_scan_inline(line, cs, len, &qs, 0, sp_before, sp_after, MD_QUOTE_INDENT);
        return;
    }

    /* ---- 列表: - * + 与 N. (后跟空格) ---- */
    {
        int cs = -1;
        int clen = len - tail_len;
        if (p < clen && (line[p] == L'-' || line[p] == L'*' || line[p] == L'+') &&
            p + 1 < clen && (line[p+1] == L' ' || line[p+1] == L'\t')) {
            cs = p + 2;
        } else if (p < clen && line[p] >= L'0' && line[p] <= L'9') {
            int q = p;
            while (q < clen && line[q] >= L'0' && line[q] <= L'9') q++;
            if (q < clen && line[q] == L'.' && q + 1 < clen &&
                (line[q+1] == L' ' || line[q+1] == L'\t'))
                cs = q + 2;
        }
        if (cs >= 0) {
            md_put(line, 0, cs, &dim, sp_before, sp_after, indent);
            md_scan_inline(line, cs, len, &base, 0, sp_before, sp_after, indent);
            return;
        }
    }

    /* ---- 普通段落行 ---- */
    md_scan_inline(line, 0, len, &base, 0, sp_before, sp_after, indent);
}

/* ===== 对外接口 (前向声明见 ui/state.h) ===== */

/* 喂一段 AI 文本。完整行 (以 \n 收尾) 立即解析上屏; 半行进行缓冲等后续 delta。
 * 行尾连同内容一起进缓冲, 保证 CRLF 与内容同一次插入 (第 ④ 条)。 */
static void md_feed(const WCHAR *w) {
    if (!w) return;
    int n = lstrlenW(w);
    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && w[j] != L'\n') j++;
        if (j >= n) {
            /* 半行: 只进缓冲。超长就地把已有的落地 (不解析), 单段本身超长则直接落地 */
            if (md_len + (n - i) + 1 > MD_LINE_CAP) {
                if (md_len > 0) { md_emit_raw(md_buf, md_len); md_len = 0; }
                if (n - i > MD_LINE_CAP) { md_emit_raw(w + i, n - i); break; }
            }
            md_buf_push(w + i, n - i);
            break;
        }
        /* 完整行: 内容 [i, le), 行尾 [le, j] (含可能的 \r) */
        int le = (j > i && w[j-1] == L'\r') ? j - 1 : j;
        int tail_len = j - le + 1;
        if (md_len + (le - i) + tail_len + 1 > MD_LINE_CAP) {
            /* 超长行: 整行 (含行尾) 原样落地。**必须连行尾一起** —— 分开落地的话
             * 行尾会变成下一个独立空段, 凭空多出一个空行。 */
            if (md_len > 0) { md_emit_raw(md_buf, md_len); md_len = 0; }
            md_emit_raw(w + i, (le - i) + tail_len);
            i = j + 1;
            continue;
        }
        md_buf_push(w + i, le - i);
        md_buf_push(w + le, tail_len);
        md_emit_line(md_buf, md_len, tail_len);
        md_len = 0;
        i = j + 1;
    }
}

/* 把缓冲里的半行落地 (不补换行 —— 换行不是模型输出的字符, 加了就破坏第 ③ 条;
 * 段落分隔由 core 的段尾换行/角色切换兜底负责)。返回实际插入的 cp 数,
 * 供调用方计入 g_stream_chars。 */
static long md_flush(void) {
    if (md_len <= 0) return 0;
    long before = ui_text_cp_len(g_hHistory);
    md_emit_line(md_buf, md_len, 0);
    md_len = 0;
    return ui_text_cp_len(g_hHistory) - before;
}
