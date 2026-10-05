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
LIBSSP_PATH ?= $(shell $(CC) -print-file-name=libssp.a)
LIBSSP_REAL := $(filter-out libssp.a,$(LIBSSP_PATH))
ifneq ($(LIBSSP_REAL),)
  STACK_CFLAGS  := -fstack-protector-strong
  STACK_LDFLAGS := -Wl,-Bstatic -lssp -Wl,-Bdynamic
  STACK_NOTE    := libssp.a 静态链接 ($(LIBSSP_REAL))
else
  STACK_CFLAGS  := -fno-stack-protector
  STACK_LDFLAGS :=
  STACK_NOTE    := 未找到 libssp.a, 回退 -fno-stack-protector
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

# 源码全部在 src/, 资源在 res/, 回归测试在 test/; 编译产物留在仓库根 ——
# 程序的"数据根"就是 exe 所在目录 (core/session.h 的 get_exe_dir_utf8 用它定位
# cagent.ini / sessions\ / log\ / SYSTEM_PROMPT), 所以 exe 不能挪进子目录,
# 否则配置文件会跟着跑到那里去。
GUI_TARGET = cagent.exe
GUI_SRC = src/main_gui.c
GUI_RES = app.res

# 子模块头: 核心按功能拆在 src/core/, 界面拆在 src/ui/
CORE_HDRS = src/cagent_core.h $(wildcard src/core/*.h)
UI_HDRS = $(wildcard src/ui/*.h)

GUI_LDFLAGS = -mwindows -lcomctl32 -lwinhttp -lcrypt32 -lshell32 -lole32 -luuid

# 控制台回归测试: 复用 src/core/* 全部 static 实现, 退出码 = 失败数
# -Isrc 找 cagent_core.h, -Itest 找同套件的 test.h
TEST_SRC = test/main.c
TEST_HDRS = test/test.h
TEST_TARGET = cagent_test.exe
TEST_CFLAGS = -Isrc -Itest
TEST_LDFLAGS = -lcomctl32 -lwinhttp -lcrypt32 -lshell32 -lole32 -luuid

.PHONY: all clean test

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
test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRC) $(TEST_HDRS) $(CORE_HDRS)
	$(CC) $(CFLAGS) $(TEST_CFLAGS) -o $@ $(TEST_SRC) $(LDFLAGS) $(TEST_LDFLAGS)

clean:
	@rm -rf $(GUI_TARGET) $(GUI_RES) $(TEST_TARGET)
