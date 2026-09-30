/*
 * core/exec.h - 命令执行: CreateProcess + 匿名管道 + 超时
 *
 * cagent 核心的一部分, 由 cagent_core.h 按依赖顺序聚合 (单 TU, 全 static)。
 */


/* 命令执行超时 (毫秒): 默认 60s, 可用环境变量 CAGENT_CMD_TIMEOUT (秒) 覆盖。 */
static int cmd_timeout_ms(void) {
    char *e = getenv("CAGENT_CMD_TIMEOUT");
    if (e && atoi(e) > 0) return atoi(e) * 1000;
    return 60000;
}

/* 用 CreateProcess + 匿名管道静默运行命令(仅本地工具用),纯内存收发数据。
 * 子进程的 stdout+stderr 合并写入 output (含 \0)。
 * 返回实际读到的字节数,失败返回 -1。超过 timeout 则终止整个进程树。 */
static size_t utf8_trim_len(const char *s, size_t len);   /* 前向声明 */

/* 追加一段提示到输出尾部: 空间不足时先回退内容(对齐字符边界), 保证提示一定可见 */
static void append_note(char *output, size_t *pos, size_t out_cap, const char *note) {
    size_t nl = strlen(note);
    if (*pos + nl + 1 > out_cap) {
        size_t cut = (out_cap > nl + 1) ? (out_cap - nl - 1) : 0;
        *pos = utf8_trim_len(output, cut);
    }
    memcpy(output + *pos, note, nl);
    *pos += nl;
    output[*pos] = '\0';
}

static int run_pipe(const char *cmdline, char *output, size_t out_cap) {
    if (out_cap) memset(output, 0, out_cap);   /* 清空, 避免上一轮残留泄漏 */
    HANDLE outR = NULL, outW = NULL;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

    if (!CreatePipe(&outR, &outW, &sa, 0)) return -1;
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = outW;
    si.hStdError  = outW;

    /* 命令行必须转宽字符交给 CreateProcessW, 否则中文参数会被按 ANSI 解释 */
    char buf[16384];
    snprintf(buf, sizeof(buf), "cmd /c %s", cmdline);
    wchar_t wbuf[16384];
    if (MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, 16384) <= 0) {
        CloseHandle(outR); CloseHandle(outW);
        return -1;
    }

    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessW(NULL, wbuf, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(outR); CloseHandle(outW);
        return -1;
    }
    CloseHandle(outW);

    /* 作业对象: 超时统一杀死进程树 (含 cmd 派生的子进程) */
    HANDLE hJob = NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
    memset(&jeli, 0, sizeof(jeli));
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    hJob = CreateJobObjectA(NULL, NULL);
    if (hJob && SetInformationJobObject(hJob, JobObjectExtendedLimitInformation,
                                        &jeli, sizeof(jeli))) {
        AssignProcessToJobObject(hJob, pi.hProcess); /* 失败则退化为仅杀主进程 */
    }

    size_t pos = 0;
    DWORD nread;
    char tmp[8192];
    int timedout = 0, truncated = 0;
    int timeout = cmd_timeout_ms();
    DWORD deadline = GetTickCount() + (DWORD)timeout;

    /* 持续抽干管道直到子进程退出或超时。
     * 关键 1: 即使输出缓冲已满也必须继续读并丢弃多余数据, 否则子进程写满管道后会阻塞,
     *         而下方 WaitForSingleObject(INFINITE) 将永远等不到它 -> 死锁。
     * 关键 2: PeekNamedPipe 在写端关闭后即失败(即使管道内仍有缓冲数据), 因此失败时必须
     *         改用 ReadFile 读净残留, 否则"写得快、退出快"的命令会整段丢输出。 */
    for (;;) {
        if (GetTickCount() >= deadline) { timedout = 1; break; }
        DWORD avail = 0;
        if (!PeekNamedPipe(outR, NULL, 0, NULL, &avail, NULL)) {
            /* 写端已关闭: 读净缓冲中剩余数据, 读完即结束 */
            if (!ReadFile(outR, tmp, sizeof(tmp), &nread, NULL) || nread == 0) break;
        } else if (avail == 0) {
            Sleep(15);
            continue;
        } else {
            DWORD toread = (avail > sizeof(tmp)) ? (DWORD)sizeof(tmp) : avail;
            if (!ReadFile(outR, tmp, toread, &nread, NULL) || nread == 0) break;
        }
        size_t room = (out_cap > pos + 1) ? (out_cap - 1 - pos) : 0;
        size_t copy = (nread < room) ? nread : room;
        if (copy < nread) truncated = 1;          /* 超限部分丢弃 */
        if (copy) { memcpy(output + pos, tmp, copy); pos += copy; }
    }
    /* 缓冲可能在多字节字符中间被截断: 回退到字符边界, 保证输出是合法 UTF-8 */
    pos = utf8_trim_len(output, pos);
    output[pos] = '\0';

    if (truncated) {
        append_note(output, &pos, out_cap,
                    "\n...(输出过长已截断; 需要完整结果请改用更精确的命令, "
                    "如 findstr 过滤或重定向到文件后分段读取)");
    }

    if (timedout) {
        if (hJob) TerminateJobObject(hJob, 1);
        else TerminateProcess(pi.hProcess, 1);
        append_note(output, &pos, out_cap, "\n(命令执行超时, 已终止)");
        pos = strlen(output);   /* 让返回值 > 0, 超时提示不被当作"无输出" */
    }

    WaitForSingleObject(pi.hProcess, timedout ? 2000 : INFINITE);
    if (hJob) CloseHandle(hJob);
    CloseHandle(outR);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)pos;
}

/* 把 WinHTTP/系统错误码翻译为中文可读文本。 */
