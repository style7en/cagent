CC = gcc
# -Os : 优化代码体积 (相对 -O2 牺牲极小性能, I/O bound 程序无感)
# -s  : 链接后 strip 全部符号
CFLAGS  = -Wall -Wextra -Os
LDFLAGS = -s

GUI_TARGET = cagent_gui.exe
GUI_SRC = cagent_gui.c
GUI_LDFLAGS = -mwindows -lcomctl32 -lwinhttp

.PHONY: all clean

all: $(GUI_TARGET)

$(GUI_TARGET): $(GUI_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(GUI_LDFLAGS)

clean:
	@rm -rf $(GUI_TARGET)
