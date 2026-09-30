CC = gcc
WINDRES = windres
# -Os : 优化代码体积; -s : 链接后 strip 符号
CFLAGS  = -Wall -Wextra -Os
LDFLAGS = -s

GUI_TARGET = cagent.exe
GUI_SRC = cagent.c
GUI_RES = app.res

# 子模块头: 核心按功能拆在 core/, 界面拆在 ui/
CORE_HDRS = cagent_core.h $(wildcard core/*.h)
UI_HDRS = $(wildcard ui/*.h)

GUI_LDFLAGS = -mwindows -lcomctl32 -lwinhttp -lcrypt32 -lshell32 -lole32 -luuid

.PHONY: all clean

all: $(GUI_TARGET)

$(GUI_RES): app.rc app.ico
	$(WINDRES) -O coff -o $@ $<

$(GUI_TARGET): $(GUI_SRC) $(CORE_HDRS) $(UI_HDRS) $(GUI_RES)
	$(CC) $(CFLAGS) -o $@ $< $(GUI_RES) $(LDFLAGS) $(GUI_LDFLAGS)

clean:
	@rm -rf $(GUI_TARGET) $(GUI_RES)
