CC = gcc
CFLAGS = -Wall -Wextra -O2

MINI_TARGET = cagent_mini.exe
MINI_SRC = cagent_mini.c

GUI_TARGET = cagent_gui.exe
GUI_SRC = cagent_gui.c
GUI_LDFLAGS = -mwindows -lcomctl32 -lwinhttp

.PHONY: all clean

all: $(MINI_TARGET) $(GUI_TARGET)

$(MINI_TARGET): $(MINI_SRC)
	$(CC) $(CFLAGS) -o $@ $<

$(GUI_TARGET): $(GUI_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(GUI_LDFLAGS)

clean:
	@rm -rf $(MINI_TARGET) $(GUI_TARGET) req.json resp.json tool_out.txt
