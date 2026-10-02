CC = gcc
WINDRES = windres
# -Os : 优化代码体积; -s : 链接后 strip 符号
CFLAGS  = -Wall -Wextra -Os
LDFLAGS = -s

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
