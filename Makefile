# ===== 输出约定: make 打到终端的一切文案必须是纯 ASCII 英文 =====
# 本文件以 UTF-8 保存, 而中文 Windows 的控制台默认代码页是 936(GBK)。
# $(info) / $(warning) / @echo 的字节是原样透传给终端的, 不做任何转码 ——
# 于是 UTF-8 中文会被按 GBK 两两拆解, "静态链接" 显示成 "闈欐€侀摼鎺?" 之类的乱码。
# 结论: 注释可以随便写中文(注释不进终端), 但凡是 make 会打印出来的文案一律用英文。
# 若确需在终端看中文, 可在支持 UTF-8 的终端里先执行 `chcp 65001`。

CC = gcc
WINDRES = windres

# ===== 栈保护探测: libssp.a 是否存在 =====
# -fstack-protector-strong 在 MinGW 上会引入对 libssp 的引用。工具链自带 libssp.a 时,
# 静态链接它即可去掉 exe 对 libssp-0.dll 的运行时依赖 (本项目要求 exe 单文件可分发);
# 但精简/裁剪过的工具链可能没有 libssp.a —— 那时 -lssp 会直接链接失败。
# 先探测: `gcc -print-file-name=libssp.a` 找到给绝对路径, 找不到原样回显 "libssp.a"。
#   找到   -> 保留 -fstack-protector-strong + 静态链接 libssp;
#   找不到 -> 回退 -fno-stack-protector (关掉栈保护), 保证仍能构建出可运行的 exe。
# 可用 `make LIBSSP_PATH=libssp.a` 强制走回退分支 (验证用)。
# 注意: STACK_NOTE 会被 $(info) 打印到终端, 只能写 ASCII 英文 (见文件头"输出约定")。
LIBSSP_PATH ?= $(shell $(CC) -print-file-name=libssp.a)
LIBSSP_REAL := $(filter-out libssp.a,$(LIBSSP_PATH))
ifneq ($(LIBSSP_REAL),)
  STACK_CFLAGS  := -fstack-protector-strong
  STACK_LDFLAGS := -Wl,-Bstatic -lssp -Wl,-Bdynamic
  STACK_NOTE    := libssp.a static-linked ($(LIBSSP_REAL))
else
  STACK_CFLAGS  := -fno-stack-protector
  STACK_LDFLAGS :=
  STACK_NOTE    := libssp.a not found, falling back to -fno-stack-protector
endif
$(info [stack-protector] $(STACK_NOTE))

# ===== C99 stdio (切到 mingwex 实现) =====
# MinGW 默认可能走 msvcrt 的 printf, 它不认识 %zu / %lld 等 C99 长度修饰符 —— 既会让
# -Wformat 按 ms_printf 判据报"未知转换字符", 运行期也可能打印错误。定义下面两个宏切到
# mingwex 的 C99 实现 (此时 __MINGW_PRINTF_FORMAT 由 ms_printf 变为 gnu_printf):
#   _USE_MINGW_ANSI_STDIO    MinGW-w64 文档里的开关名;
#   __USE_MINGW_ANSI_STDIO   _mingw.h 内部实际读取的宏 (本工具链只认这个)。
# 两个都定义以覆盖不同版本的 _mingw.h; 取值一致, 不会触发重定义告警。
MINGW_STDIO_DEFS = -D_USE_MINGW_ANSI_STDIO=1 -D__USE_MINGW_ANSI_STDIO=1

# -Os : 优化代码体积; -s : 链接后 strip 符号
# 告警组 (全部在本项目上实测零噪音, 2026-10-03):
#   -Wshadow               局部/参数遮蔽同名全局即告警 (body[BUFSZ] 事故兜底)
#   -Wformat=2             printf 族格式串检查 (含非常量格式串的参数核对)
#   -Wlogical-op           疑似逻辑运算符误用 (&& 当 & 等)
#   -Wduplicated-cond/-branches  if-else 复制粘贴分支 / 重复条件
#   -Wnull-dereference    可能的空指针解引用 (依赖 -O 分析)
#   -Wvla                 禁变长数组 (本项目全用定长缓冲)
#   -Wundef               #if 里用了未定义宏
#   -Wstrict-prototypes   无参函数必须写 (void)
# 错误组 (GCC 14 默认, 提前升为错误):
#   -Werror=implicit-function-declaration / incompatible-pointer-types
# 加固组:
#   $(STACK_CFLAGS)  栈溢出保护 —— 由上方 libssp.a 探测决定:
#                    找到 -> -fstack-protector-strong; 找不到 -> -fno-stack-protector。
#                    链接端 NX/ASLR 三项见 LDFLAGS。
CFLAGS  = -Wall -Wextra -Wshadow -Wformat=2 -Wlogical-op \
          -Wduplicated-cond -Wduplicated-branches -Wnull-dereference \
          -Wvla -Wundef -Wstrict-prototypes \
          -Werror=implicit-function-declaration -Werror=incompatible-pointer-types \
          $(MINGW_STDIO_DEFS) $(STACK_CFLAGS) -Os
# 栈保护的链接段由上方探测决定 (STACK_LDFLAGS):
#   找到 libssp.a -> -Wl,-Bstatic -lssp -Wl,-Bdynamic (静态链接, 无 libssp-0.dll 运行时依赖);
#   找不到        -> 空 (配合 -fno-stack-protector, 不引入 libssp 引用, 链接不会失败)。
LDFLAGS = -s -Wl,--nxcompat -Wl,--dynamicbase -Wl,--high-entropy-va \
          $(STACK_LDFLAGS)

# 源码全部在 src/, 资源在 res/, 回归测试在 test/; **产品**编译产物留在仓库根 ——
# 程序的"数据根"就是 exe 所在目录 (core/session.h 的 get_exe_dir_utf8 用它定位
# cagent.ini / sessions\ / log\ / SYSTEM_PROMPT), 所以 exe 不能挪进子目录,
# 否则配置文件会跟着跑到那里去。
# 但 **test/ 源码生成的产物**(回归测试 exe / mdtest / preview / 各排查探针的 exe
# 与它们吐出的 BMP/JSON 临时文件)一律落 test/out/, 不散在工程根。
GUI_TARGET = cagent.exe
GUI_SRC = src/main_gui.c
GUI_RES = app.res
OUT_DIR = test/out

# 子模块头: 核心按功能拆在 src/core/, 界面拆在 src/ui/
CORE_HDRS = src/cagent_core.h $(wildcard src/core/*.h)
UI_HDRS = $(wildcard src/ui/*.h)

GUI_LDFLAGS = -mwindows -lcomctl32 -lwinhttp -lcrypt32 -lshell32 -lole32 -luuid

# 控制台回归测试: 复用 src/core/* 全部 static 实现, 退出码 = 失败数
# -Isrc 找 cagent_core.h, -Itest 找同套件的 test.h
TEST_SRC = test/main.c
TEST_HDRS = test/test.h
TEST_TARGET = $(OUT_DIR)/cagent_test.exe
TEST_CFLAGS = -Isrc -Itest
TEST_LDFLAGS = -lcomctl32 -lwinhttp -lcrypt32 -lshell32 -lole32 -luuid

# Markdown 渲染器边界单测: 桩掉 SendMessageW, 不建窗口 (无 UI 断言)
# -Wno-unused-*: 聚合进来的 UI 模块大部分函数本测试用不到, 与 preview 同理
MDTEST_SRC = test/md_test.c
MDTEST_TARGET = $(OUT_DIR)/md_test.exe

# 输出区排版的离屏预览: 把角色样式渲染成 BMP, 人眼检查 (验证工具, 不进产品二进制)
PREVIEW_SRC = test/ui_preview.c
PREVIEW_TARGET = $(OUT_DIR)/ui_preview.exe

# test/out 产物目录: 链接前先存在 (order-only 依赖, 目录 mtime 变化不触发重编)
$(OUT_DIR):
	@mkdir -p $@

.PHONY: all clean test mdtest preview

all: $(GUI_TARGET)

# app.rc 里的 include "core/version.h" 与 ICON "app.ico" 都相对自身目录解析:
# 前者要 src/ 才找得到 core/, 后者要 res/ 才找得到 app.ico, 故两个 -I 都要给。
# -c 65001: res/app.rc 是 UTF-8(无 BOM), 而 windres 默认按系统 ANSI 码页解释源码 ——
# 中文 Windows 上是 936(GBK), 于是 "语言极简编程" 的 18 个 UTF-8 字节被两两拆成 9 个
# GBK 字写进版本资源, 文件属性里就成了 "C 璇█鏋佺畝缂栫▼ Agent"。必须显式声明 UTF-8。
$(GUI_RES): res/app.rc res/app.ico res/app.manifest $(CORE_HDRS)
	$(WINDRES) -c 65001 -I src -I res -O coff -o $@ $<

$(GUI_TARGET): $(GUI_SRC) $(CORE_HDRS) $(UI_HDRS) $(GUI_RES)
	$(CC) $(CFLAGS) -o $@ $< $(GUI_RES) $(LDFLAGS) $(GUI_LDFLAGS)

# make test: 编译并运行回归测试 (失败则 make 返回非 0)
# make test: 跑离线回归测试, **退出码 = 失败数**。
# 跑完把测试二进制删掉, 不留在工程根 (下次 make test 会重新编译, 代价可接受)。
# 注意: 必须先把退出码存进变量再删 —— rm 之后 $? 就变成 rm 的退出码了, 会吞掉失败数。
test: $(TEST_TARGET)
	./$(TEST_TARGET); rc=$$?; rm -f $(TEST_TARGET); exit $$rc

$(TEST_TARGET): $(TEST_SRC) $(TEST_HDRS) $(CORE_HDRS) | $(OUT_DIR)
	$(CC) $(CFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_SRC) $(LDFLAGS) $(TEST_LDFLAGS)

# make mdtest: 跑 Markdown 边界单测 (失败则 make 返回非 0)
mdtest: $(MDTEST_TARGET)
	./$(MDTEST_TARGET); rc=$$?; rm -f $(MDTEST_TARGET); exit $$rc

$(MDTEST_TARGET): $(MDTEST_SRC) $(CORE_HDRS) $(UI_HDRS) | $(OUT_DIR)
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable \
	      -Isrc -o $@ $(MDTEST_SRC) $(LDFLAGS) $(TEST_LDFLAGS) -lgdi32 -luser32

# make preview: 渲染输出区排版 -> test/out/preview_out.bmp (人工看配色/缩进/段间距对不对)
# -Wno-unused-*: 预览程序直接聚合整套 UI 代码, 但只用到其中一小部分 —— 单 TU 下其余
# 部分必然"未被使用"。只在这里关, 别动主程序的告警开关。
# 跑完删掉 ui_preview.exe (与 make test / mdtest 同规): BMP 是给人眼看的产物要留下
# (在 test/out/ 下), 执行文件不必留着 —— 下次 make preview 重新编译, 代价可接受。
preview: $(PREVIEW_TARGET)
	./$(PREVIEW_TARGET); rc=$$?; rm -f $(PREVIEW_TARGET); exit $$rc

$(PREVIEW_TARGET): $(PREVIEW_SRC) $(CORE_HDRS) $(UI_HDRS) src/main_gui.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable \
	      -Isrc -o $@ $(PREVIEW_SRC) $(LDFLAGS) $(TEST_LDFLAGS) -lgdi32 -luser32

clean:
	@rm -rf $(GUI_TARGET) $(GUI_RES) $(OUT_DIR)
