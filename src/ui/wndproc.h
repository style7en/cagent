/*
 * ui/wndproc.h - 布局 / 输入框子类 / 主窗口过程 / 历史框子类
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* ===== 布局 / 子类化 / 窗口过程 ===== */

/* 输出区的内部留白: RichEdit 的文字默认**贴着控件边缘**, 顶到边框看着很局促。
 * 用 EM_SETRECT(格式化矩形) 而不是 EM_SETMARGINS —— 后者只能设左右, 上下设不了。
 * 矩形是相对客户区的, 所以尺寸一变就得重算, 调用点都在布局之后。 */
static void ui_history_pad(HWND h, int w, int hh) {
    if (!h) return;
    RECT rc;
    rc.left   = dp(10);
    rc.top    = dp(8);
    rc.right  = w  - dp(10);
    rc.bottom = hh - dp(8);
    SendMessageW(h, EM_SETRECT, 0, (LPARAM)&rc);
    /* 同样不带 erase: EM_SETRECT 会全文档重排, 重排后重画一帧即可, 擦背景只会添空窗 */
    RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

static void layout(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    /* 以下全部按 96 DPI 逻辑像素书写, 经 dp() 换算成物理像素 (见 ui/dpi.h)。
     * 调尺寸请改这些逻辑值, 不要再往布局里塞裸像素数字。 */
    int gap = dp(8);
    /* 行高 22 是**算出来的**, 不是随手定的:
     * 单行 EDIT 没有垂直对齐的开关(没有对应 ES_* 样式, 也没有 EM_* 消息), 文字由系统锚在
     * "距控件顶约 0.28×字号" 处, 余下的空间全留在下方 —— 行高给大了就看着偏上。
     * 实测(200%, 16 逻辑像素字号): 字形高 26px、锚点 9px, 行高 52 时是 9/17(偏上);
     * 要让上下相等需要 9+26+9 = 44px = dp(22)。锚点与字形高都随字号等比缩放,
     * 所以这个 22 在任何 DPI 下都成立。 */
    int row_h = dp(22);
    int lbl_w = dp(80);
    int top_h = row_h * 4 + gap * 5;
    int btn_w = dp(80);
    int ws_btn_w = dp(72);
    int new_w = dp(80);
    int sess_w = dp(80);
    int input_h = dp(72);
    /* 状态栏钉在窗口最底部, 输入区整体上移让出它的高度 */
    int status_h = dp(20);
    int status_y = H - status_h - dp(2);
    int input_y  = status_y - gap - input_h;
    /* 发送与停止**垂直叠成一列** (停止在上, 发送在下), 而不是左右并列:
     * 并排时这一列要吃 160+ 逻辑像素, 输入框被挤窄; 叠起来只占一个按钮的宽。
     * 两个按钮在 input_h 内均分高度 —— 用"总高减一个 gap 再对半"算, 于是列的上下边缘
     * 与输入框齐平, 两个按钮中间正好一个 gap。 */
    int btn_h = (input_h - gap) / 2;
    int input_w = W - btn_w - gap * 3;

    for (int i = 0; i < 3; i++) {
        int y = gap + i * (row_h + gap);
        HWND lbl = GetDlgItem(hwnd, ID_LBL_BASE + i);
        MoveWindow(lbl,        gap,         y + dp(4), lbl_w,             row_h, TRUE);
        MoveWindow(g_hCfg[i],  gap + lbl_w, y,         W - gap*2 - lbl_w, row_h, TRUE);
    }
    {
        int y = gap + 3 * (row_h + gap);
        HWND lbl = GetDlgItem(hwnd, ID_LBL_WS);
        MoveWindow(lbl, gap, y + dp(4), lbl_w, row_h, TRUE);
        /* 按钮从右往左依次排: 加载会话 | 新建会话 | 浏览... —— 顺序写死在这里,
         * 加成对/删除都只动这几行, 不必再手算每个偏移。工作目录输入框吃掉剩余宽度。 */
        int x = W - gap;
        x -= sess_w;         MoveWindow(g_hSess,     x, y, sess_w,   row_h, TRUE);
        x -= gap + new_w;    MoveWindow(g_hNew,      x, y, new_w,    row_h, TRUE);
        x -= gap + ws_btn_w; MoveWindow(g_hWsBrowse, x, y, ws_btn_w, row_h, TRUE);
        x -= gap;
        MoveWindow(g_hWorkspace, gap + lbl_w, y, x - (gap + lbl_w), row_h, TRUE);
    }

    MoveWindow(g_hHistory, gap, top_h, W - gap * 2,
               input_y - gap - top_h, TRUE);
    ui_history_pad(g_hHistory, W - gap * 2, input_y - gap - top_h);
    MoveWindow(g_hInput, gap, input_y, input_w, input_h, TRUE);
    {
        int bx = gap * 2 + input_w;
        MoveWindow(g_hStop, bx, input_y,               btn_w, btn_h, TRUE);   /* 上: 停止 */
        MoveWindow(g_hSend, bx, input_y + btn_h + gap, btn_w, btn_h, TRUE);   /* 下: 发送 */
    }
    MoveWindow(g_hStatus, gap, status_y, W - gap * 2, status_h, TRUE);
}

/* ===== 字体与行距 (跨显示器 DPI 变化时要整体重做) ===== */

static BOOL CALLBACK ui_font_cb(HWND h, LPARAM lp) {
    (void)lp;
    /* 输出区用独立字体, 其余控件共用界面字体 */
    SendMessageW(h, WM_SETFONT,
                 (WPARAM)(GetDlgCtrlID(h) == ID_HISTORY ? g_hFontHist : g_hFont), TRUE);
    return TRUE;
}

/* 按 g_dpi 重建界面字体并下发到全部子控件。
 * **顺序必须是** 建新的 -> 下发 -> 再删旧的; 反过来控件会短暂引用已释放的句柄。
 * 窗口首次创建时也调它 —— 那时还没有子控件, 下发这一步自然是空操作。 */
static void ui_fonts_rebuild(HWND hwnd) {
    HFONT old[2] = { g_hFont, g_hFontHist };
    g_hFont     = CreateFontW(dp(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CAGENT_FONT_QUALITY, FF_DONTCARE, CAGENT_UI_FACE);
    /* 输出区独立字体: 默认比控件/标签大一档, 阅读更醒目。
     * **基准字号恒为 HIST_LPX_DEF** —— Ctrl+滚轮的缩放走 EM_SETZOOM(整体显示缩放),
     * 不再靠改这个字体, 否则每次滚动都要重建字体+刷全部 run, 还会把 run 字号钉死。 */
    g_hFontHist = CreateFontW(dp(HIST_LPX_DEF), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CAGENT_FONT_QUALITY, FF_DONTCARE, CAGENT_UI_FACE);
    EnumChildWindows(hwnd, ui_font_cb, 0);
    for (int i = 0; i < 2; i++) if (old[i]) DeleteObject(old[i]);
}

/* DPI 变化后重算所有段落的留白与缩进。
 * 段落属性(段前/段后/左缩进)与行距一样是**绝对 twips**, 不随字体缩放 —— 不重算的话,
 * 从 200% 屏拖到 250% 屏, 行距是对的(ui_history_parafmt 重设过)而区块之间的留白还是
 * 旧 DPI 的值, 整体节奏就散了。
 *
 * 做法是逐段读出旧值、按 DPI 比例缩放后写回, **不需要知道每段是什么角色**: 缩放是线性的,
 * 各角色之间的相对关系(工具名紧凑、输出块宽松)自然保留。
 *
 * 两个坑:
 *   ① EM_GETLINECOUNT / EM_LINEINDEX 数的是**显示行**(软换行也占一行), 同一段落可能横跨
 *      多行 —— 逐行缩放会把该段的留白缩放多次。所以只处理**段落起点**(前一字符是换行的位置)。
 *   ② 只在跨显示器时跑一次, 但行数可能上千, 循环里全是 SendMessage, 别在别处随手调用。 */
static void ui_para_reflow(HWND h, UINT old_dpi, UINT new_dpi) {
    if (!h || !old_dpi || !new_dpi || old_dpi == new_dpi) return;
    int n = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    if (n <= 0) return;
    LONG sel_s = 0, sel_e = 0;
    SendMessageW(h, EM_GETSEL, (WPARAM)&sel_s, (LPARAM)&sel_e);
    for (int i = 0; i < n; i++) {
        LONG pos = (LONG)SendMessageW(h, EM_LINEINDEX, (WPARAM)i, 0);
        if (pos < 0) continue;
        if (pos > 0) {
            /* 段落起点判定: 前一个字符必须是换行 (CRLF 的两个字节都算) */
            WCHAR prev = 0;
            TEXTRANGEW tr;
            tr.chrg.cpMin = pos - 1;
            tr.chrg.cpMax = pos;
            tr.lpstrText = &prev;
            SendMessageW(h, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
            if (prev != L'\r' && prev != L'\n') continue;
        }
        SendMessageW(h, EM_SETSEL, pos, pos);
        PARAFORMAT2 pf;
        memset(&pf, 0, sizeof(pf));
        pf.cbSize = sizeof(pf);
        if (!SendMessageW(h, EM_GETPARAFORMAT, 0, (LPARAM)&pf)) continue;
        pf.dwMask        = PFM_SPACEBEFORE | PFM_SPACEAFTER | PFM_STARTINDENT;
        pf.dySpaceBefore = MulDiv(pf.dySpaceBefore, (int)new_dpi, (int)old_dpi);
        pf.dySpaceAfter  = MulDiv(pf.dySpaceAfter,  (int)new_dpi, (int)old_dpi);
        pf.dxStartIndent = MulDiv(pf.dxStartIndent, (int)new_dpi, (int)old_dpi);
        SendMessageW(h, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    }
    SendMessageW(h, EM_SETSEL, sel_s, sel_e);
}

/* 收紧历史框行距: 用 PARAFORMAT2 设"精确"行距 = 字身格高 + HIST_LINE_EXTRA,
 * 去掉 RichEdit 默认的额外行距。三个坑:
 *   ① dyLineSpacing 是**绝对 twips**, 不随字体走 —— 字体换了(跨显示器 DPI 变化)就必须重设,
 *      否则行高按旧字号卡死, 字号变大时字会被裁切;
 *   ② 段落属性只作用于选中范围, 空选区时仅改光标所在那一段 —— 所以先全选、改完再还原选区;
 *   ③ 用**精确(exactly)**行距时行盒小于字形就会被削顶(见 state.h 的 HIST_LINE_EXTRA),
 *      别把值设成刚好等于字身格高。
 * 另外: 空字符串 WM_SETTEXT 会把段落格式复位(实测行距会跳回 RichEdit 的自然行距),
 * 所以清空历史框之后必须重新调一次本函数。 */
static void ui_history_parafmt(HWND h) {
    if (!h) return;
    LONG sel_s = 0, sel_e = 0;
    SendMessageW(h, EM_GETSEL, (WPARAM)&sel_s, (LPARAM)&sel_e);
    SendMessageW(h, EM_SETSEL, 0, -1);

    PARAFORMAT2 pf;
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_LINESPACING;
    pf.bLineSpacingRule = 4;          /* 精确行距 */
    /* 行高直接由"请求的字体像素高 + 余量"换算成 twips, **单一 DPI 来源 = g_dpi**。
     * 这里曾经用 GetTextMetricsW + GetDeviceCaps(hdc, LOGPIXELSY) 去量, 但那是错的:
     * 在 WM_DPICHANGED 里子控件的 DPI 上下文还没跟着更新, 那个 DC 报的仍是**旧屏**的 DPI,
     * 而 dp() 已经是新屏的 —— 两个口径混用会让行距整体偏一个 DPI 比例
     * (实测 200%->250% 时行距从应有的 47px 变成 60px)。 */
    pf.dyLineSpacing = (LONG)((LONGLONG)dp(HIST_LPX_DEF + HIST_LINE_EXTRA) * 1440
                              / (g_dpi ? g_dpi : 96));
    SendMessageW(h, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    SendMessageW(h, EM_SETSEL, sel_s, sel_e);
    SendMessageW(h, EM_SETMODIFY, FALSE, 0);
}

/* ===== 输出区字号缩放 (Ctrl + 滚轮) ===== */

/* 把 g_hist_lpx 折算成 EM_SETZOOM 的比值下发。基准字号(1:1)就是 HIST_LPX_DEF,
 * 其余尺寸全由控件自己缩放显示。
 *
 * 为什么用 EM_SETZOOM 而不是"重建字体 + SCF_ALL 刷 run 字号"(这两条路都实测过):
 *   - EM_SETZOOM 只缩放**显示**, 不碰字符格式 -> 行距(精确 twips)自动等比跟随。
 *     实测 1:1 行距 36px -> 5:4 得 45 -> 3:2 得 54, 回到 1:1 精确还原; 12:18 得 24、
 *     40:18 得 80, 与预期一致, 两端都被接受。
 *   - 写 run 字号会引入两个真 bug: ① yHeight 是 em 高、而默认字体是字身格高
 *     (YaHei 差 1.27 倍), 字比行距大 -> 削顶; ② WM_SETFONT 会作废 run 字号,
 *     跨显示器触发 ui_fonts_rebuild 后文字大小会跳变。
 * 若将来换到不认 EM_SETZOOM 的 RichEdit, 返回 0 —— 那时再考虑别的路, 但别再写 run 字号。 */
static void ui_history_zoom_apply(HWND hist) {
    if (!hist) return;
    SendMessageW(hist, EM_SETZOOM, (WPARAM)g_hist_lpx, (LPARAM)HIST_LPX_DEF);
}

/* 步进单位 1 逻辑像素。
 *
 * EM_SETZOOM 触发的重排是全文档最重的一次: 每个段落都要重新算行高、每个换行点都要
 * 重新断行。它本身会同步重画, 之后若再 InvalidateRect 就是第二次全量重画, 且中间还
 * 隔着"整屏擦成背景色"的空窗 —— 缩放手势那一下闪就是这么来的。所以: SETREDRAW 包住
 * EM_SETZOOM, 末尾只留一次不带 erase 的同步重画收尾。 */
static void ui_history_zoom(HWND hist, int steps) {
    if (!hist || !steps) return;
    int n = g_hist_lpx + steps;
    if (n < HIST_LPX_MIN) n = HIST_LPX_MIN;
    if (n > HIST_LPX_MAX) n = HIST_LPX_MAX;
    if (n == g_hist_lpx) return;                 /* 已到上下限: 不做无谓的缩放 */
    g_hist_lpx = n;
    SendMessageW(hist, WM_SETREDRAW, FALSE, 0);
    ui_history_zoom_apply(hist);
    SendMessageW(hist, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(hist, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
}

/* Ctrl+滚轮 -> 缩放输出区字号; 返回非 0 表示该消息已被消费。
 *
 * 三处都要挂(主窗口 / 输出框子类 / 输入框子类), 因为 Windows 是把滚轮消息发给**焦点窗口**
 * 而不是光标下的窗口 —— 只挂输出框的话, 焦点在输入框里时就压根收不到。反过来说, 这个
 * 手势的语义就是"整窗有效、只作用于输出区", 与浏览器里 Ctrl+滚轮缩放一致。
 *
 * 修饰键用消息自带的 MK_CONTROL 而不是只靠 GetKeyState: 它随消息一起送达, 不依赖当前
 * 线程的消息队列状态(GetKeyState 取的是**调用线程**的键状态), 也让这条路径能被自动化验证。 */
static int ui_zoom_wheel(WPARAM wp) {
    if (!(LOWORD(wp) & MK_CONTROL) && !(GetKeyState(VK_CONTROL) & 0x8000)) return 0;

    int delta = (int)(short)HIWORD(wp);
    /* 一格 = WHEEL_DELTA(120)。高精度滚轮/触控板给出的是更小的增量, 累积满一格才走一步;
     * 方向一变就丢弃残量, 否则来回微滚会攒出"白送的"一步。 */
    if ((delta > 0) != (g_hist_zoom_acc > 0)) g_hist_zoom_acc = 0;
    g_hist_zoom_acc += delta;
    int steps = 0;
    while (g_hist_zoom_acc >= WHEEL_DELTA)  { g_hist_zoom_acc -= WHEEL_DELTA; steps++; }
    while (g_hist_zoom_acc <= -WHEEL_DELTA) { g_hist_zoom_acc += WHEEL_DELTA; steps--; }
    ui_history_zoom(g_hHistory, steps);
    return 1;   /* 攒不满一格也算消费: 否则缩放途中会同时触发控件自己的滚动 */
}

/* 输入框的占位提示: 把排队中的插话列出来 (文本由 hooks.h 的 ui_inbox_tip 拼好)。
 *
 * 为什么不用 EM_SETCUEBANNER: 那个接口**只认单行 EDIT** —— 实测多行控件上
 * EM_SETCUEBANNER 直接返回 FALSE 且什么都不画 (test/ui_cue_probe.c 就是为查这个写的),
 * 而我们的输入框是多行可换行的 (Shift+Enter)。自己画还顺带得到原生接口给不了的行为:
 * **聚焦时也显示**。这里显示的是"还有几条排队没发", 用户正一边打字一边看它 —— 原生 cue
 * 恰恰是聚焦即隐 (实测 focused 灰字像素 679 -> 0), 那等于常态下什么都看不到。
 *
 * 只在输入框为空时画: 有内容时画上去会和正文重叠 (文本一非空, 下面那次空转绘制就不再
 * 发生, 原先的灰字被控件自己的重绘覆盖掉 —— EN_CHANGE 分支已经把整块客户区标脏)。
 * 颜色取 COLOR_GRAYTEXT, 与配置区接口地址框的 cue 同色, 跟着系统主题走。 */
static void ui_input_draw_tip(HWND h) {
    if (GetWindowTextLengthW(h) > 0) return;
    WCHAR tip[INBOX_TIP_LEN];
    if (!ui_inbox_tip(tip, INBOX_TIP_LEN)) return;

    HDC dc = GetDC(h);
    if (!dc) return;
    HFONT old_font = (HFONT)SelectObject(dc, g_hFont);
    int      old_bk = SetBkMode(dc, TRANSPARENT);
    COLORREF old_fg = SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));

    RECT rc;
    GetClientRect(h, &rc);
    rc.left  += dp(5);      /* 起点让开控件自身的左边距: 插入符就画在那一列, 不会压字 */
    rc.top   += dp(3);
    rc.right -= dp(3);
    /* DT_NOPREFIX: 用户消息里可能有 '&' (会当助记符吃掉); 不设 DT_SINGLELINE, 文本里的
     * '\n' 就是换行 —— 每条插话一行正是靠它。 */
    DrawTextW(dc, tip, -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX);

    SetTextColor(dc, old_fg);
    SetBkMode(dc, old_bk);
    SelectObject(dc, old_font);
    ReleaseDC(h, dc);
}

static LRESULT CALLBACK InputProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    /* 队列变了 (工作线程 drain 之后投来的) -> 重画占位清单 */
    if (msg == WM_APP_INBOX) {
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }
    /* 画完控件自己的内容再补占位 —— 必须排在 CallWindowProc 之后: 默认过程在 WM_PAINT
     * 里已经做完 BeginPaint/EndPaint, 此刻的 GetDC 画上去才不会被它覆盖。 */
    if (msg == WM_PAINT) {
        LRESULT r = CallWindowProcW(g_oldInputProc, h, msg, wp, lp);
        ui_input_draw_tip(h);
        return r;
    }
    /* Ctrl+滚轮: 缩放输出区字号(见 ui_zoom_wheel 的注释: 焦点在这里时消息只到这儿) */
    if (msg == WM_MOUSEWHEEL && ui_zoom_wheel(wp)) return 0;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            return CallWindowProcW(g_oldInputProc, h, msg, wp, lp);
        }
        SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(ID_SEND, BN_CLICKED), 0);
        return 0;
    }
    /* Esc = 停止 (与主流 agent 一致)。走 ID_STOP 的真实处理: 空闲时处理器自己no-op,
     * 不必在这里判 g_running。Edit 控件本身不吃 Esc, 不存在吞掉原有行为的问题。 */
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        SendMessageW(GetParent(h), WM_COMMAND, MAKEWPARAM(ID_STOP, BN_CLICKED), 0);
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_RETURN) {
        if (!(GetKeyState(VK_SHIFT) & 0x8000)) return 0;
    }
    return CallWindowProcW(g_oldInputProc, h, msg, wp, lp);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        LoadLibraryW(L"Msftedit.dll");   /* 注册 RICHEDIT50W 控件类 */

        /* 此刻窗口已经落在某块显示器上 —— 按**这块屏**的 DPI 定 g_dpi。
         * per-monitor 下不能沿用 dpi_init() 里那个(那是主屏的)。 */
        g_dpi = dpi_of_window(hwnd);

        /* 按 DPI 修正外框, 让客户区正好是 720x560 逻辑像素 */
        {
            RECT r = { 0, 0, dp(720), dp(560) };
            dpi_adjust_rect(&r, CAGENT_WND_STYLE, FALSE, 0, g_dpi);
            SetWindowPos(hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        ui_fonts_rebuild(hwnd);   /* 建字体; 此刻还没有子控件, 下发是空操作 */

        /* 前三项配置的标签: 中文、不带冒号 (冒号是"标签 + 输入框"的旧习惯, 这里靠布局对齐
         * 已经够清楚了)。第 4 项"工作目录"在下面单独建 —— 它旁边还有个"浏览"按钮, 排版不同。 */
        static const WCHAR *labels[3] = { L"接口地址", L"API Key", L"模型名称" };
        static const DWORD ed_styles[3] = { 0, ES_PASSWORD, 0 };

        for (int i = 0; i < 3; i++) {
            HWND lbl = CreateWindowW(L"STATIC", labels[i],
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)(ID_LBL_BASE + i), NULL, NULL);
            SendMessageW(lbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);

            g_hCfg[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ed_styles[i],
                0, 0, 0, 0, hwnd,
                (HMENU)(LONG_PTR)(ID_CFG_BASE + i), NULL, NULL);
            SendMessageW(g_hCfg[i], WM_SETFONT, (WPARAM)g_hFont, TRUE);
        }
        SendMessageW(g_hCfg[CFG_KEY], EM_SETPASSWORDCHAR, (WPARAM)'*', 0);
        /* 接口地址的占位提示: 框为空时以灰字显示, 顺带说明协议的兼容范围。
         * EM_SETCUEBANNER (Vista + comctl32 v6, manifest 已声明) 只对空串生效 ——
         * 用户已保存的 api_url 不会被打扰。用 Edit_SetCueBannerText 宏 (wParam=0):
         * 聚焦即隐藏、失焦且仍为空时回来。提示只是"看一眼就该被替换"的示例地址,
         * 用户已经知道要填什么了还挂在里面反而挡眼。 */
        Edit_SetCueBannerText(g_hCfg[CFG_URL],
                              L"仅支持 OpenAI 兼容协议 API, 如 https://api.example.com/v1");

        config_load();
        log_line("[启动] cagent %s, 日志已启用 (cagent.ini 里 log=0 可关)", CAGENT_VERSION_STR);
        skills_init();               /* 扫描 exe 同目录 skills\ 的技能, 生成提示词索引 */
        system_prompt_init();        /* 读 exe 同目录外置 SYSTEM_PROMPT, 缺失/无效静默用默认; 会拼上技能索引 */
        migrate_legacy_sessions();   /* 旧版散落在 exe 目录的会话搬进 sessions\ */
        cleanup_orphan_session_backups();   /* 主文件已删的 .bak / .pre_compact 顺手清掉 */
        set_edit_utf8(g_hCfg[CFG_URL], g_api_url);
        set_edit_utf8(g_hCfg[CFG_KEY], g_api_key);
        set_edit_utf8(g_hCfg[CFG_MDL], g_model);

        {
            HWND lbl = CreateWindowW(L"STATIC", L"工作目录",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                0,0,0,0, hwnd, (HMENU)(LONG_PTR)ID_LBL_WS, NULL, NULL);
            SendMessageW(lbl, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            g_hWorkspace = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                0,0,0,0, hwnd, (HMENU)(LONG_PTR)ID_WS_EDIT, NULL, NULL);
            SendMessageW(g_hWorkspace, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            g_hWsBrowse = CreateWindowW(L"BUTTON", L"浏览...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                0,0,0,0, hwnd, (HMENU)(LONG_PTR)ID_WS_BTN, NULL, NULL);
            SendMessageW(g_hWsBrowse, WM_SETFONT, (WPARAM)g_hFont, TRUE);
            ensure_workspace();
            set_edit_utf8(g_hWorkspace, g_workspace);
        }

        g_hHistory = CreateWindowExW(WS_EX_CLIENTEDGE, L"RICHEDIT50W", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_HISTORY, NULL, NULL);
        SendMessageW(g_hHistory, WM_SETFONT, (WPARAM)g_hFontHist, TRUE);
        ui_history_parafmt(g_hHistory);
        /* 输出区底色略深于输入框(见 state.h 的 HIST_BG)。必须走 EM_SETBKGNDCOLOR ——
         * RichEdit 不是 STATIC, 不会来问 WM_CTLCOLORSTATIC, 那边返回的画刷对它无效。
         * 颜色与 DPI 无关, 所以只在创建时设一次; WM_DPICHANGED 里不必重设。 */
        SendMessageW(g_hHistory, EM_SETBKGNDCOLOR, 0, (LPARAM)(DWORD)HIST_BG);

        g_hInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_INPUT, NULL, NULL);
        SendMessageW(g_hInput, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_oldInputProc = (WNDPROC)SetWindowLongPtrW(g_hInput, GWLP_WNDPROC, (LONG_PTR)InputProc);

        g_hSend = CreateWindowW(L"BUTTON", L"发送",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_SEND, NULL, NULL);
        SendMessageW(g_hSend, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        /* 停止是独立按钮: 与发送分开, 运行中点亮、空闲禁用 (ui_buttons_* 管状态)。
         * 没有"运行中把发送按钮改成停止"那套了 —— 发送永远可点: 空闲发新轮,
         * 运行中把文字排队成插话 (见 WM_COMMAND 的 ID_SEND 分支)。 */
        g_hStop = CreateWindowW(L"BUTTON", L"停止",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_STOP, NULL, NULL);
        SendMessageW(g_hStop, WM_SETFONT, (WPARAM)g_hFont, TRUE);
        EnableWindow(g_hStop, FALSE);

        g_hNew = CreateWindowW(L"BUTTON", L"新建会话",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_NEW_BTN, NULL, NULL);
        SendMessageW(g_hNew, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        g_hSess = CreateWindowW(L"BUTTON", L"加载会话",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_SESS_BTN, NULL, NULL);
        SendMessageW(g_hSess, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        /* 状态栏: 只读 STATIC, 显示"模型此刻在做什么"。这些信息以前是往输出区打旁白的,
         * 但状态不属于对话内容 —— 混进去既占地方, 又会被历史保存和回放 (见 core/base.h
         * 的 cagent_status)。初值"就绪"由 WM_APP_STATUS 带 0 时统一设置。 */
        g_hStatus = CreateWindowW(L"STATIC", L"就绪",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            0, 0, 0, 0, hwnd, (HMENU)(LONG_PTR)ID_STATUS, NULL, NULL);
        SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);

        /* 「关于」挂到系统菜单 (标题栏图标左键 / Alt+Space / 右键标题栏)。
         * GetSystemMenu 第二参数必须传 FALSE: 传 TRUE 会把菜单重置回默认,
         * 我们刚加的两项会被抹掉。分隔线把自定义项与系统的 关闭 隔开。 */
        {
            HMENU sysm = GetSystemMenu(hwnd, FALSE);
            if (sysm) {
                AppendMenuW(sysm, MF_SEPARATOR, 0, NULL);
                AppendMenuW(sysm, MF_STRING, IDM_ABOUT, L"关于(&A)...");
            }
        }

        {
            /* 启动不自动加载历史: 全新对话, 旧会话通过"加载会话"按钮按需载入 */
            reset_conversation();
            snprintf(g_active_ws, sizeof(g_active_ws), "%s", g_workspace);
        }

        if (g_key_decrypt_failed) {
            emit_err("API Key 解密失败 (可能换了用户/机器), 请重新填写 Key。\r\n");
            g_key_decrypt_failed = 0;
        }

        /* 不要在这里 SetFocus(g_hInput): 窗口尚未显示, 输入框会在"光标创建但
         * 不可见"的半初始化状态下拿到焦点, 且之后点击不再触发 WM_SETFOCUS
         * (已有焦点), 光标永不出现。保持不设焦点, 由用户点击时自然建立。 */
        return 0;
    }

    case WM_SIZE:
        layout(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        /* 最小跟踪尺寸: 下方 layout() 没有对极小窗口做钳制, 不设下限的话用户把窗口
         * 拖到几十像素高时输入框/历史区会算出负坐标溢出窗口。560x400 逻辑像素足够
         * 放下头部三行配置 + 输入区。 */
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        RECT r = { 0, 0, dp(560), dp(400) };
        dpi_adjust_rect(&r, CAGENT_WND_STYLE, FALSE, 0, g_dpi);
        mmi->ptMinTrackSize.x = r.right - r.left;
        mmi->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }

    case WM_COMMAND:
        /* 输入框文本变了 (打字 / 粘贴 / 撤销 / SetWindowText 清空) -> 重画占位提示:
         * 空则画上, 非空则让控件自己的重绘把灰字盖掉。不逐个拦键盘消息, 是因为
         * EN_CHANGE 覆盖了所有改文本的路径; 重复标脏也会和控件自身的重绘合并成一次
         * WM_PAINT, 不会多画一遍。 */
        if (LOWORD(wp) == ID_INPUT && HIWORD(wp) == EN_CHANGE) {
            InvalidateRect(g_hInput, NULL, FALSE);
            return 0;
        }
        /* 「发送」永远可点: 空闲 -> 发新轮; 运行中 -> 有文字就**排队** (插话),
         * 空则无事可做。**不再有"运行中把发送按钮临时改成停止"那套** ——
         * 停止是「停止」按钮与 Esc 的独立职责 (见下面的 ID_STOP)。 */
        if (LOWORD(wp) == ID_SEND && HIWORD(wp) == BN_CLICKED) {
            if (g_running) {
                int wlen = GetWindowTextLengthW(g_hInput);
                if (wlen > 0) {
                    wchar_t *wbuf = (wchar_t*)malloc((size_t)(wlen + 1) * sizeof(wchar_t));
                    if (wbuf) {
                        GetWindowTextW(g_hInput, wbuf, wlen + 1);
                        int need = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, NULL, 0, NULL, NULL);
                        char *u8 = need > 0 ? (char*)malloc((size_t)need) : NULL;
                        if (u8) {
                            WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, u8, need, NULL, NULL);
                            if (inbox_push(u8)) {
                                /* 入队即清空输入框: 用户看到"话已递出去", 按钮带上队计数;
                                 * 清空后占位提示立刻列出全部排队条目 (见 ui_inbox_tip)。 */
                                SetWindowTextW(g_hInput, L"");
                                ui_buttons_running();
                                ui_inbox_tip_refresh();
                            } else {
                                /* 队满 (INBOX_MAX) 或超长 (INBOX_SLOT): 拒绝并说明,
                                 * 文字留在输入框 —— 不静默截断用户的指令 */
                                SetWindowTextW(g_hStatus, L"排队已满或内容过长, 未入队");
                            }
                            free(u8);
                        }
                        free(wbuf);
                    }
                }
            } else {
                start_task(hwnd);
            }
            return 0;
        }
        /* 「停止」: 一点即请求取消当前轮 (g_cancel 在迭代/工具间隙被检查, 见 agent.h)。
         * 只影响当前轮 —— 排队里的插话照旧在轮末自动发出: 那是用户清醒时打好的指令,
         * 不该被一次取消吞掉。 */
        if (LOWORD(wp) == ID_STOP && HIWORD(wp) == BN_CLICKED) {
            if (g_running) {
                InterlockedExchange(&g_cancel, 1);
                ui_buttons_stopping();
                emit_note("(正在停止, 需等当前工具执行完毕...)\r\n");
            }
            return 0;
        }
        if (LOWORD(wp) == ID_WS_BTN && HIWORD(wp) == BN_CLICKED) {
            IFileOpenDialog *pfd = NULL;
            if (SUCCEEDED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC,
                                            &IID_IFileOpenDialog, (void**)&pfd))) {
                FILEOPENDIALOGOPTIONS opts = 0;
                pfd->lpVtbl->GetOptions(pfd, &opts);
                pfd->lpVtbl->SetOptions(pfd, opts | FOS_PICKFOLDERS);
                pfd->lpVtbl->SetTitle(pfd, L"选择工作目录");
                if (SUCCEEDED(pfd->lpVtbl->Show(pfd, hwnd))) {
                    IShellItem *psi = NULL;
                    if (SUCCEEDED(pfd->lpVtbl->GetResult(pfd, &psi)) && psi) {
                        PWSTR ppath = NULL;
                        if (SUCCEEDED(psi->lpVtbl->GetDisplayName(psi, SIGDN_FILESYSPATH, &ppath)) && ppath) {
                            char utf8[MAX_PATH];
                            WideCharToMultiByte(CP_UTF8, 0, ppath, -1, utf8, sizeof(utf8), NULL, NULL);
                            snprintf(g_workspace, sizeof(g_workspace), "%s", utf8);
                            set_edit_utf8(g_hWorkspace, g_workspace);
                            CoTaskMemFree(ppath);
                        }
                        psi->lpVtbl->Release(psi);
                    }
                }
                pfd->lpVtbl->Release(pfd);
            }
            return 0;
        }
        if (LOWORD(wp) == ID_NEW_BTN && HIWORD(wp) == BN_CLICKED) {
            if (!g_running) {
                /* 新建会话: 当前对话先落盘保留, 重开空对话 (不删除任何文件) */
                read_edit_utf8(g_hWorkspace, g_workspace, sizeof(g_workspace));
                snprintf(g_active_ws, sizeof(g_active_ws), "%s", g_workspace);
                history_start_new();
                SetWindowTextW(g_hHistory, L"");
                g_last_role = -1;        /* 清空后追加状态作废 */
                g_last_no_lf = 0;
                md_reset();              /* Markdown 行缓冲/围栏状态跟着作废 */
                ui_fold_reset();         /* 折叠块的位置索引也跟着作废 */
                emit_note("(新会话已创建。旧会话文件保留, 可通过 [加载会话] 恢复)\r\n");
                SetFocus(g_hInput);
            }
            return 0;
        }
        if (LOWORD(wp) == ID_SESS_BTN && HIWORD(wp) == BN_CLICKED) {
            if (!g_running) show_session_dialog(hwnd);
            return 0;
        }
        break;

    case WM_SYSCOMMAND:
        /* 系统菜单里我们加的自定义项。低 4 位被系统用作内部标志(如 SC_MOUSEMENU 高位),
         * 所以比较前必须先掩掉 —— 否则鼠标点出来的命令 (0xF0x0) 匹配不上。 */
        if ((wp & 0xFFF0) == IDM_ABOUT) {
            show_about(hwnd);
            return 0;
        }
        break;

    case WM_DPICHANGED: {
        /* 窗口被拖到了缩放比例不同的显示器。
         * **顺序很关键**: 先按新 DPI 调整外框并重新排布控件, **最后**才重建字体。
         * 反过来(先换字体)会让控件带着新字号、按**旧几何**先重画一次; 那一帧的像素若没被
         * 随后的位移擦掉就会留在原位 —— 实测: 主屏(200%)拖到副屏(250%)后, "工作目录:" 上方
         * 出现一排残留的笔画尖(屏幕上看得到, PrintWindow 重画时看不到, 因为它是残迹不是绘制错)。 */
        UINT nd = (UINT)HIWORD(wp);
        log_line("[ui] DPI 切换: %u -> %u", g_dpi, nd ? nd : dpi_of_window(hwnd));
        UINT old_dpi = g_dpi;   /* 段落留白要按新旧 DPI 比例缩放, 换 g_dpi 前先留一份 */
        g_dpi = nd ? nd : dpi_of_window(hwnd);

        {
            const RECT *pr = (const RECT*)lp;
            RECT r = { 0, 0, dp(720), dp(560) };
            dpi_adjust_rect(&r, CAGENT_WND_STYLE, FALSE, 0, g_dpi);
            SetWindowPos(hwnd, NULL, pr->left, pr->top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        layout(hwnd);
        ui_fonts_rebuild(hwnd);        /* 最后换字体: 控件按新几何重画一次即可 */
        ui_history_parafmt(g_hHistory);
        ui_para_reflow(g_hHistory, old_dpi, g_dpi);   /* 区块留白/缩进同样是绝对 twips */
        ui_history_zoom_apply(g_hHistory);   /* 缩放比是控件属性, 这里重下一遍保险 */

        /* 兜底: DPI 切换期间父窗口可能有大片区域没被擦到(子控件位移留下的空档)。
         * 整棵子树擦除 + 立即重画, 保证不留旧 DPI 的残迹。 */
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
        return 0;
    }

    case WM_MOUSEWHEEL:
        /* Ctrl+滚轮: 缩放输出区字号。没按 Ctrl 就交给控件自己滚动。 */
        if (ui_zoom_wheel(wp)) return 0;
        break;

    case WM_CTLCOLORSTATIC:
        /* 透明背景: 标签不擦自己的底, 底色由父窗口提供。
         * (试过改成"不透明 + 返回同色画刷"让标签自擦 —— 结果每个标签后面多出一块**白底**:
         *  OPAQUE 模式下 STATIC 填充用的是 DC 的 BkColor 而不是返回的画刷, 而 BkColor 默认是白。
         *  真要走自擦那条路, 必须同时 SetBkColor(COLOR_BTNFACE)。这里选择留在透明模式,
         *  由 WM_DPICHANGED 末尾那次整棵子树重画来保证不留旧像素。) */
        SetBkMode((HDC)wp, TRANSPARENT);
        /* 状态栏不单独设色 —— 用默认黑, 与配置区那几个标签同一观感。
         * (之前用的是灰色 RGB(110,110,110), 那是"次要信息"的取色, 但灰字看着比黑字小,
         * 实际字号是同一个 g_hFont。字号要改的话改 dpi.h 里 UI 字体那一处, 别在这里单设。) */
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);

    case WM_APP_STATUS: {
        /* core 在工作线程里报告状态, 走消息队列投递到这里才碰控件 */
        char *txt = (char*)lp;
        if (txt) {
            set_edit_utf8(g_hStatus, txt);   /* UTF-8 -> UTF-16 + SetWindowTextW, 对 STATIC 同样适用 */
            free(txt);
        } else {
            SetWindowTextW(g_hStatus, L"就绪");
        }
        return 0;
    }

    case WM_DESTROY:
        /* Agent 线程若仍在运行: 先请求取消并等它收尾 (history_save 在线程里执行,
         * 直接退出可能把会话文件写一半)。有界等待 —— 服务端僵死时最多多等 5s,
         * 不让关闭动作无限卡住; 超时则照常退出 (.bak 兜底)。 */
        if (g_hThread) {
            InterlockedExchange(&g_cancel, 1);
            WaitForSingleObject(g_hThread, 5000);
            CloseHandle(g_hThread);
            g_hThread = NULL;
        }
        InterlockedExchange(&g_running, 0);
        config_save();
        ui_fold_reset();          /* 收起态块里存着的正文副本要释放, 别指望进程退出兜底 */
        if (g_hFont) DeleteObject(g_hFont);
        if (g_hFontHist) DeleteObject(g_hFontHist);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK HistoryProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_APP_APPEND) {
        char *txt = (char*)lp;
        do_append(txt, (int)wp);
        free(txt);
        return 0;
    }
    if (msg == WM_APP_STREAM) {
        /* 新分段 / 回删都要废掉 Markdown 行缓冲: 缓冲里是没上屏的半行, 文档刚被
         * 改写, 它接着拼上去就是错文本 (旧答案的残尾)。回删分支还要连带围栏状态。 */
        md_reset();
        if (wp == 1) {
            g_stream_chars = 0;
        } else if (g_stream_chars > 0) {
            /* 用 cp 长度而不是 GetWindowTextLengthW: g_stream_chars 记的是 cp 增量,
             * 两个口径混用会删多或删少 (老代码里"估算误差留下残尾"就是这个原因)。
             * -1 表示选到真正的末尾。 */
            long start = ui_text_cp_len(h) - g_stream_chars;
            if (start < 0) start = 0;
            SendMessageW(h, EM_SETSEL, start, -1);
            SendMessageW(h, EM_REPLACESEL, FALSE, (LPARAM)L"");
            ui_fold_trim_from(start);   /* 起点之后的块正文已被删, 位置不再可信 */
            g_stream_chars = 0;
        }
        /* 回删后文档末尾回到了别的位置, 上一次的角色与"是否以换行结尾"都作废 ——
         * 留着会让下次追加基于过时状态补出多余的换行。 */
        g_last_role  = -1;
        g_last_no_lf = 0;
        return 0;
    }
    if (msg == WM_APP_FOLDEND) {
        /* 一轮结束的收尾 (半行落地 + 最后一个块按默认收起 + 滚回末尾)。
         * 历史回放走同一条 ui_turn_tail —— 保证"重新打开会话"与"实时对话"终态一致。 */
        ui_turn_tail();
        return 0;
    }
    /* 点击折叠块的标题行 -> 收起/展开。
     * **运行中只放过已写完的块**: 正在生成的那一块由 ui_fold_clickable 挡掉 —— 它的正文
     * 还在往文档末尾追加, 收起来会让下一片增量直接接在标题行后面, 块的状态和内容就再也
     * 对不上了。等这一轮结束再折它。 */
    if (msg == WM_LBUTTONDOWN) {
        POINTL pt;
        pt.x = (LONG)(short)LOWORD(lp);
        pt.y = (LONG)(short)HIWORD(lp);
        int idx = ui_fold_hit(pt);
        if (ui_fold_clickable(idx, g_running)) { ui_fold_toggle(idx); return 0; }
    }
    /* 悬停在**可点**的标题行上给手型, 提示"这里能点"。运行中正在生成的那一块不给 ——
     * 与点击行为保持一致: 不给手型就是"现在别点"的信号。 */
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        POINT p;
        if (GetCursorPos(&p) && ScreenToClient(h, &p)) {
            POINTL pt;
            pt.x = p.x;
            pt.y = p.y;
            if (ui_fold_clickable(ui_fold_hit(pt), g_running)) {
                SetCursor(LoadCursor(NULL, IDC_HAND));
                return TRUE;
            }
        }
    }
    /* Ctrl+滚轮缩放输出区字号; 其余滚轮动作照旧交回原过程(滚动)并补一次重画。
     * 重画不带 erase: 滚一下擦一次整屏, 快速滚动时会看到底色一闪一闪。 */
    if (msg == WM_MOUSEWHEEL && ui_zoom_wheel(wp)) return 0;
    if (msg == WM_VSCROLL || msg == WM_HSCROLL || msg == WM_MOUSEWHEEL ||
        msg == WM_KEYDOWN || msg == WM_KEYUP) {
        LRESULT r = CallWindowProcW(g_oldHistoryProc, h, msg, wp, lp);
        RedrawWindow(h, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW);
        return r;
    }
    return CallWindowProcW(g_oldHistoryProc, h, msg, wp, lp);
}
