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

/* 输出区角色样式查表。role 越界(理论上不会)时退回 SYS, 不做越界访问。 */
static const UiRoleStyle *ui_role_style(int role) {
    if (role < 0 || role >= CAGENT_ROLE_COUNT) role = CAGENT_ROLE_SYS;
    return &g_role_style[role];
}

/* 逻辑像素 -> twips (1/20 pt)。段落属性的单位是绝对值, 必须按 g_dpi 换算。
 * 换算口径与 ui_history_parafmt 的行距一致: dp(px) * 1440 / dpi。 */
static LONG ui_twips(int px) {
    return (LONG)((LONGLONG)dp(px) * 1440 / (g_dpi ? g_dpi : 96));
}

/* 段落格式: 段前/段后留白 + 左缩进。作用于**选区所在的段落**, 所以在追加文本前调用,
 * 新文本就落在已经设好格式的段落里。
 * 只写 dwMask 里声明的字段, 因此不会碰 ui_history_parafmt 设的精确行距。 */
/* 段落格式的显式取值版 (逻辑像素)。段前/段后/左缩进由调用方给**绝对值** ——
 * Markdown 渲染 (ui/md.h) 要按行级类型覆盖这几项: 引用缩进、围栏代码行零间距。
 * 只写 dwMask 里声明的字段, 因此不会碰 ui_history_parafmt 设的精确行距。 */
static void ui_para_ex(HWND h, int sp_before, int sp_after, int indent) {
    PARAFORMAT2 pf;
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_SPACEBEFORE | PFM_SPACEAFTER | PFM_STARTINDENT;
    pf.dySpaceBefore = ui_twips(sp_before);
    pf.dySpaceAfter  = ui_twips(sp_after);
    pf.dxStartIndent = ui_twips(indent);
    SendMessageW(h, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

static void ui_para_for_role(HWND h, int role) {
    const UiRoleStyle *st = ui_role_style(role);
    ui_para_ex(h, st->sp_before, st->sp_after, st->indent);
}

/* 文档的字符位置 (cp) 总数。
 * **必须用 EM_GETTEXTLENGTHEX 而不是 GetWindowTextLengthW**: 后者为了兼容 EDIT
 * 会把每个段落标记按 CRLF 算成两个字符, 拿它当 cp 用会偏大 —— 追加时正好被 RichEdit
 * 钳到末尾所以以前没暴露, 但块索引表是**按 cp 定位**的, 差一个字符就全错。
 * GTL_DEFAULT 才是 cp 语义 (段落标记算 1 个)。 */
static long ui_text_cp_len(HWND h) {
    GETTEXTLENGTHEX gtl;
    memset(&gtl, 0, sizeof(gtl));
    gtl.flags = GTL_DEFAULT;
    gtl.codepage = 1200;          /* Unicode */
    return (long)SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
}

/* ===== 显式样式覆盖 =====
 * Markdown 渲染 (ui/md.h) 要在角色表之上指定**绝对的**字符属性与段落属性
 * (标题加粗 / 引用缩进 / 标记淡化……), 而现有调用方一律走角色表。与其在
 * ui_insert_styled 里堆 if-else, 给它一个显式覆盖入口: 插入/兜底/emoji/段落
 * 归属那套骨架原样不动, 变的只是两层样式的**来源**。
 *   has_cf   字符层用绝对值 (fg/bold/italic/mono/underline), 不用角色表
 *   has_para 段落层用绝对值 (sp_before/sp_after/indent), 不用角色表
 * 两个位都清零时行为与 ui_insert_styled 完全一致。 */
typedef struct {
    int has_cf;
    COLORREF fg;
    int bold, italic, mono, underline;
    int has_para;
    int sp_before, sp_after, indent;
} UiStyleOv;

/* 在 pos 处插入一段文本, 返回插入的 UTF-16 码元数。pos < 0 = 追加到末尾。
 * ov = NULL 或全零时等价于 ui_insert_styled (按 g_role_style 走)。
 *
 * **插入点必须落在段落边界上** (紧跟某个 \r\n 之后): 段落格式作用于**整个段落**,
 * 在段落中间插入会把那一段的缩进/留白一起改掉。现有调用点都满足 —— 追加在末尾,
 * 折叠/展开/标题重写也都发生在以换行结尾的整段边界上; md.h 的行发射同样
 * 一行一批, 批批落在段边界上 (行尾 CRLF 与内容同一次插入)。
 *
 * 排版分两层, 别混:
 *   段落层 (ui_para_for_role / ui_para_ex) —— 段前/段后留白、左缩进, 决定"块与块之间的距离";
 *   字符层 (下面按角色/覆盖构造的 CHARFORMAT2W) —— 颜色、粗斜体、等宽、下划线、底色。 */
static long ui_insert_ex(HWND h, const WCHAR *w, int role, long pos, const UiStyleOv *ov) {
    int total = w ? lstrlenW(w) : 0;
    if (total <= 0) return 0;
    long doc_len = ui_text_cp_len(h);
    if (pos < 0) pos = doc_len;

    /* 空文档时吃掉开头的换行。core 习惯在每条消息前加 \r\n 做分隔, 但**文档开头不需要**:
     * 那个空段落自身就占一整行高, 再叠上该角色的段前距, 输出区顶部会凭空多出一大块空白。 */
    if (doc_len == 0) {
        while (total > 0 && (*w == L'\r' || *w == L'\n')) { w++; total--; }
        if (total <= 0) return 0;
    }

    /* **中间插入会顺带改掉"后一段"的段落格式。** RichEdit 把落在段边界上的插入点算作
     * **后一段**, 所以下面那次 EM_SETPARAFORMAT 会连后一段的缩进/留白一起改掉。
     * 症状很隐蔽: 展开上一个折叠块之后, 下一个块的标题行莫名被缩进 (实测 indent 从 0
     * 变成 240 twips), 而它自己什么都没做错。先把后一段的格式存下来, 插完再写回。 */
    PARAFORMAT2 saved;
    int restore = (pos > 0 && pos < doc_len);
    if (restore) {
        memset(&saved, 0, sizeof(saved));
        saved.cbSize = sizeof(saved);
        SendMessageW(h, EM_SETSEL, pos, pos);
        SendMessageW(h, EM_GETPARAFORMAT, 0, (LPARAM)&saved);
    }

    SendMessageW(h, EM_SETSEL, pos, pos);

    /* 当前插入点的字符格式 (字体/字号) 作为普通文本基底 */
    CHARFORMAT2W base;
    memset(&base, 0, sizeof(base));
    base.cbSize = sizeof(base);
    SendMessageW(h, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&base);

    /* 按角色叠加样式。注意必须显式清掉 CFE_AUTOCOLOR —— 它随插入点基底带来, 置位时
     * RichEdit 会忽略 crTextColor, 颜色设置将完全无效 (症状: 所有文字都是系统黑)。
     * 同理清 CFE_AUTOBACKCOLOR: 不设底色的角色要让底色跟随控件背景, 设底色的角色
     * 必须先把"自动"位摘掉, 否则 crBackColor 同样会被忽略。
     * CFE_UNDERLINE 也一并清: 插入点基底可能带着上一个 run 的下划线 (Markdown 链接
     * 之后紧跟普通文本), 不清就会"下划线永远洗不掉"。
     * **刻意不设 CFM_SIZE / yHeight** —— 字号一律继承控件的默认字体。
     * 踩过的两个坑, 都是"给 run 写死字号"引出来的:
     *   ① CHARFORMAT.yHeight 是 em 高, 而 WM_SETFONT 的 CreateFontW(h) 是**字身格高**;
     *      YaHei 的 格/em ≈ 1.27, 于是写死 yHeight 的字比默认字体大 27%,
     *      而行距是按默认字体的 tmHeight 算的 -> 字被行距削顶(实测削掉 10px)。
     *   ② WM_SETFONT 会让 RichEdit 丢掉 run 上的显式字号 —— 一旦跨显示器触发
     *      WM_DPICHANGED 里那轮 ui_fonts_rebuild, 文字会从"大"跳回"默认", 看着像换了套排版。
     * 缩放输出区字号请走 EM_SETZOOM(见 wndproc.h 的 ui_history_zoom), 它不碰字符格式。 */
    const UiRoleStyle *st = ui_role_style(role);
    CHARFORMAT2W role_cf = base;
    role_cf.dwMask    |= CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_FACE | CFM_BACKCOLOR
                         | CFM_UNDERLINE;
    role_cf.dwEffects &= ~(CFE_AUTOCOLOR | CFE_AUTOBACKCOLOR | CFE_BOLD | CFE_ITALIC
                           | CFE_UNDERLINE);
    if (ov && ov->has_cf) {
        /* 显式覆盖: 全套绝对值 (MdStyle 落过来的), 不再查角色表。
         * **底色一律走 CFE_AUTOBACKCOLOR**: ov 没有底色字段, 而 dwMask 里含
         * CFM_BACKCOLOR (构造 role_cf 时加的), 不置自动位就会把 base 里零值的
         * crBackColor (= 黑) 应用上去 —— Markdown 的等宽 run (行内码/围栏内容)
         * 整块发黑正是这么来的 (preview 实测)。 */
        role_cf.crTextColor = ov->fg;
        role_cf.dwEffects |= CFE_AUTOBACKCOLOR;
        if (ov->bold)      role_cf.dwEffects |= CFE_BOLD;
        if (ov->italic)    role_cf.dwEffects |= CFE_ITALIC;
        if (ov->underline) role_cf.dwEffects |= CFE_UNDERLINE;
        wcscpy(role_cf.szFaceName, ov->mono ? CAGENT_MONO_FACE : CAGENT_UI_FACE);
    } else {
        role_cf.crTextColor = st->fg;
        if (st->bold)      role_cf.dwEffects |= CFE_BOLD;
        if (st->italic)    role_cf.dwEffects |= CFE_ITALIC;
        if (st->underline) role_cf.dwEffects |= CFE_UNDERLINE;
        if (st->has_bg) role_cf.crBackColor = st->bg;
        else            role_cf.dwEffects  |= CFE_AUTOBACKCOLOR;   /* 跟随输出区背景 */
        wcscpy(role_cf.szFaceName, st->mono ? CAGENT_MONO_FACE : CAGENT_UI_FACE);
    }

    /* 一次性插入整串, 再按 run 覆盖字体。
     * 不用"分段插入"的写法是因为那需要给每段就地补 NUL, 而 w 可能是字符串字面量
     * (只读段, 写进去直接崩)。先整串插入 + 后置格式, 同样满足"不同字体分段"的需求。 */
    SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&role_cf);
    SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)w);

    /* 连续 emoji 代理对为一段, 换成 Segoe UI Emoji (YaHei 无字形, 字体绑定回退会渲染成 '?') */
    int i = 0;
    while (i < total) {
        BOOL emo = (w[i] >= 0xD800 && w[i] <= 0xDBFF &&
                    w[i+1] >= 0xDC00 && w[i+1] <= 0xDFFF);
        int end = i;
        if (emo) {
            while (end + 1 < total && w[end] >= 0xD800 && w[end] <= 0xDBFF &&
                   w[end+1] >= 0xDC00 && w[end+1] <= 0xDFFF) end += 2;
        } else {
            while (end < total && !(w[end] >= 0xD800 && w[end] <= 0xDBFF &&
                                  end + 1 < total && w[end+1] >= 0xDC00 && w[end+1] <= 0xDFFF)) end++;
        }
        if (emo) {
            CHARFORMAT2W cf = role_cf;
            cf.dwMask |= CFM_FACE;
            wcscpy(cf.szFaceName, L"Segoe UI Emoji");
            SendMessageW(h, EM_SETSEL, pos + i, pos + end);
            SendMessageW(h, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        }
        i = end;
    }

    /* **段落格式按"实际插入范围"设, 不能按"插入点所在段落"设。**
     * 插入点落在文档末尾时, RichEdit 把它算作最后一段落之内 —— 末尾那个段落标记不计入
     * cp 长度, 于是设置会落到**上一段**上。症状很有迷惑性: 参数行的缩进丢了、紧跟其后
     * 的第一条旁白反而缩进了, 而再往后几条又是好的 (插入点这时才真正落在新段落里)。
     * 选中刚插入的范围再设, 段落归属就没有歧义。 */
    long added = ui_text_cp_len(h) - doc_len;
    if (added > 0) {
        SendMessageW(h, EM_SETSEL, pos, pos + added);
        if (ov && ov->has_para) ui_para_ex(h, ov->sp_before, ov->sp_after, ov->indent);
        else                    ui_para_for_role(h, role);
    }

    /* 把后一段的段落格式写回去 (见上面 restore 的说明)。位置用**实际 cp 增量**算,
     * 不能用 total —— 段落标记在两种口径下长度不同。 */
    if (restore) {
        SendMessageW(h, EM_SETSEL, pos + added, pos + added);
        SendMessageW(h, EM_SETPARAFORMAT, 0, (LPARAM)&saved);
    }
    return total;
}

/* 按角色表插入 (绝大多数调用方走这个; Markdown 等需要显式覆盖的走 ui_insert_ex)。 */
static long ui_insert_styled(HWND h, const WCHAR *w, int role, long pos) {
    return ui_insert_ex(h, w, role, pos, NULL);
}

/* 读回 [start,end) 的文本 (UTF-16, 调用方负责 free)。失败返回 NULL。
 * 注意 RichEdit 的 EM_GETTEXTRANGE 把段落标记还原成**单个 CR** —— 所以读回的长度
 * 可能小于 end-start (文档里是 CRLF)。重新插回时单个 CR 同样能分段, 但**调用方必须
 * 用返回的字符串长度而不是原来的范围长度**去更新位置索引。 */
static WCHAR *ui_get_range(HWND h, long start, long end) {
    if (end <= start) return NULL;
    long n = end - start;
    WCHAR *buf = (WCHAR*)malloc((size_t)(n + 1) * sizeof(WCHAR));
    if (!buf) return NULL;
    TEXTRANGEW tr;
    tr.chrg.cpMin = start;
    tr.chrg.cpMax = end;
    tr.lpstrText = buf;
    LONG got = (LONG)SendMessageW(h, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    if (got < 0) { free(buf); return NULL; }
    buf[got] = L'\0';
    return buf;
}

/* 用带样式的新文本替换 [start,end) 范围, 返回新文本的码元数 (删除后为空则 0)。
 * 折叠 (替换成空) / 展开 (插回正文) / 标题重写 (替换单个状态符号) 都走它。 */
static long ui_replace_styled(HWND h, long start, long end, const WCHAR *w, int role) {
    if (end > start) {
        SendMessageW(h, EM_SETSEL, start, end);
        SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }
    if (!w || !*w) return 0;
    return ui_insert_styled(h, w, role, start);
}

/* ===== 可折叠块 (思考过程 / 工具调用) =====
 * 数据与契约见 ui/state.h 的 UiFoldBlock。这里只放操作。 */

/* 释放一个块持有的所有正文副本。三个丢弃路径 (reset / drop_oldest / trim_from) 共用 ——
 * 收起态的块把正文 malloc 在 segs[].text 里, 漏一条就是一次内存泄漏。 */
static void ui_fold_free_block(UiFoldBlock *b) {
    for (int s = 0; s < b->n_segs; s++) { free(b->segs[s].text); b->segs[s].text = NULL; }
    b->n_segs = 0;
}

static void ui_fold_reset(void) {
    for (int i = 0; i < g_fold_n; i++) ui_fold_free_block(&g_folds[i]);
    g_fold_n = 0;
}

/* 新块的标题行刚插入: cp_before/cp_after 是插入前后的文档 cp 长度。
 * head_cp 在这里**实测**而不是写死常量 —— 工具块的标题行长度随工具名变化,
 * 而且"一个段落标记算几个 cp"这件事 RichEdit 各版本口径不一, 实测比猜可靠:
 * 这个值错了, 该块正文的起点就全偏。 */
/* 丢掉**最老的**一个块 (索引表满了的时候)。三条丢弃路径都走 ui_fold_free_block 释放正文。
 *
 * 两条写进注释的边界 (都是可接受的权衡, 但必须写明):
 * ① 点击被丢块的标题行会命中**相邻剩余块**: 命中测试按"最近行"取块, 被丢块的标题还
 *    在文档里但已不在索引里, 点它会把旁边那块切换了。旧行为是命中它自己 —— 概率低
 *    (只发生在滚回几屏之前、且正好点到被丢块的标题), 但行为确实变了。
 * ② 被丢块若是**展开态** (用户特意点开过、user_toggled), 它的正文会永久摊开且不可再
 *    收。"老块通常已是收起态"只是大概率, 不是保证 —— 滚回去把早期块展开再聊长会话,
 *    就可能踩到。 */
static void ui_fold_drop_oldest(void) {
    if (g_fold_n <= 0) return;
    ui_fold_free_block(&g_folds[0]);
    memmove(g_folds, g_folds + 1, (size_t)(g_fold_n - 1) * sizeof(UiFoldBlock));
    g_fold_n--;
}

static void ui_fold_new(int kind, long cp_before, long cp_after) {
    /* 表满时**挤掉最老的**, 而不是把新块丢掉 —— 新块在文档末尾, 正是用户正在看、也最
     * 想收起的区域; 丢它等于"会话聊到一定长度之后就再也折不动了", 而且是静默失效:
     * 界面上标题行照常显示、点上去却没反应。老块此时通常已是收起态, 丢掉索引只是不能
     * 再点开 (它在好几屏之前, 且视觉上没变化)。 */
    if (g_fold_n >= UI_FOLD_MAX) ui_fold_drop_oldest();
    UiFoldBlock *b = &g_folds[g_fold_n++];
    memset(b, 0, sizeof(*b));
    b->kind    = kind;
    b->cp_head = cp_before;
    b->head_cp = cp_after - cp_before;
}

/* 正文追加后延长当前块。只有属于该块的角色才算数:
 *   思考块收 THINK, 工具块收 ARGS / OUTPUT。
 * 末尾同角色就并进上一段, 否则开一段新的 —— 展开时要按段重放, 才能保住
 * "参数行是等宽灰、输出块是等宽灰底"这个区别。 */
static void ui_fold_extend(int role, long cp_added) {
    if (g_fold_n <= 0 || cp_added <= 0) return;
    UiFoldBlock *b = &g_folds[g_fold_n - 1];
    if (b->collapsed) return;          /* 收起态下不该再有正文追加 (流式中已禁用折叠) */
    int takes = (b->kind == UI_FOLD_THINK && role == CAGENT_ROLE_THINK)
             || (b->kind == UI_FOLD_TOOL  && (role == CAGENT_ROLE_ARGS ||
                                              role == CAGENT_ROLE_OUTPUT));
    if (!takes) return;
    if (b->n_segs > 0 && b->segs[b->n_segs - 1].role == role) {
        b->segs[b->n_segs - 1].len += cp_added;
    } else if (b->n_segs < UI_FOLD_SEG_MAX) {
        b->segs[b->n_segs].role = role;
        b->segs[b->n_segs].len  = cp_added;
        b->segs[b->n_segs].text = NULL;
        b->n_segs++;
    } else {
        return;
    }
    /* body_len 是**各段之和**, 不是"从正文起点到文档末尾"。正文中间可能插进不属于本块的
     * 内容 (典型是限流重试时那几条旁白), 拿文档末尾去减会把它们的长度也算进来 —— 于是
     * "各段之和 != body_len" 的校验永远通不过, 整个块就折不动了 (症状是"点了没反应")。 */
    b->body_len = 0;
    for (int s = 0; s < b->n_segs; s++) b->body_len += b->segs[s].len;
}

/* 丢弃 cp 起点在 cp 之后的块: 重试回删把它们的正文删了, 位置不再可信。 */
static void ui_fold_trim_from(long cp) {
    while (g_fold_n > 0 && g_folds[g_fold_n - 1].cp_head >= cp) {
        UiFoldBlock *b = &g_folds[--g_fold_n];
        ui_fold_free_block(b);
    }
}

/* 命中测试: pt 是否落在某个块的标题行上, 返回块下标, 没有则 -1。
 *
 * 做法是取标题行首字符的坐标, 再判断 pt 与哪一行最接近 —— **不用 EM_CHARFROMPOS**:
 * 它返回值里的字符索引只有 16 位, 文档超过 65535 字符就截断了, 而这里的对话可以很长。
 * 行高直接取 ui_history_parafmt 设的那个精确行距 —— 它不随字体走, 但会随 EM_SETZOOM 缩放,
 * 所以用 dp(g_hist_lpx + HIST_LINE_EXTRA)。
 *
 * **纵向放宽到一个行高**: 标题行本身只有一行, 200% 下也就 38px, 鼠标稍微偏一点就点空了。
 * 放宽后落在"标题行上下各一行"以内都算命中, 取**最近的那一行**。参数行参与折叠后,
 * 相邻两个收起的标题行可能紧挨着, 落点在两者正中时取最近的 —— 行为确定, 不会两头跳。
 * 横向不设限: 标题行整行可点, 不用精确戳到字上。 */
static int ui_fold_hit(POINTL pt) {
    if (g_fold_n <= 0) return -1;
    long line_h = dp(g_hist_lpx + HIST_LINE_EXTRA);
    if (line_h <= 0) return -1;
    long half = line_h / 2;
    int  best = -1;
    long best_d = 0x7FFFFFFF;
    for (int i = 0; i < g_fold_n; i++) {
        POINTL p;
        memset(&p, 0, sizeof(p));
        if (SendMessageW(g_hHistory, EM_POSFROMCHAR, (WPARAM)&p, (LPARAM)g_folds[i].cp_head) == -1)
            continue;
        long d = (long)pt.y - (p.y + half);      /* 与标题行垂直中心的距离 */
        if (d < 0) d = -d;
        if (d <= line_h && d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* 标题行首字符的展开/收起符号 —— **必须与 core 的 CAGENT_FOLD_GLYPH_OPEN 一致**
 * (那边是窄字符串 UTF-8, 这边是宽字符; C 没有 UTF-8->UTF-16 字面量转换, 只能两份手写)。
 * 两个都是单个 UTF-16 码元, 替换后标题行长度不变; 长度一变, 正文起点和后续块的位置
 * 就全得重算。收起态只存在于前端 —— core 发标题行时永远是展开态。 */
#define UI_FOLD_GLYPH_OPEN  L"▾"
#define UI_FOLD_GLYPH_SHUT  L"▸"

/* 正文起点 = 标题行之后。**参数行与输出块一起折叠** —— 收起后只剩一行"▸ 调用工具 X",
 * 展开时逐段按原角色还原 (ARGS 与 OUTPUT 样式不同)。早期版本参数行保留过一版,
 * 实测用户明确要求参数也收进去: 收起的本意就是"过程细节先不看"。 */
static long ui_fold_cut_start(const UiFoldBlock *b) {
    return b->cp_head + b->head_cp;
}

/* 把所有块的标题行按各自角色重设一遍段落格式。
 *
 * 为什么需要这个"兜底重设": 折叠/展开都是在**段边界**上插入或删除内容, 而 RichEdit 对
 * "插入点落在段边界时算作哪一段"的处理会让相邻段落的缩进/留白被带跑 —— 实测展开第 N 块
 * 之后, 第 N+1 块的标题行缩进从 0 变成 240 twips (正是输出块的缩进值)。
 * ui_insert_styled 里已经存/恢复了一道, 但规则太绕、容易漏; 块数有上限 (UI_FOLD_MAX,
 * 满了挤掉最老的, 见 ui_fold_new), 直接按角色
 * 把标题行重设一遍, 比逐处推演 RichEdit 的继承行为可靠得多。
 * 只动 SPACEBEFORE/SPACEAFTER/STARTINDENT (见 ui_para_for_role), 不碰行距。 */
static void ui_fold_refresh_heads(void) {
    for (int i = 0; i < g_fold_n; i++) {
        int role = (g_folds[i].kind == UI_FOLD_THINK) ? CAGENT_ROLE_THINK_HEAD
                                                      : CAGENT_ROLE_TOOL;
        SendMessageW(g_hHistory, EM_SETSEL, g_folds[i].cp_head, g_folds[i].cp_head);
        ui_para_for_role(g_hHistory, role);
    }
}

/* 切换第 idx 块的收起/展开 —— 只改文档, **不重画**。返回 1 表示状态真的变了
 * (调用方需要重画), 0 表示目标状态已是现状、什么都没动。
 *
 * 三个必须守住的点:
 *   ① 折叠范围两端都落在段落边界上 (core 保证各段以换行收尾), 否则删完会把相邻段落并到一起;
 *   ② 读回的正文长度可能**小于**原 cp 长度 (EM_GETTEXTRANGE 把 CRLF 还原成单个 CR),
 *      所以展开后要用实际插入量回写每段长度, 并用同一个 delta 更新后续块的位置;
 *   ③ 标题状态符号只替换**一个字符**, 长度不变, 所以标题自身不引起位置漂移。
 *
 * 为什么单独拆出来不做重画: 一路都是 EM_REPLACESEL / EM_SETPARAFORMAT, 末尾还有
 * ui_fold_refresh_heads 给**每个**块的标题行重设格式 —— 每步都画的话, 用户看到的是
 * 内容一段段刷出来。重画由调用方 (ui_fold_set / ui_fold_toggle) 在序列末尾统一收尾。 */
static int ui_fold_apply(int idx, int collapse) {
    if (idx < 0 || idx >= g_fold_n) return 0;
    UiFoldBlock *b = &g_folds[idx];
    if (b->collapsed == !!collapse) return 0;    /* 已经是目标状态 */
    long cut_start = ui_fold_cut_start(b);
    long delta;

    if (collapse) {
        /* 折叠范围 = 整个正文 (参数行 + 输出块); 长度就是各段长度之和, 不需要再和
         * body_len 对账 —— body_len 本身就是它们的和 (见 ui_fold_extend)。 */
        long cut_len = 0;
        for (int s = 0; s < b->n_segs; s++) cut_len += b->segs[s].len;
        if (cut_len <= 0) return 0;               /* 没有可收的内容 (空块) */
        long pos = cut_start;
        for (int s = 0; s < b->n_segs; s++) {
            b->segs[s].text = ui_get_range(g_hHistory, pos, pos + b->segs[s].len);
            if (!b->segs[s].text) {
                for (int k = 0; k < s; k++) { free(b->segs[k].text); b->segs[k].text = NULL; }
                return 0;
            }
            pos += b->segs[s].len;
        }
        ui_replace_styled(g_hHistory, cut_start, cut_start + cut_len, NULL, 0);
        delta = -cut_len;
        b->collapsed = 1;
        ui_replace_styled(g_hHistory, b->cp_head, b->cp_head + 1,
                          UI_FOLD_GLYPH_SHUT, b->kind == UI_FOLD_THINK ? CAGENT_ROLE_THINK_HEAD
                                                                       : CAGENT_ROLE_TOOL);
    } else {
        long pos = cut_start, total = 0;
        for (int s = 0; s < b->n_segs; s++) {
            long n = ui_replace_styled(g_hHistory, pos, pos, b->segs[s].text, b->segs[s].role);
            free(b->segs[s].text);
            b->segs[s].text = NULL;
            b->segs[s].len  = n;      /* 用实际插入量回写, 见上面第 ② 条 */
            pos   += n;
            total += n;
        }
        /* 展开后按段重算 body_len (上面已把 segs[].len 回写成实际插入量) */
        b->body_len = 0;
        for (int s = 0; s < b->n_segs; s++) b->body_len += b->segs[s].len;
        delta = total;
        b->collapsed = 0;
        ui_replace_styled(g_hHistory, b->cp_head, b->cp_head + 1,
                          UI_FOLD_GLYPH_OPEN, b->kind == UI_FOLD_THINK ? CAGENT_ROLE_THINK_HEAD
                                                                      : CAGENT_ROLE_TOOL);
    }
    for (int j = idx + 1; j < g_fold_n; j++) g_folds[j].cp_head += delta;
    ui_fold_refresh_heads();     /* 兜底: 段边界插入会把相邻段的缩进带跑, 见上面的说明 */
    return 1;
}

/* 变更序列结束后的一次性重画 —— **不带 erase**。RichEdit 是子窗口, 会自己处理
 * WM_ERASEBKGND 之前的背景 (实测不擦背景时 WM_PAINT 出来的角落底色仍是 HIST_BG),
 * 所以这里直接 RDW_INVALIDATE | RDW_UPDATENOW: 同步画一帧, 且不经过"整屏擦成背景色"
 * 的空窗期 —— 那正是折叠/缩放时闪一下的根源。 */
static void ui_fold_paint_once(void) {
    if (!g_hHistory) return;
    RedrawWindow(g_hHistory, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

/* 折叠/展开 + 重画的完整路径: 整段文档变更包在 WM_SETREDRAW(FALSE) 里, 末尾一次性
 * 同步重画。中间态的 EM_REPLACESEL / EM_SETPARAFORMAT 一步都不会上屏, 用户看到的是
 * 旧状态直接过渡到新状态, 没有逐段刷出来的过程, 也没有擦背景的空白帧。 */
static void ui_fold_set(int idx, int collapse) {
    if (idx < 0 || idx >= g_fold_n) return;
    SendMessageW(g_hHistory, WM_SETREDRAW, FALSE, 0);
    int changed = ui_fold_apply(idx, collapse);
    SendMessageW(g_hHistory, WM_SETREDRAW, TRUE, 0);
    if (changed) ui_fold_paint_once();
}

/* 用户点击切换: 记下"这个块被手动动过", 之后不再自动折叠 (见 state.h 的 user_toggled)。
 *
 * **切换前后要把滚动位置原样恢复** —— RichEdit 在文档长度变化时会自己挪滚动条, 折叠后
 * 整屏内容上移、展开后被推下去, 而用户的视线就落在刚点的那一行上, 于是"点一下整个视图
 * 都跳了"。恢复滚动位置的效果正是想要的: **标题行钉在原地, 只有它下面的内容伸缩**。
 * 自动折叠走 ui_fold_collapse, 那条路径希望视图跟着滚到底, 所以这层只包在点击路径上。 */
static void ui_fold_toggle(int idx) {
    if (idx < 0 || idx >= g_fold_n) return;
    g_folds[idx].user_toggled = 1;
    POINT scroll;
    memset(&scroll, 0, sizeof(scroll));
    /* 取滚动位、改文档、恢复滚动位三步必须包在同一对 WM_SETREDRAW 里: 折叠路径的重绘
     * 本就发生在恢复滚动位那一步 (RichEdit 文档长度变化后会自己挪一次滚动条), 把它
     * 留在批处理外面就相当于又多一次独立重画 —— 批处理白做, 还是会闪。 */
    SendMessageW(g_hHistory, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_hHistory, EM_GETSCROLLPOS, 0, (LPARAM)&scroll);
    ui_fold_apply(idx, !g_folds[idx].collapsed);
    SendMessageW(g_hHistory, EM_SETSCROLLPOS, 0, (LPARAM)&scroll);
    SendMessageW(g_hHistory, WM_SETREDRAW, TRUE, 0);
    ui_fold_paint_once();
}

/* 按"默认折叠"收起第 idx 块 (块结束时调用)。用户手动展开过的不动 —— 别把人家特意
 * 展开的那个又按回去。 */
static void ui_fold_collapse(int idx) {
    if (idx < 0 || idx >= g_fold_n) return;
    if (g_folds[idx].user_toggled) return;
    ui_fold_set(idx, 1);
}

/* 一轮对话 / 一次历史回放结束后的收尾: Markdown 半行落地, 最后一个块按默认收起,
 * 滚回末尾。
 * **实时与回放必须走同一条收尾** (回放与实时同构): 实时路径由 gui_on_done 投递
 * WM_APP_FOLDEND 触发; 回放路径以前漏了这一步, 于是同一段对话只因"重新载入"终态
 * 就不一样 —— 实时里最后一个工具块是收起的, 从会话列表加载回来它却摊开着。 */
static void ui_turn_tail(void) {
    g_stream_chars += md_flush();
    ui_fold_collapse(g_fold_n - 1);
    SendMessageW(g_hHistory, WM_VSCROLL, SB_BOTTOM, 0);
}

/* 主线程处理: UTF-8 -> UTF-16, LF 补 CRLF, 按角色着色/缩进后追加到历史框末尾。 */
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

    /* 空内容直接返回: 它没有可显示的东西, 却会带走一整套副作用 —— 角色切换触发
     * md_flush() 把正在攒的 AI 半行落地、下面的兜底再补一个换行。两者都会把半行从句子
     * 中间切断, 用户看到的就是"正式回答每行都是半句"。服务端把"本片没内容"表达成空串
     * delta 时就会这样 (core 已按空串=没有处理, 这里兜住其余来源)。 */
    if (!wbuf[0]) { free(wbuf); return; }

    /* Markdown: 只有正式答案 (AI 角色) 走行缓冲渲染。切到别的角色前, 先把缓冲里的
     * 半行落地 —— 它属于刚结束的 AI 正文, 不落地就会在换角色后丢掉。
     * **必须赶在下面的兜底换行之前**: 半行是上一段 AI 正文的延续, 分隔换行属于
     * 它和新角色之间; 顺序反了会把半行挤成一个新段 (中间多一个空行)。
     * NOTE 是流式中的短暂插播 (限流重试旁白), 围栏状态要**跨过它**: 在围栏中间
     * 插一条旁白, 回来接着出的还是代码 (若借此把围栏关掉, 闭合行反而会再开一个)。
     * 其余角色意味着这个 AI 分段真的结束了, 未闭合的围栏不该泄漏到下一段。 */
    if (role != CAGENT_ROLE_AI) {
        if (md_pending()) g_stream_chars += md_flush();
        if (role != CAGENT_ROLE_NOTE) md_reset();
    }

    /* 兜底: 角色切换而上一段没写完时先补个换行 (理由见 ui/state.h 的 g_last_no_lf) */
    if (g_last_role >= 0 && role != g_last_role && g_last_no_lf) {
        long len = ui_text_cp_len(g_hHistory);
        SendMessageW(g_hHistory, EM_SETSEL, len, len);
        SendMessageW(g_hHistory, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    }

    /* 新块开始 = 上一个块到此结束, 按"默认折叠"把它收起来。
     * **必须在插入新标题行之前做**: 折叠会让后面所有块的位置前移, 插完再折就会把刚记下的
     * cp_head 弄错 —— 之后所有命中测试和展开都跟着偏。 */
    if (role == CAGENT_ROLE_THINK_HEAD || role == CAGENT_ROLE_TOOL)
        ui_fold_collapse(g_fold_n - 1);

    long cp_before = ui_text_cp_len(g_hHistory);
    if (role == CAGENT_ROLE_AI) md_feed(wbuf);      /* 行缓冲: 完整行即时解析, 半行等后续 */
    else                        ui_insert_styled(g_hHistory, wbuf, role, -1);
    long cp_after = ui_text_cp_len(g_hHistory);
    long cp_added = cp_after - cp_before;

    /* 折叠块索引: 标题行开一个新块, 属于当前块的正文延长它。
     * THINK_HEAD 开思考块 (正文是 THINK 角色), TOOL 开工具块 (正文是参数行 + 输出块)。
     * 工具块对所有模型都有用 —— 工具输出经常几十上百行, 折起来界面才清爽。 */
    if (role == CAGENT_ROLE_THINK_HEAD)   ui_fold_new(UI_FOLD_THINK, cp_before, cp_after);
    else if (role == CAGENT_ROLE_TOOL)    ui_fold_new(UI_FOLD_TOOL, cp_before, cp_after);
    else                                  ui_fold_extend(role, cp_added);

    SendMessageW(g_hHistory, EM_SCROLLCARET, 0, 0);
    SendMessageW(g_hHistory, WM_VSCROLL, SB_BOTTOM, 0);   /* 强制滚到末尾 */
    /* 供重试时回删本分段: 记的是**实际增加的 cp 数**, 不是 wbuf 的码元数 —— 回删按 cp
     * 定位, 两个口径混用会删多或删少 (段落标记在两种口径下长度不同)。
     * 思考内容也必须算进去, 否则重试时旧思考块会留在界面上跟新的一起叠着。 */
    if (role == CAGENT_ROLE_AI || role == CAGENT_ROLE_THINK || role == CAGENT_ROLE_THINK_HEAD)
        g_stream_chars += cp_added;
    /* AI 的"是否以换行结尾"要看**行缓冲**: 半行在缓冲里 = 这段还没写完,
     * 尽管最后一个完整行可能以 \n 收尾 (g_last_no_lf 描述的是文本流, 不是文档)。 */
    g_last_role  = role;
    g_last_no_lf = (role == CAGENT_ROLE_AI)
                 ? md_pending()
                 : (wbuf[0] && wbuf[lstrlenW(wbuf) - 1] != L'\r' &&
                    wbuf[lstrlenW(wbuf) - 1] != L'\n');
    free(wbuf);
}
