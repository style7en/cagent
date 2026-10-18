/*
 * ui/state.h - 控件 ID / 自定义消息 / 全局句柄与字体
 *
 * cagent 界面层的一部分, 由 main_gui.c 按依赖顺序聚合 (单 TU, 全 static)。
 */

/* 界面层额外依赖: Shell/COM (选目录对话框) */
#include <shlobj.h>
#include <shobjidl.h>

/* ===== 控件 ID ===== */
#define ID_HISTORY  1001
#define ID_INPUT    1002
#define ID_SEND     1003
#define ID_NEW_BTN  1004   /* 新建会话按钮 */
#define ID_CFG_BASE 1005   /* +0 url, +1 key, +2 model */
#define ID_LBL_BASE 1008   /* +0 url, +1 key, +2 model */
#define ID_LBL_WS   1011   /* 工作目录标签 */
#define ID_WS_EDIT  1012   /* 工作目录输入框 */
#define ID_WS_BTN   1013   /* 浏览按钮 */
#define ID_SESS_BTN 1014   /* 加载会话按钮 */
#define ID_STATUS   1015   /* 状态栏: 显示模型此刻在做什么 (等待响应/生成中/执行工具/重试中) */
#define ID_STOP     1016   /* 停止按钮: 与发送彻底分开 (见 wndproc.h 的插话设计) */
#define ID_SESS_LB  2001   /* 会话列表 ListBox */
#define ID_SESS_OK  2002   /* 载入 */
#define ID_SESS_CAN 2003   /* 取消 */

/* ===== 主窗口样式 =====
 * CreateWindowW 与三处 dpi_adjust_rect(按客户区反推外框) 共用同一份, 免得两处各写一遍
 * 后走样。WS_CLIPCHILDREN 是**窗口**样式(WS_*), 只属于 CreateWindowW 的 dwStyle ——
 * 不要写进 WNDCLASSW.style, 那里只认 CS_* 类样式(如 CS_HREDRAW/CS_VREDRAW, 见 main.h)。
 * AdjustWindowRectEx 只看影响外框的样式, WS_CLIPCHILDREN 不影响计算结果, 故三处共用安全。 */
#define CAGENT_WND_STYLE (WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN)

/* 主窗口**扩展**样式: 整棵窗口树 (含全部子控件) 离屏合成后**一次性**呈现。
 *
 * 根因链 (2026-10-18 两轮取证): 减重画次数只能降概率, 根除不了闪 —— 输出区
 * 最大化 + 长文档时, **一次全客户区重画要几十毫秒, 跨显示帧**, DWM 会把画到
 * 一半的内容上屏, 用户看到的就是撕裂式闪烁 (用户实测反馈: 每片从 2 次降到
 * 1 次仍闪)。让呈现原子化 (整帧合成完再一次 BitBlt 上屏) 才能根除。
 * 附带效应: EM_SETRECT 重排的 5 次中间帧 (RichEdit 不吃 WM_SETREDRAW, 批不掉)
 * 也不再上屏 —— resize 时的抽动一并解决。
 *
 * 代价: 任一子控件重画都会触发一次整窗合成 (一次大 memcpy + 全部子控件重绘到
 * 缓冲)。换来的是**任何单帧重画多慢都不闪**。配合流式的小块自然重绘
 * (helpers.h 的 do_append 注释), 稳态成本只有一次合成。
 *
 * 注意: WS_EX_COMPOSITED 只是 ex 样式, **不影响 AdjustWindowRectEx 的外框计算**,
 * 所以两处 dpi_adjust_rect 的 exstyle 传 0 是对的, 别跟着改。 */
#define CAGENT_WND_EXSTYLE (WS_EX_COMPOSITED)

/* ===== 系统菜单命令 (WM_SYSCOMMAND) =====
 * 「关于」挂在窗口的系统菜单上 —— 主窗口没有菜单栏, 而标题栏问号按钮(WS_EX_CONTEXTHELP)
 * 与最小化/最大化互斥(实测加了它问号根本不画), 所以系统菜单是这里放"关于"的标准位置:
 * 点标题栏图标 / Alt+Space / 右键标题栏都能到。
 *
 * 取值有两处硬约束:
 *   ① 必须避开系统预定义命令 —— SC_* 全在 0xF000 以上
 *      (SC_SIZE=0xF000 … SC_CONTEXTHELP=0xF180, 另有 SC_SEPARATOR=0xF00F)。
 *   ② **必须是 16 的倍数。** WM_SYSCOMMAND 的低 4 位被系统占用, 分发时要 `wp & 0xFFF0`,
 *      若 ID 自身低 4 位非 0 (如 900 = 0x384), 掩码会把它削成别的值 —— 分支永远不命中。
 *      GCC 会以 -Wtautological-compare 报出来, 这正是本例踩过的坑。 */
#define IDM_ABOUT   0x0100

#define WM_APP_APPEND  (WM_APP + 1)   /* wParam = role(int), lParam = UTF-8 char* (须 free) */
#define WM_APP_STREAM  (WM_APP + 2)   /* wParam = 1: 新分段开始(清零计数); 0: 回删本分段 AI 文本
                                       * 必须与 APP_APPEND 一样按 FIFO 投递, 不能跨线程 SendMessage */
#define WM_APP_FOLDEND (WM_APP + 3)   /* 一轮结束: 把最后一个折叠块也按默认收起
                                       * (由工作线程投递 —— 折叠要碰控件, 不能跨线程直接调) */
#define WM_APP_STATUS  (WM_APP + 4)   /* 更新状态栏。lParam = UTF-8 char* (须 free),
                                       * 传 0 表示回到空闲。**必须走消息队列**: core 在工作
                                       * 线程里报告状态, 直接碰控件是不允许的。 */
#define WM_APP_INBOX   (WM_APP + 5)   /* 插话队列变了 -> 重绘输入框的占位提示 ("后续任务"
                                       * 清单, 见 hooks.h 的 ui_inbox_tip)。drain 发生在
                                       * 工作线程, 同样只能投递, 不能直接碰控件。 */

#define CFG_URL 0
#define CFG_KEY 1
#define CFG_MDL 2

/* ===== UI 配色 =====
 * 输出区(历史框)底色比输入框略深一档, 让"只读的输出"与"可编辑的输入"一眼能分层。
 * 输入框不设色, 保持系统默认的 COLOR_WINDOW(白)。
 *
 * 247 是刻意选的: 与白差 8 级(约 3%) —— 并排能看出分层, 单独看又不会觉得"那块发灰";
 * 再深就接近窗口自身的 COLOR_BTNFACE(240), 输出区会和背景糊在一起, 层次反而没了。
 * 想整体调深浅只改这一个数。 */
#define HIST_BG RGB(247, 247, 247)

/* ===== 输出区角色样式表 =====
 * 每行对应一个 CAGENT_ROLE_* (索引即角色值), 决定这类文字长什么样。
 * 集中成一张表而不是散在 do_append 的 if-else 里: 加角色只要加一行, 调观感只改一处。
 *
 * 配色分工 (一屏内最多同时出现三四种颜色, 再多就成圣诞树):
 *   蓝底=用户说的话, 黑=AI 正文, 灰=过程旁白, 琥珀=工具调用, 灰底等宽=工具输出, 红=出错,
 *   冷灰底=思考过程。
 *
 * **三块容易混的东西靠"字体 + 底色冷暖 + 有没有引导行"同时错开**, 任一维度单独用都不够:
 *   思考过程  正文字体 + 冷灰底 RGB(241,242,246) + 缩进 16, 前面有"▾ 思考过程"引导行
 *   工具输出  等宽字体 + 暖灰底 RGB(237,237,233) + 缩进 16, 前面有"▸ 工具名"引导行
 *   AI 正文   正文字体 + 无底色 + 不缩进
 * 只靠灰度区分是不行的 —— 两块都是灰底, 挨在一起就糊成一片了。
 *
 * **灰色有下限**: 输出区底色是 RGB(247,247,247), 灰字最浅只能到 RGB(110,110,110) ——
 * 再浅对比度就跌破 WCAG AA 的 4.5:1 (实测 138 灰只有 3.2:1)。旁白想"更淡"只能靠
 * 字号或留白, 不能靠调浅颜色。
 *
 * 字段:
 *   fg/bg     文字色 / 字符底色 (has_bg=0 时不设底色, 跟随输出区背景)
 *   bold/italic/mono   粗体 / 斜体 / 等宽字体 (mono 用于工具参数与输出, 让数字对齐)
 *   sp_before/sp_after 段前 / 段后留白 (逻辑像素) —— 靠它把区块拉开, 不再一行行贴死
 *   indent             左缩进 (逻辑像素) —— 参数与输出缩进在工具名之下, 形成从属关系
 *
 * **段前/段后距是"整段"属性, 多行内容里每一行都是独立段落**, 所以: 留白只给那些
 * "一段就是一块"的角色 (用户消息、工具名行、旁白、错误), 而工具输出这种多行块
 * 必须保持 0 —— 否则每一行都带上间距, 整块会被拉散成稀疏的几行。
 * 块与块之间的间隔, 靠**下一个块首行的 sp_before** 提供。
 *
 * **字符底色只覆盖文字本身, 不铺满整行** (RichEdit 的限制, 详见 helpers.h 的说明):
 * 所以用户消息的蓝底是"像荧光笔划过一条"的效果, 不是通栏色块。想要通栏卡片只能自绘。
 *
 * **带底色的角色 (has_bg=1), 其色带高度 = 行盒 ± 该段自己的 sp_before/sp_after**
 * (test/ui_userbg_probe.c 实测: 段前距/段后距也被涂进字符底色里 —— 0/0 时色带=37px
 * 正好一行盒, 14/6 时被撑到 78px)。推论两条:
 *   ① **对称才居中**: ink 只居线盒中, sp_before≠sp_after 时文字在色带里偏上/偏下。
 *      USER(14/6)就是这样被用户抓到"蓝底上方太高、文字不在垂直中间", 已改 8/8;
 *   ② **多行块保持 0**: OUTPUT/THINK 的 sp 必须留 0, 否则每行一条色带、块被拉散
 *      (这条原本就知道, 现在知道了根因是色带把段距也涂了)。 */
typedef struct {
    COLORREF fg;
    COLORREF bg;
    int has_bg;
    int bold, italic, mono, underline;
    int sp_before, sp_after, indent;
} UiRoleStyle;

static const UiRoleStyle g_role_style[CAGENT_ROLE_COUNT] = {
    /* SYS    兜底 */ { RGB(110,110,110), 0,              0, 0,0,0,0,  2,  0,  0 },
    /* USER   用户 */ { RGB( 12, 68,124), RGB(230,241,251),1, 1,0,0,1,  8,  8,  0 },
    /* AI     正文 */ { RGB( 26, 26, 26), 0,              0, 0,0,0,0, 16,  0,  0 },
    /* NOTE   旁白 */ { RGB(110,110,110), 0,              0, 0,0,0,0,  4,  4,  0 },
    /* TOOL   调用 */ { RGB(138, 90,  0), 0,              0, 1,0,0,0, 14,  0,  0 },
    /* ARGS   参数 */ { RGB(107,107,107), 0,              0, 0,0,1,0,  0,  0, 16 },
    /* OUTPUT 输出 */ { RGB( 60, 60, 60), RGB(237,237,233),1, 0,0,1,0,  0,  0, 16 },
    /* ERR    出错 */ { RGB(176, 32, 32), 0,              0, 1,0,0,0,  8,  0,  0 },
    /* THINK  思考 */ { RGB(110,110,110), RGB(241,242,246),1, 0,0,0,0,  0,  0, 16 },
    /* THINKH 思考题头 */ { RGB( 84, 84, 84), RGB(241,242,246),1, 1,0,0,0, 10,  0,  0 },
};

/* ===== 输出区字号 (Ctrl+滚轮缩放) =====
 * 单位是 96 DPI 的**逻辑像素**, 表示"等效字号"。实现方式是 `EM_SETZOOM(当前值, 默认值)`,
 * 即整体缩放控件的显示 —— **不去改字符格式**, 因此行距会跟着等比缩放, 也不会出现
 * "run 字号被 WM_SETFONT 冲掉"那类跨屏跳变(详见 wndproc.h 的 ui_history_zoom)。
 * 上下限的理由: 12 约合 9pt, 再小汉字就难认了; 40 已是默认值的 2.2 倍, 一行放不下几个字。 */
#define HIST_LPX_DEF 18
#define HIST_LPX_MIN 12
#define HIST_LPX_MAX 40

/* 输出区行距在"字身格高"之外再留的余量(逻辑像素)。
 * 留 0 会让精确行距正好等于字身格高, 而实测 CJK 的字形外框比字身格还高约 2px(36px 字号时;
 * 带【】、重音等越界字形更明显) —— 字的顶边就会被行盒削掉(用户报过"字的顶都被削掉了")。
 * 留 1 逻辑像素即可消除削顶, 视觉上只比原来松约 5%。 */
#define HIST_LINE_EXTRA 1

/* 前向声明 (实现见 UI 辅助区) */
static void read_edit_utf8(HWND h, char *out, size_t cap);
static void set_edit_utf8(HWND h, const char *utf8);

/* ===== 可折叠块的位置索引 =====
 * RichEdit **没有"块"的概念, 也没有隐藏文本** (CFE_HIDDEN 在它这儿不生效), 所谓"折叠"
 * 只能是把正文从字符流里删掉、展开时再插回去。于是必须自己记住每块在哪 —— 而且任何一次
 * 折叠/展开都会让后面所有块的位置左移/右移, 得同步更新这张表。
 *
 * 位置一律用**字符位置 (cp)**, 与 EM_SETSEL / EM_POSFROMCHAR 同一坐标系。
 * 注意别和 GetWindowTextLengthW 混用: 那个把段落标记按 CRLF 算两个字符 (见 ui_text_cp_len)。
 *
 * 两种块共用一套机制:
 *   THINK  推理模型的思考过程 —— 标题行 "▾ 思考过程", 正文是 THINK 角色
 *   TOOL   一次工具调用 —— 标题行 "▾ 工具名", 正文是参数行 + 输出块 (ARGS / OUTPUT)
 * 工具块对所有模型都有用 (工具输出经常几十上百行), 思考块只在模型真返回 reasoning 时才有。 */
#define UI_FOLD_THINK 0
#define UI_FOLD_TOOL  1

/* 正文按"角色段"记 —— 折叠时逐段读回、展开时按原角色重放, 否则参数行和输出块会
 * 被刷成同一种样式 (读回来的是纯文本, 样式信息全在角色里)。
 * 一个工具块最多两段 (ARGS + OUTPUT), 思考块一段; 给 8 个是留足余量。 */
#define UI_FOLD_SEG_MAX 8
typedef struct {
    int    role;
    long   len;        /* 该段的 cp 长度 */
    WCHAR *text;       /* 收起时保存的文本 (展开时释放并置 NULL) */
} UiFoldSeg;

/* 折叠块索引表的容量。**表满时丢最老的块给新块腾位置** (见 ui_fold_new), 所以这不是
 * "最多能折多少个"而是"同时可交互的窗口有多大": 128 够往回翻好几屏, 又不至于让
 * ui_fold_refresh_heads (每次折叠都要遍历全表重设标题行格式) 变得昂贵。 */
#define UI_FOLD_MAX 128
typedef struct {
    int        kind;       /* UI_FOLD_THINK / UI_FOLD_TOOL */
    long       cp_head;    /* 标题行首字符的 cp */
    long       head_cp;    /* 标题行的 cp 长度 —— **因块而异** (工具名长短不同), 创建时实测 */
    long       body_len;   /* 正文的 cp 总长 (展开态) */
    UiFoldSeg  segs[UI_FOLD_SEG_MAX];
    int        n_segs;
    int        collapsed;
    /* 用户手动展开/收起过。置位后**不再自动折叠** —— 块结束时的默认收起不该把用户
     * 特意展开的那个又按回去。 */
    int        user_toggled;
} UiFoldBlock;

static UiFoldBlock g_folds[UI_FOLD_MAX];
static int  g_fold_n;

/* 前向声明 (实现见 UI 辅助区) */
static void   ui_fold_reset(void);
static void   ui_fold_new(int kind, long cp_before, long cp_after);
static void   ui_fold_extend(int role, long cp_added);
static void   ui_fold_collapse(int idx);
static int    ui_fold_hit(POINTL pt);
static void   ui_fold_toggle(int idx);
static void   ui_fold_trim_from(long cp);

/* ===== Markdown 渲染 (只作用于 AI 正式答案) =====
 * 前向声明, 实现见 ui/md.h —— 它排在 helpers.h 之后 (要用那里的 ui_insert_ex),
 * 而 helpers.h 的 do_append 又要调这里, 所以走声明而不是调整 include 顺序。
 *   md_reset   清空行缓冲与围栏状态: 重试回删 / 新分段 / 清空历史框之后必须调
 *   md_feed    喂一段 AI 文本: 完整行立即解析上屏, 半行留在缓冲里
 *   md_flush   把缓冲里的半行落地 (不补换行), 返回插入的 cp 数
 *   md_pending 行缓冲里有没有未落地的半行 */
static void md_reset(void);
static void md_feed(const WCHAR *w);
static long md_flush(void);
static int  md_pending(void);

/* ===== 插话排队 (运行中发送的消息) =====
 * 主流 agent (Claude Code / Codex / Cursor) 的"边跑边打字": 模型思考/执行期间用户照旧
 * 能输入, 按下发送**不打断当前轮**, 消息进队; 当前轮正常结束后自动开下一轮发给模型。
 * 停止是独立按钮/Esc 的职责, 与发送彻底分开 —— 所以输入框运行中也从不禁用。
 *
 * 容量与槽位: 8 条封顶 (一般插话一次一条, 8 条已是极端); 单槽 64KB —— 聊天输入到这个
 * 量级已是病态, 再长拒绝入队 (文字留在输入框, 不静默截断用户的指令)。
 * 不用 BUFSZ(1MB) 当槽位: 8 条就是 8MB 静态缓冲, 而 BUFSZ 是给 messages 整体用的。
 *
 * 锁: push 在 UI 线程 (发送按钮), take 在工作线程 (gui_on_done 的 drain), 必须互斥。
 * SRWLOCK 可静态初始化, 与 log.h 的 g_log_lock 同一手法。 */
#define INBOX_MAX  8
#define INBOX_SLOT (64 * 1024)
static char g_inbox[INBOX_MAX][INBOX_SLOT];
static int  g_inbox_n;
static SRWLOCK g_inbox_lock = SRWLOCK_INIT;

/* 占位提示的排版常量 (输入框为空时显示的"后续任务"清单, 见 hooks.h 的 ui_inbox_tip):
 *   INBOX_TIP_MAXCH 单条最多保留的字符数, 超出补 "..."  —— 每条只占一行, 长度固定
 *   INBOX_TIP_LEN   占位文本的总容量 (宽字符): 标题 + INBOX_MAX 行 + 余量 */
#define INBOX_TIP_MAXCH 28
#define INBOX_TIP_LEN   512

/* ===== GUI 状态 ===== */
static HWND g_hHistory, g_hInput, g_hSend, g_hStop, g_hNew, g_hSess;
static void show_session_dialog(HWND owner);
static void show_about(HWND owner);
static HWND g_hCfg[3];                 /* [url, key, model] */
static HWND g_hWorkspace, g_hWsBrowse; /* 工作目录 Edit + 浏览按钮 */
static HWND g_hStatus;                 /* 状态栏 (只读 STATIC) */
static HFONT g_hFont;
static HFONT g_hFontHist;
static int g_hist_lpx = HIST_LPX_DEF;   /* 输出区字号(逻辑像素), Ctrl+滚轮可调 */
static int g_hist_zoom_acc;             /* Ctrl+滚轮的累积残量, 给高精度滚轮/触控板用 */
static long g_stream_chars;      /* 当前分段已上屏的 AI 文本字符数(UTF-16), 供重试时回删 */
/* 上一次追加的角色, 以及"它是否以换行结尾"。用于在角色切换处补换行 —— 段落格式
 * (缩进/段间距)作用于整段, 若上一段没写完就切角色, 新格式会把上一段一起重排。
 * 核心侧已保证各类文本以换行收尾, 这里只是兜底, 别把它当成排版的主力。 */
static int g_last_role = -1;
static int g_last_no_lf = 0;
static HANDLE g_hThread = NULL;
static WNDPROC g_oldInputProc;
static WNDPROC g_oldHistoryProc;
