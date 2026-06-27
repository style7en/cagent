# cagent 功能增强 实现计划(阶段二 - 子项目 2)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给 cagent_gui 加对话历史持久化 + read_file/write_file/list_dir/search 四工具,并把工具调用改为按名分发。

**Architecture:** 仍单文件。历史持久化用 `cagent_history.json` 直接存 `messages` 字符串(每轮 done 后保存/启动加载)。4 个 `tool_*` 函数写入全局 `tool_out`。agent_thread 工具调用从硬编码 `execute_bash` 改为按 `c->name` 分发。手动测试验证(无新 selftest)。

**Tech Stack:** C(MinGW-w64)、Win32、WinHTTP、DPAPI、单文件。

**对应 spec:** `docs/superpowers/specs/2026-06-28-cagent-functionality-design.md`

---

## 文件结构

- **Modify** `cagent_gui.c` —— 历史函数、WM_CREATE 加载、agent_thread done 保存、ID_CLEAR/watermark 删历史、4 工具函数、TOOLS_JSON 扩展、agent_thread 分发
- **Modify** `README.md` —— 工具列表、历史持久化说明

---

### Task 1: 对话历史持久化

**Files:**
- Modify: `cagent_gui.c`(config 函数区加历史函数、`WM_CREATE`、`agent_thread` done、`ID_CLEAR`、watermark)

- [ ] **Step 1: 加 get_history_path / history_save / history_load 函数**

在 `config_load` 函数之前(或 `get_ini_path` 之后)插入:

```c
/* 取 exe 同目录下的 cagent_history.json 绝对路径 */
static void get_history_path(char *out, size_t cap) {
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { snprintf(out, cap, "cagent_history.json"); return; }
    char *slash = strrchr(exe, '\\');
    if (slash) *(slash + 1) = '\0';
    else exe[0] = '\0';
    snprintf(out, cap, "%scagent_history.json", exe);
}

/* 把 messages 写入历史文件 (失败静默) */
static void history_save(void) {
    char path[MAX_PATH];
    get_history_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(messages, f);
    fclose(f);
}

/* 读历史文件到 messages, 成功返回 1, 失败/不存在返回 0 */
static int history_load(void) {
    char path[MAX_PATH];
    get_history_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(messages, 1, BUFSZ - 1, f);
    fclose(f);
    if (n == 0) return 0;
    messages[n] = '\0';
    return 1;
}
```

- [ ] **Step 2: WM_CREATE 中加载历史(替换 reset_conversation 调用)**

定位 `WM_CREATE` 中(历史框已创建之后):
```c
        /* 初始化对话历史(只有 system prompt) */
        reset_conversation();

        /* DPAPI 解密失败提示 (config_load 在历史框创建前执行, 延迟到此处显示) */
```
改为:
```c
        /* 加载历史对话; 失败则初始化为只含 system prompt */
        if (!history_load()) reset_conversation();
        else append_text("(已恢复历史对话, 可继续)\r\n");

        /* DPAPI 解密失败提示 (config_load 在历史框创建前执行, 延迟到此处显示) */
```

- [ ] **Step 3: agent_thread done 前保存历史**

定位 `agent_thread` 的 `done:` 标签:
```c
done:
    if (rolled_back) {
        messages[savepoint] = '\0';
    }
    free(task);
    PostMessage(g_hHistory, WM_APP_DONE, 0, 0);
    return 0;
```
改为:
```c
done:
    if (rolled_back) {
        messages[savepoint] = '\0';
    }
    history_save();   /* 每轮 done 后保存 (含回滚后状态) */
    free(task);
    PostMessage(g_hHistory, WM_APP_DONE, 0, 0);
    return 0;
```

- [ ] **Step 4: ID_CLEAR 清空时删历史文件**

定位 `WM_COMMAND` 的 `ID_CLEAR` 分支:
```c
            if (!g_running) {  /* 任务运行中不允许清空 */
                reset_conversation();
                SetWindowTextW(g_hHistory, L"");
                SetFocus(g_hInput);
            }
```
改为:
```c
            if (!g_running) {  /* 任务运行中不允许清空 */
                reset_conversation();
                char hpath[MAX_PATH];
                get_history_path(hpath, sizeof(hpath));
                DeleteFileA(hpath);   /* 同步删除历史文件 */
                SetWindowTextW(g_hHistory, L"");
                SetFocus(g_hInput);
            }
```

- [ ] **Step 5: watermark 自动清空时删历史文件**

定位 `agent_thread` 开头的 watermark 检测:
```c
    if (strlen(messages) > MESSAGES_WATERMARK) {
        append_text("(对话历史过长, 已自动清空上下文)\r\n");
        reset_conversation();
    }
```
改为:
```c
    if (strlen(messages) > MESSAGES_WATERMARK) {
        append_text("(对话历史过长, 已自动清空上下文)\r\n");
        reset_conversation();
        char hpath[MAX_PATH];
        get_history_path(hpath, sizeof(hpath));
        DeleteFileA(hpath);
    }
```

- [ ] **Step 6: 编译 + selftest 回归**

Run: `make && ./cagent_gui.exe --selftest; echo exit=$?`
Expected: 零警告;selftest OK(exit 0)。

- [ ] **Step 7: 提交**

```bash
git add cagent_gui.c
git commit -m "feat: 对话历史持久化 (cagent_history.json, 每轮保存/启动加载)"
```

---

### Task 2: 4 个工具函数

**Files:**
- Modify: `cagent_gui.c`(在 `execute_bash` 之后插入 4 个 `tool_*` 函数)

- [ ] **Step 1: 加 tool_read_file / tool_write_file / tool_list_dir / tool_search**

在 `execute_bash` 函数之后插入:

```c
/* ===== 结构化工具 (写入全局 tool_out) ===== */

static void tool_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(tool_out, BUFSZ, "(读取失败: 无法打开 %s)", path); return; }
    size_t n = fread(tool_out, 1, BUFSZ - 64, f);
    fclose(f);
    tool_out[n] = '\0';
    if (n >= BUFSZ - 64) {
        strcat(tool_out, "\n(已截断, 文件过大)");
    }
    oem_to_utf8(tool_out, BUFSZ);
}

static void tool_write_file(const char *path, const char *content) {
    /* 覆盖确认: 文件已存在则弹窗 (工作线程, 同命令沙箱) */
    FILE *test = fopen(path, "rb");
    if (test) {
        fclose(test);
        int rc = MessageBoxW(NULL, L"文件已存在, 确认覆盖?", L"write_file 确认",
                             MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (rc != IDYES) { strcpy(tool_out, "(用户拒绝覆盖)"); return; }
    }
    FILE *f = fopen(path, "wb");
    if (!f) { strcpy(tool_out, "(写入失败)"); return; }
    size_t len = strlen(content);
    fwrite(content, 1, len, f);
    fclose(f);
    snprintf(tool_out, BUFSZ, "(已写入 %zu 字节)", len);
}

static void tool_list_dir(const char *path) {
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { snprintf(tool_out, BUFSZ, "(列目录失败: %s)", path); return; }
    size_t pos = 0;
    int count = 0;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        const char *type = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "<DIR>" : "file";
        char line[512];
        snprintf(line, sizeof(line), "%s\t%llu\t%s\n", fd.cFileName,
                 (unsigned long long)fd.nFileSizeLow, type);
        size_t ln = strlen(line);
        if (pos + ln + 32 >= BUFSZ) { strcat(tool_out, "(更多条目已截断)"); break; }
        memcpy(tool_out + pos, line, ln);
        pos += ln;
        if (++count >= 200) { strcat(tool_out, "(更多条目已截断)"); break; }
    } while (FindNextFileA(h, &fd));
    tool_out[pos] = '\0';
    FindClose(h);
}

static void tool_search(const char *pattern, const char *path) {
    char glob[MAX_PATH];
    snprintf(glob, sizeof(glob), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) { snprintf(tool_out, BUFSZ, "(搜索失败: %s)", path); return; }
    size_t pos = 0;
    int matches = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;  /* 非递归: 跳过子目录 */
        char fpath[MAX_PATH];
        snprintf(fpath, sizeof(fpath), "%s\\%s", path, fd.cFileName);
        FILE *f = fopen(fpath, "rb");
        if (!f) continue;
        char *buf = (char*)malloc(256 * 1024);
        if (!buf) { fclose(f); continue; }
        size_t n = fread(buf, 1, 256 * 1024 - 1, f);
        fclose(f);
        if (n == 0) { free(buf); continue; }
        buf[n] = '\0';
        char *line = buf;
        while (line < buf + n && matches < 50) {
            char *eol = strchr(line, '\n');
            int linelen = eol ? (int)(eol - line) : (int)(buf + n - line);
            char save = line[linelen];
            line[linelen] = '\0';
            if (strstr(line, pattern)) {
                char ml[1024];
                snprintf(ml, sizeof(ml), "%s: %s\n", fd.cFileName, line);
                size_t mlen = strlen(ml);
                if (pos + mlen + 32 >= BUFSZ) {
                    pos += snprintf(tool_out + pos, BUFSZ - pos, "(更多匹配已截断)");
                    line[linelen] = save;
                    goto done;
                }
                memcpy(tool_out + pos, ml, mlen);
                pos += mlen;
                matches++;
            }
            line[linelen] = save;
            if (!eol) break;
            line = eol + 1;
        }
        free(buf);
        if (matches >= 50) { pos += snprintf(tool_out + pos, BUFSZ - pos, "(更多匹配已截断)"); break; }
    } while (FindNextFileA(h, &fd));
done:
    tool_out[pos] = '\0';
    FindClose(h);
    if (matches == 0 && pos == 0) strcpy(tool_out, "(无匹配)");
}
```

注意:`tool_search` 里 `goto done` 跳出双层循环,`done:` 标签前需确保 `buf` 已 free(在 goto 前已 `line[linelen]=save` 但未 free buf)。修正:goto 前先 `free(buf)`。把 goto 前改为:
```c
                if (pos + mlen + 32 >= BUFSZ) {
                    pos += snprintf(tool_out + pos, BUFSZ - pos, "(更多匹配已截断)");
                    line[linelen] = save;
                    free(buf);
                    goto done;
                }
```

- [ ] **Step 2: 编译**

Run: `make`
Expected: 零警告。

- [ ] **Step 3: 提交**

```bash
git add cagent_gui.c
git commit -m "feat: 新增 read_file/write_file/list_dir/search 工具函数"
```

---

### Task 3: TOOLS_JSON 扩展 + agent_thread 工具分发

**Files:**
- Modify: `cagent_gui.c`(`TOOLS_JSON`、agent_thread 执行循环)

- [ ] **Step 1: TOOLS_JSON 追加 4 工具定义**

定位 `TOOLS_JSON` 定义(以 `execute_bash` 结尾):
```c
static const char *TOOLS_JSON =
    "[{\"type\":\"function\",\"function\":{"
    "\"name\":\"execute_bash\","
    "\"description\":\"Execute a shell command via cmd /c\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"]}}}]";
```
替换为(在 `}]}]` 前追加 4 个):
```c
static const char *TOOLS_JSON =
    "[{\"type\":\"function\",\"function\":{"
    "\"name\":\"execute_bash\","
    "\"description\":\"Execute a shell command via cmd /c\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"command\":{\"type\":\"string\"}},"
    "\"required\":[\"command\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"read_file\","
    "\"description\":\"Read text content of a file\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"write_file\","
    "\"description\":\"Write content to a file (overwrite)\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"},\"content\":{\"type\":\"string\"}},"
    "\"required\":[\"path\",\"content\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"list_dir\","
    "\"description\":\"List directory entries (name, size, type)\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"path\"]}}},"
    "{\"type\":\"function\",\"function\":{"
    "\"name\":\"search\","
    "\"description\":\"Search pattern in files under a directory (non-recursive)\","
    "\"parameters\":{\"type\":\"object\","
    "\"properties\":{\"pattern\":{\"type\":\"string\"},\"path\":{\"type\":\"string\"}},"
    "\"required\":[\"pattern\",\"path\"]}}}]";
```

- [ ] **Step 2: agent_thread 工具分发(替换 execute_bash 硬编码)**

定位 agent_thread"执行所有工具"循环内,`[Tool]` 显示之后、`c->output = strdup(tool_out)` 之前的部分:
```c
            char command[4096] = "";
            JValue *argsj = json_parse(c->args);
            if (argsj) {
                const char *cmd = json_as_str(json_obj_get(argsj, "command"));
                if (cmd) snprintf(command, sizeof(command), "%s", cmd);
                json_free(argsj);
            }
            execute_bash(command);
            c->output = strdup(tool_out);
```
替换为(按名分发):
```c
            JValue *argsj = json_parse(c->args);
            if (strcmp(c->name, "execute_bash") == 0) {
                const char *cmd = json_as_str(json_obj_get(argsj, "command"));
                execute_bash(cmd ? cmd : "");
            } else if (strcmp(c->name, "read_file") == 0) {
                const char *p = json_as_str(json_obj_get(argsj, "path"));
                tool_read_file(p ? p : "");
            } else if (strcmp(c->name, "write_file") == 0) {
                const char *p = json_as_str(json_obj_get(argsj, "path"));
                const char *ct = json_as_str(json_obj_get(argsj, "content"));
                tool_write_file(p ? p : "", ct ? ct : "");
            } else if (strcmp(c->name, "list_dir") == 0) {
                const char *p = json_as_str(json_obj_get(argsj, "path"));
                tool_list_dir(p ? p : "");
            } else if (strcmp(c->name, "search") == 0) {
                const char *pat = json_as_str(json_obj_get(argsj, "pattern"));
                const char *p = json_as_str(json_obj_get(argsj, "path"));
                tool_search(pat ? pat : "", p ? p : "");
            } else {
                strcpy(tool_out, "(未知工具)");
            }
            json_free(argsj);
            c->output = strdup(tool_out);
```

- [ ] **Step 3: 编译 + selftest 回归**

Run: `make && ./cagent_gui.exe --selftest; echo exit=$?`
Expected: 零警告;selftest OK。

- [ ] **Step 4: 提交**

```bash
git add cagent_gui.c
git commit -m "feat: TOOLS_JSON 扩展 4 工具 + agent_thread 按名分发"
```

---

### Task 4: README 补充

**Files:**
- Modify: `README.md`

- [ ] **Step 1: 工具列表补 4 工具**

定位"### 工具"小节:
```markdown
目前只内置:

- `execute_bash(command)` — 通过 `cmd /c` 执行命令并捕获输出
```
改为:
```markdown
目前内置:

- `execute_bash(command)` — 通过 `cmd /c` 执行命令并捕获输出(危险命令弹窗确认)
- `read_file(path)` — 读取文件文本内容(超大截断)
- `write_file(path, content)` — 写入文件(已存在时弹窗确认覆盖)
- `list_dir(path)` — 列出目录条目(名/大小/类型,超 200 截断)
- `search(pattern, path)` — 在目录下文件内容中搜索(非递归,超 50 匹配截断)
```

- [ ] **Step 2: 技术特性表补历史持久化**

定位技术特性表"命令沙箱"行之后:
```markdown
| 命令沙箱 | 危险命令(rm/del/format 等)执行前弹窗确认,拒绝则把结果交回模型 |
```
在其后追加:
```markdown
| 命令沙箱 | 危险命令(rm/del/format 等)执行前弹窗确认,拒绝则把结果交回模型 |
| 对话历史持久化 | 每轮 done 后保存 `cagent_history.json`,启动加载恢复 LLM 上下文 |
```

- [ ] **Step 3: 常见问题补历史恢复说明**

定位常见问题"模型要删文件时弹了确认框"Q 之后、`---` 之前:
```markdown
**Q: 模型要删文件时弹了确认框?**
A: 命令沙箱拦截了危险命令(rm/del/format 等)。选"否"会把"(用户拒绝执行)"返回模型,模型可改用其他方案。

---
```
改为:
```markdown
**Q: 模型要删文件时弹了确认框?**
A: 命令沙箱拦截了危险命令(rm/del/format 等)。选"否"会把"(用户拒绝执行)"返回模型,模型可改用其他方案。

**Q: 重启后历史框没有显示之前的对话?**
A: 历史持久化只恢复 LLM 上下文(`messages`),不重建历史框显示。历史框会提示"已恢复历史对话",继续对话时模型仍记得之前内容。点"清空对话"会删除历史文件。

---
```

- [ ] **Step 4: 提交**

```bash
git add README.md
git commit -m "docs: 补充历史持久化与新工具说明"
```

---

### Task 5: 全量验证

**Files:** 无修改,仅验证

- [ ] **Step 1: 全量构建**

Run: `make clean && make`
Expected: 零警告。

- [ ] **Step 2: selftest 回归**

Run: `./cagent_gui.exe --selftest; echo exit=$?`
Expected: `json_selftest: OK`,exit 0。

- [ ] **Step 3: 手动测试矩阵**

| # | 操作 | 预期 |
|---|---|---|
| 1 | 对话几轮 → 关闭 → 重启 | 历史框"已恢复历史对话";继续对话 LLM 有上下文 |
| 2 | 每轮后查 `cagent_history.json` | 内容随轮增长;回滚后存回滚后状态 |
| 3 | 清空对话 → 重启 | 文件删除,无恢复 |
| 4 | watermark 自动清空 → 重启 | 不加载旧历史 |
| 5 | read_file 读小文件 | 返回内容;超大 → 截断提示 |
| 6 | write_file 新文件 | 直接写,返回字节数 |
| 7 | write_file 已存在文件 | 弹确认;否→拒绝;是→覆盖 |
| 8 | list_dir 列目录 | 名/大小/类型,超 200 截断 |
| 9 | search 搜关键词 | 匹配 `文件:行`,超 50 截断 |
| 10 | 模型并行调用多工具 | 各工具独立结果,1:1 配对 |

- [ ] **Step 4: 完成确认**

矩阵 1–10 全通过 + 零警告 + selftest 全绿 → 功能子项目完成。
