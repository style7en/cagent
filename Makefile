CC = gcc
WINDRES = windres
# -Os : 优化代码体积; -s : 链接后 strip 符号
CFLAGS  = -Wall -Wextra -Os
LDFLAGS = -s

GUI_TARGET = cagent.exe
GUI_SRC = cagent_ui.c
GUI_RES = app.res

# 子模块头: 核心按功能拆在 core/, 界面拆在 ui/
CORE_HDRS = cagent_core.h $(wildcard core/*.h)
UI_HDRS = $(wildcard ui/*.h)

GUI_LDFLAGS = -mwindows -lcomctl32 -lwinhttp -lcrypt32 -lshell32 -lole32 -luuid

# 控制台回归测试: 复用 core/* 全部 static 实现, 退出码 = 失败数
TEST_SRC = test_main.c
TEST_TARGET = cagent_test.exe
TEST_LDFLAGS = -lcomctl32 -lwinhttp -lcrypt32 -lshell32 -lole32 -luuid

.PHONY: all clean test

all: $(GUI_TARGET)

$(GUI_RES): app.rc app.ico
	$(WINDRES) -O coff -o $@ $<

$(GUI_TARGET): $(GUI_SRC) $(CORE_HDRS) $(UI_HDRS) $(GUI_RES)
	$(CC) $(CFLAGS) -o $@ $< $(GUI_RES) $(LDFLAGS) $(GUI_LDFLAGS)

# make test: 编译并运行回归测试 (失败则 make 返回非 0)
test: $(TEST_TARGET)
	./$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRC) $(CORE_HDRS)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(LDFLAGS) $(TEST_LDFLAGS)

clean:
	@rm -rf $(GUI_TARGET) $(GUI_RES) $(TEST_TARGET)
