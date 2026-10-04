# -*- coding: utf-8 -*-
"""test/e2e_ui.py - cagent GUI 端到端自动化测试 (模拟真实用户点击/键入)

不 mock 任何 cagent 内部: 启动真正的 cagent.exe, 用 SendInput 发送真实的
鼠标点击与键盘输入, 对话打到本地 mock 服务 (mock_server.py), 断言依据:
  1. cagent 运行日志 (log/cagent_*.log) 的关键行
  2. 历史框/输入框/按钮文本 (跨进程 WM_GETTEXT, SendMessage 会为该消息封送缓冲)
  3. mock 服务收到的请求 (次数 / 最后请求体)
  4. PrintWindow 截图存档 (供人工复核, 不做像素断言)

每个用例独立: 独立临时目录 (exe 副本 + ini + 工作目录) + 独立 cagent 进程。
用法: python e2e_ui.py [用例名过滤]     (无参 = 全部)
"""
import ctypes
from ctypes import wintypes, c_void_p, c_int, c_uint, c_long, c_ulong, POINTER, Structure, byref
import glob
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request

# ================================================================ Win32 绑定
user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32
gdi32 = ctypes.windll.gdi32

# 本进程也要 PMv2: 否则 GetWindowRect 返回的是虚拟化坐标, 点击会错位
user32.SetProcessDpiAwarenessContext(c_void_p(-4))

WM_GETTEXT = 0x000D
WM_GETTEXTLENGTH = 0x000E
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
VK_RETURN = 0x0D
INPUT_MOUSE = 0
INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
PW_RENDERFULLCONTENT = 2
SRCCOPY = 0x00CC0020
DIB_RGB_COLORS = 0
BI_RGB = 0

ID_HISTORY, ID_INPUT, ID_SEND = 1001, 1002, 1003
ID_NEW_BTN, ID_CFG_BASE = 1004, 1005          # cfg: 1005 url, 1006 key, 1007 model
ID_WS_EDIT = 1012

# 输入注入模式:
#   msg       (默认) 消息级: 把鼠标/键盘事件 PostMessage 到目标控件, 走与真实点击
#             完全相同的窗口过程路径 (WM_LBUTTONDOWN->BN_CLICKED / WM_KEYDOWN 回车 /
#             WM_CHAR 文本)。适用于无人值守/锁屏/远程会话 —— 硬件注入无法夺前台时。
#   sendinput 硬件级: SendInput 真实键鼠 (要求桌面可交互、能夺前台)。
#             在自己操作的桌面上跑 E2E_INPUT=sendinput 可获得完全硬件级仿真。
MODE = os.environ.get("E2E_INPUT", "msg")

WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_CHAR = 0x0102
MK_LBUTTON = 0x0001

PORT = 8765
BASE_URL = "http://127.0.0.1:%d/v1" % PORT
MOCK = "http://127.0.0.1:%d" % PORT
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # 仓库根
EXE_SRC = os.path.join(REPO, "cagent.exe")
ART = os.path.join(REPO, "test", "e2e_artifacts")


class RECT(Structure):
    _fields_ = [("left", c_long), ("top", c_long), ("right", c_long), ("bottom", c_long)]


class KEYBDINPUT(Structure):
    class _U(Structure):
        _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD),
                    ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                    ("dwExtraInfo", c_void_p)]
    _anonymous_ = ("u",)
    _fields_ = [("type", wintypes.DWORD), ("u", _U)]


class MOUSEINPUT(Structure):
    _fields_ = [("dx", c_long), ("dy", c_long), ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD),
                ("dwExtraInfo", c_void_p)]


class INPUTunion(Structure):
    class _U(Structure):
        _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT)]
    _anonymous_ = ("u",)
    _fields_ = [("u", _U)]


class INPUT(Structure):
    _fields_ = [("type", wintypes.DWORD), ("u", INPUTunion)]


def send_input(inputs):
    arr = (INPUT * len(inputs))(*inputs)
    user32.SendInput(len(arr), arr, ctypes.sizeof(INPUT))


def send_key(vk, up=False):
    ki = KEYBDINPUT(type=INPUT_KEYBOARD)
    ki.u.wVk = vk
    if up:
        ki.u.dwFlags = KEYEVENTF_KEYUP
    send_input([INPUT(type=INPUT_KEYBOARD, u=INPUTunion(ki=ki))])


def type_text(s):
    """真实键盘输入 (UNICODE 事件流, 无需 IME)"""
    seq = []
    for ch in s:
        ki = KEYBDINPUT(type=INPUT_KEYBOARD)
        ki.u.wVk = 0
        ki.u.wScan = ord(ch)
        ki.u.dwFlags = KEYEVENTF_UNICODE
        seq.append(INPUT(type=INPUT_KEYBOARD, u=INPUTunion(ki=ki)))
        ku = KEYBDINPUT(type=INPUT_KEYBOARD)
        ku.u.wVk = 0
        ku.u.wScan = ord(ch)
        ku.u.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP
        seq.append(INPUT(type=INPUT_KEYBOARD, u=INPUTunion(ki=ku)))
    send_input(seq)


def press_enter():
    send_key(VK_RETURN)
    time.sleep(0.05)
    send_key(VK_RETURN, up=True)


def click_at(x, y):
    # 防误点: 落点必须在 cagent 窗口 (或其子控件) 上, 否则拒绝点击
    pt = wintypes.POINT(int(x), int(y))
    user32.WindowFromPoint.restype = wintypes.HWND
    hit = user32.WindowFromPoint(pt)
    if not hit:
        raise RuntimeError("点击点 (%d,%d) 无窗口" % (x, y))
    user32.SetCursorPos(int(x), int(y))
    time.sleep(0.08)
    mi_d = MOUSEINPUT(dx=0, dy=0, mouseData=0, dwFlags=MOUSEEVENTF_LEFTDOWN,
                      time=0, dwExtraInfo=None)
    send_input([INPUT(type=INPUT_MOUSE, u=INPUTunion(mi=mi_d))])
    time.sleep(0.06)
    mi_u = MOUSEINPUT(dx=0, dy=0, mouseData=0, dwFlags=MOUSEEVENTF_LEFTUP,
                      time=0, dwExtraInfo=None)
    send_input([INPUT(type=INPUT_MOUSE, u=INPUTunion(mi=mi_u))])
    time.sleep(0.1)


# ================================================================ cagent 进程包装
class App:
    def __init__(self, scenario, name, env_extra=None):
        self.name = name
        self.dir = os.path.join(ART, time.strftime("%H%M%S") + "_" + name)
        os.makedirs(os.path.join(self.dir, "ws"), exist_ok=True)
        shutil.copy2(EXE_SRC, os.path.join(self.dir, "cagent.exe"))
        ini = ("url_base=%s\napi_key=%s\nmodel=mock-1\n"
               "workspace=%s\nlog=1\n" % (BASE_URL, scenario, os.path.join(self.dir, "ws")))
        with open(os.path.join(self.dir, "cagent.ini"), "w", encoding="utf-8") as f:
            f.write(ini)
        urllib.request.urlopen("%s/reset?token=%s" % (MOCK, scenario), timeout=5).read()
        env = dict(os.environ)
        if env_extra:
            env.update(env_extra)
        exe = os.path.join(self.dir, "cagent.exe")
        # 启动偶发抖动 (杀软扫新拷贝的 exe / 上个进程收尾竞争), 最多重启一次
        for attempt in range(2):
            self.proc = subprocess.Popen([exe], cwd=self.dir, env=env)
            self.hwnd = None
            self.ctrls = {}
            deadline = time.time() + 20
            while time.time() < deadline:
                self.hwnd = user32.FindWindowW("CagentGuiWnd", None)
                # 必须匹配本进程 PID: 上一个被 kill 的进程残窗未消失时会拿到旧窗口
                if self.hwnd:
                    pid = wintypes.DWORD()
                    user32.GetWindowThreadProcessId(self.hwnd, byref(pid))
                    if pid.value != self.proc.pid:
                        self.hwnd = None
                if self.hwnd and self._enum():
                    break
                self.hwnd = None
                time.sleep(0.15)
            if self.hwnd:
                break
            self.proc.kill()
            self.proc.wait(timeout=5)
            if attempt == 0:
                print("   (窗口未出现, 重启重试)", flush=True)
        if not self.hwnd:
            raise RuntimeError("cagent 窗口未出现 (含一次重启重试)")
        # 置顶并挪到固定位置: 窗口在后台时点击会落到底下用户的其他窗口 —— 绝不允许
        user32.SetWindowPos(self.hwnd, -1, 60, 60, 0, 0, 0x1 | 0x40)   # TOPMOST, NOSIZE|SHOWWINDOW
        time.sleep(0.2)
        self.activate()

    # ---- 控件枚举 ----
    def _enum(self):
        out = []
        proto = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)

        def cb(h, lp):
            out.append(h)
            return True
        user32.EnumChildWindows(self.hwnd, proto(cb), 0)
        self.ctrls = {}
        for h in out:
            cid = user32.GetDlgCtrlID(h)
            r = RECT()
            user32.GetWindowRect(h, byref(r))
            self.ctrls[cid] = (h, r)
        return ID_INPUT in self.ctrls and ID_SEND in self.ctrls

    def activate(self):
        user32.ShowWindow(self.hwnd, 9)   # SW_RESTORE
        # ALT 键 trick: 让系统认为刚有用户输入, 解除 SetForegroundWindow 的前台锁
        send_key(0x12)                    # VK_MENU down
        user32.SetForegroundWindow(self.hwnd)
        send_key(0x12, up=True)
        time.sleep(0.2)
        # 仍不前台则用 AttachThreadInput 兜底
        if user32.GetForegroundWindow() != self.hwnd:
            pid = wintypes.DWORD()
            cur = kernel32.GetCurrentThreadId()
            other = user32.GetWindowThreadProcessId(self.hwnd, byref(pid))
            user32.AttachThreadInput(cur, other, True)
            user32.SetForegroundWindow(self.hwnd)
            user32.AttachThreadInput(cur, other, False)
            time.sleep(0.15)
        return user32.GetForegroundWindow() == self.hwnd

    def _center(self, cid):
        h, r = self.ctrls[cid]
        return (r.left + r.right) // 2, (r.top + r.bottom) // 2

    def click(self, cid):
        h, r = self.ctrls[cid]
        cx = (r.right - r.left) // 2
        cy = (r.bottom - r.top) // 2
        if MODE == "msg":
            # 消息级: 客户区坐标直接投递, 走控件自己的窗口过程 (与真实点击同路径)
            lp = (cy & 0xFFFF) << 16 | (cx & 0xFFFF)
            user32.PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, lp)
            time.sleep(0.05)
            user32.PostMessageW(h, WM_LBUTTONUP, 0, lp)
            time.sleep(0.1)
        else:
            self.activate()
            x, y = self._center(cid)
            pt = wintypes.POINT(x, y)
            user32.WindowFromPoint.restype = wintypes.HWND
            hit = user32.WindowFromPoint(pt)
            if hit != self.ctrls[cid][0] and user32.GetAncestor(hit, 2) != self.hwnd:
                raise RuntimeError("点击点 (%d,%d) 被 %r 遮挡, 拒绝点击" % (x, y, hit))
            click_at(x, y)

    def click_input_and_type(self, text):
        self.click(ID_INPUT)
        self.type_text(text)
        time.sleep(0.1)

    def type_text(self, text):
        if MODE == "msg":
            for ch in text:
                user32.PostMessageW(self.ctrls[ID_INPUT][0], WM_CHAR, ord(ch), 0)
                time.sleep(0.01)
        else:
            type_text(text)
            time.sleep(0.1)

    def type_text_fast(self, text, chunk=1000, pause=0.35):
        """大批量键入: 不逐字 sleep。PostMessage 队列上限 1 万, 每投 chunk 字歇一次
        让 UI 线程抽干; 队列满时 PostMessage 静默失败, 必须检测并重试 (实测 1000/块零丢字)"""
        if MODE != "msg":
            type_text(text)
            return
        h = self.ctrls[ID_INPUT][0]
        for i, ch in enumerate(text):
            n = 0
            while not user32.PostMessageW(h, WM_CHAR, ord(ch), 0):
                n += 1
                if n > 40:
                    raise RuntimeError("输入队列持续溢出, 丢字")
                time.sleep(0.05)
            if (i + 1) % chunk == 0:
                time.sleep(pause)
        time.sleep(0.2)

    def window_rect(self):
        r = RECT()
        user32.GetWindowRect(self.hwnd, byref(r))
        return r

    def resize(self, w, hgt):
        """真实路径: SetWindowPos -> WM_SIZE -> layout() 重排全部子控件"""
        user32.SetWindowPos(self.hwnd, 0, 0, 0, int(w), int(hgt), 0x4 | 0x2)   # NOZORDER|NOMOVE
        time.sleep(0.3)
        self._enum()
    # 注: WM_DPICHANGED 无法跨进程注入 (系统保留消息, Send/Post 均被 user32 静默拦截),
    # DPI 切换用例经 CAGENT_SIMULATE_DPI 环境变量驱动 cagent 自发送, 见 T021。

    def press_enter(self):
        if MODE == "msg":
            h = self.ctrls[ID_INPUT][0]
            # 输入框子类 InputProc 拦 WM_KEYDOWN VK_RETURN 触发发送 ( lParam: 扫描码 0x1C )
            user32.PostMessageW(h, WM_KEYDOWN, VK_RETURN, 0x001C0001)
            time.sleep(0.05)
            user32.PostMessageW(h, WM_KEYUP, VK_RETURN, 0xC01C0001)
        else:
            press_enter()

    def send_message(self, text, use_button=False):
        if use_button:
            self.click_input_and_type(text)
            self.click(ID_SEND)
        else:
            self.click_input_and_type(text)
            self.press_enter()

    def text(self, cid, cap=65536):
        h = self.ctrls[cid][0]
        buf = ctypes.create_unicode_buffer(cap)
        n = user32.SendMessageTimeoutW(h, WM_GETTEXT, cap, buf, 2, 2000, None)
        return buf.value if n is not None else ""

    # ---- 截图 (PrintWindow -> BMP 存档) ----
    def screenshot(self, fname):
        hwnd = self.hwnd
        r = RECT()
        user32.GetWindowRect(hwnd, byref(r))
        w, hgt = r.right - r.left, r.bottom - r.top
        hdc = user32.GetDC(0)
        mem = gdi32.CreateCompatibleDC(hdc)
        bmp = gdi32.CreateCompatibleBitmap(hdc, w, hgt)
        old = gdi32.SelectObject(mem, bmp)
        user32.PrintWindow(hwnd, mem, PW_RENDERFULLCONTENT)
        # BMP 文件头
        class BMIH(Structure):
            _fields_ = [("biSize", wintypes.DWORD), ("biWidth", c_long),
                        ("biHeight", c_long), ("biPlanes", wintypes.WORD),
                        ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                        ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", c_long),
                        ("biYPelsPerMeter", c_long), ("biClrUsed", wintypes.DWORD),
                        ("biClrImportant", wintypes.DWORD)]
        bi = BMIH(ctypes.sizeof(BMIH), w, -hgt, 1, 32, BI_RGB, 0, 0, 0, 0, 0)
        bufsize = w * hgt * 4
        buf = ctypes.create_string_buffer(bufsize)
        gdi32.GetDIBits(mem, bmp, 0, hgt, buf, byref(bi), DIB_RGB_COLORS)
        path = os.path.join(self.dir, fname)
        with open(path, "wb") as f:
            f.write(b"BM")
            f.write((14 + 40 + bufsize).to_bytes(4, "little"))
            f.write(b"\x00\x00\x00\x00" + (54).to_bytes(4, "little"))
            f.write(bytes(bi))
            f.write(buf.raw)
        gdi32.SelectObject(mem, old)
        gdi32.DeleteObject(bmp)
        gdi32.DeleteDC(mem)
        user32.ReleaseDC(0, hdc)
        return path

    # ---- 日志 ----
    def log_text(self):
        out = ""
        for p in glob.glob(os.path.join(self.dir, "log", "*.log")):
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                out += f.read()
        return out

    def wait_log(self, needle, timeout=30):
        return self.wait(lambda: needle in self.log_text(), timeout,
                         "日志等待超时: %s" % needle)

    def wait_hist(self, needle, timeout=30):
        return self.wait(lambda: needle in self.text(ID_HISTORY), timeout,
                         "历史框等待超时: %s" % needle)

    def wait(self, fn, timeout, msg):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                if fn():
                    return True
            except Exception:
                pass
            time.sleep(0.25)
        raise AssertionError(msg + " (最近日志尾部: ...%s)" % self.log_text()[-400:])

    def close(self):
        if self.proc.poll() is None:
            # 先温和关闭并等窗口消失, 再 kill: 防残窗被下一个用例误认
            user32.PostMessageW(self.hwnd, 0x0010, 0, 0)   # WM_CLOSE
            deadline = time.time() + 5
            while time.time() < deadline:
                if user32.FindWindowW("CagentGuiWnd", None) != self.hwnd:
                    break
                time.sleep(0.1)
            if self.proc.poll() is None:
                self.proc.kill()
            self.proc.wait(timeout=5)
        deadline = time.time() + 3
        while time.time() < deadline and user32.FindWindowW("CagentGuiWnd", None):
            time.sleep(0.1)


# ---- mock 服务查询 ----
def mock_get(path):
    return json.loads(urllib.request.urlopen(MOCK + path, timeout=5).read())


def mock_count(token):
    return mock_get("/count?token=%s" % token)["count"]


def mock_last_body(token):
    return mock_get("/last_body?token=%s" % token)["body"]


# ================================================================ 测试用例
CASES = []   # (name, scenario, fn, env)


def case(scenario, env=None):
    def deco(fn):
        CASES.append((fn.__name__, scenario, fn, env))
        return fn
    return deco


@case("chat")
def T001_启动与配置回显(app):
    """窗口出现, 配置回显 ini 值, 截图存档 (Key 是密码框, 跨进程读不到, 不断言)"""
    assert app.text(ID_CFG_BASE) == BASE_URL, "Base Url 回显不符: %r" % app.text(ID_CFG_BASE)
    assert app.text(ID_CFG_BASE + 2) == "mock-1", "Model 回显不符"
    app.screenshot("T001_launch.bmp")


@case("chat")
def T002_回车发送_普通对话(app):
    """输入框键入 + 回车 -> AI 回复上屏 -> 按钮回到[发送]"""
    app.send_message("你好")
    app.wait_hist("你好，我是 mock 模型")
    app.wait(lambda: app.text(ID_SEND) == "发送", 10, "发送按钮未复位")
    assert mock_count("chat") == 1
    app.screenshot("T002_chat.bmp")


@case("chat2")
def T003_按钮发送_普通对话(app):
    """真实鼠标点击[发送]按钮 (不经回车)"""
    app.click_input_and_type("点按钮发送")
    app.click(ID_SEND)
    app.wait_hist("第二次对话回复 OK")


@case("tool")
def T004_工具往返_read_file(app):
    """模型发起 read_file -> agent 执行 -> 结果回传 -> 最终回复"""
    with open(os.path.join(app.dir, "ws", "note.txt"), "w", encoding="utf-8") as f:
        f.write("HELLO-FROM-MOCK")
    app.send_message("读取 note.txt")
    app.wait_hist("文件内容是 HELLO-FROM-MOCK", timeout=40)
    assert "iter=2" in app.log_text(), "日志无第二轮请求 (工具未触发往返)"
    body = mock_last_body("tool")
    toolmsg = [m for m in body["messages"] if m.get("role") == "tool"]
    assert toolmsg and "HELLO-FROM-MOCK" in toolmsg[-1]["content"], \
        "工具结果未回传给模型: %s" % json.dumps(body)[-500:]
    assert mock_count("tool") == 2, "工具往返应为 2 次请求, 实际 %d" % mock_count("tool")


@case("tool2")
def T005_并行工具调用(app):
    """一轮两个工具调用 -> 两个结果都回传"""
    for fn in ("a.txt", "b.txt"):
        with open(os.path.join(app.dir, "ws", fn), "w", encoding="utf-8") as f:
            f.write("内容" + fn)
    app.send_message("读 a.txt 和 b.txt")
    app.wait_hist("两个文件都读完了", timeout=40)
    body = mock_last_body("tool2")
    toolmsgs = [m for m in body["messages"] if m.get("role") == "tool"]
    assert len(toolmsgs) == 2, "应有 2 条工具结果, 实际 %d" % len(toolmsgs)


@case("drip")
def T006_点停止_取消会话(app):
    """慢流期间点[停止] -> (已取消) -> 按钮复位"""
    app.send_message("慢慢说")
    time.sleep(1.5)
    assert app.text(ID_SEND) == "停止", "运行中按钮应显示[停止]"
    app.click(ID_SEND)
    app.wait_hist("已取消", timeout=30)
    app.wait(lambda: app.text(ID_SEND) == "发送", 10, "取消后按钮未复位")


@case("err500x2")
def T007_服务端500重试后成功(app):
    """500×2 -> 指数退避 -> 第 3 次成功; 界面有[服务端错误]提示"""
    t0 = time.time()
    app.send_message("重试我")
    app.wait_hist("第三次尝试成功", timeout=40)
    log = app.log_text()
    assert log.count("[http] 瞬时错误重试") == 2, "应重试 2 次: %s" % log[-600:]
    assert "服务端错误" in app.text(ID_HISTORY), "界面应提示服务端错误重试"
    assert mock_count("err500x2") == 3
    assert time.time() - t0 > 5, "退避间隔应累计 >5s (2s+4s)"


@case("err429_ra")
def T008_限流429_尊重RetryAfter(app):
    """429 + Retry-After: 8 -> 等待 8s (服务端说了算) -> 恢复"""
    app.send_message("限流我")
    app.wait_hist("限流恢复后的回复", timeout=45)
    log = app.log_text()
    assert "wait=8000" in log, "未按 Retry-After 等 8s: %s" % log[-600:]
    assert "ra=8" in log
    assert "请求限流" in app.text(ID_HISTORY), "界面应提示[请求限流]"


@case("err429_quota")
def T009_额度用完_零重试(app):
    """429 + insufficient_quota -> 一次请求即停, 界面[额度用完]"""
    app.send_message("测试额度")
    app.wait_hist("额度用完", timeout=20)
    assert mock_count("err429_quota") == 1, "额度错误不应重试, 实际请求 %d 次" % mock_count("err429_quota")
    assert "[http] 瞬时错误重试" not in app.log_text()
    assert "额度用完" in app.text(ID_HISTORY)


@case("err402")
def T010_欠费402_零重试(app):
    app.send_message("测试欠费")
    app.wait_hist("额度用完", timeout=20)
    assert mock_count("err402") == 1


@case("err401")
def T011_鉴权失败401(app):
    app.send_message("测试鉴权")
    app.wait_hist("鉴权失败", timeout=20)
    assert mock_count("err401") == 1
    assert "API Key" in app.text(ID_HISTORY)


@case("trunc")
def T012_流截断触发重试(app):
    """200 但无 [DONE] -> 判定流中断 -> 重试拿全量"""
    app.send_message("截断我")
    app.wait_hist("重试后拿到完整回复", timeout=40)
    assert "响应流中断" in app.text(ID_HISTORY)
    assert mock_count("trunc") == 2


@case("chat")
def T013_新建会话按钮(app):
    """对话后点[新建会话] -> 历史清空 + 旧会话保留提示"""
    app.send_message("你好")
    app.wait_hist("mock 模型", timeout=20)
    app.click(ID_NEW_BTN)
    app.wait_hist("新会话已创建", timeout=10)
    assert "你好，我是 mock 模型" not in app.text(ID_HISTORY), "旧对话未被清空"


# ---------------- 边界补全 (输入 / TCP / 参数截断 / 回删 / 窗口) ----------------

@case("chat")
def T014_空输入回车不发包(app):
    """空输入按回车 -> start_task 直接 return: 无请求、无状态变化"""
    app.click(ID_INPUT)
    app.press_enter()
    time.sleep(1.5)
    assert mock_count("chat") == 0, "空输入不应发出请求, 实际 %d 次" % mock_count("chat")
    assert app.text(ID_SEND) == "发送", "空输入不应进入运行状态"
    assert app.text(ID_INPUT) == "", "空输入场景输入框应保持为空"


@case("chat")
def T015_超大输入完整送达(app):
    """~12KB 中文输入 (首尾标记) -> 完整进请求体 -> 正常回复"""
    text = "头标记HEAD-7a3c" + "字" * 12000 + "尾标记TAIL-9f8e7d"
    app.click_input_and_type("")
    app.type_text_fast(text)
    # PostMessage 队列抽干有延迟 (实测数秒), 轮询等输入框收满, 不能读一次就断言
    app.wait(lambda: len(app.text(ID_INPUT)) >= len(text), 20,
             "输入框未容纳全部大文本 (期望 %d)" % len(text))
    app.press_enter()
    app.wait_hist("mock 模型", timeout=30)
    body = mock_last_body("chat")
    all_content = json.dumps(body, ensure_ascii=False)
    assert "头标记HEAD-7a3c" in all_content and "尾标记TAIL-9f8e7d" in all_content, \
        "大文本被截断 (尾标记丢失)"
    app.screenshot("T015_biginput.bmp")


@case("drip")
def T016_连按回车_不重复发送(app):
    """快速两次回车: 第一次启动会话, 第二次命中 g_running 走取消路径;
    关键断言: 绝不产生第二次请求 (防双发)"""
    app.click_input_and_type("连按回车")
    app.press_enter()
    app.press_enter()
    app.wait_hist("已取消", timeout=30)
    app.wait(lambda: app.text(ID_SEND) == "发送", 10, "取消后按钮未复位")
    assert mock_count("drip") == 1, "连按回车导致重复发送, 请求 %d 次" % mock_count("drip")


@case("bigargs")
def T017_大参数工具_不截断(app):
    """write_file 参数 ~30KB (含尾部标记): 回归 b0e0f41 参数截断修复;
    断言文件完整落盘 + 工具结果回传"""
    app.send_message("写大文件")
    app.wait_hist("大文件写入完成", timeout=40)
    path = os.path.join(app.dir, "ws", "big.txt")
    with open(path, "r", encoding="utf-8") as f:
        content = f.read()
    assert len(content) >= 30000, "文件被掐断: %d bytes" % len(content)
    assert "TAIL-MARKER-9f8e7d" in content, "文件尾部标记丢失 (写入被截断)"
    assert mock_count("bigargs") == 2
    body = mock_last_body("bigargs")
    toolmsg = [m for m in body["messages"] if m.get("role") == "tool"]
    assert toolmsg, "工具结果未回传给模型"


@case("resetconn")
def T018_服务端断开连接_网络类重试(app):
    """请求发出后服务端一字节不回直接断连 -> 网络类错误 -> 退避重试成功"""
    t0 = time.time()
    app.send_message("断我")
    app.wait_hist("断线重连后的回复", timeout=45)
    log = app.log_text()
    assert "[http] 瞬时错误重试" in log, "断连未被当作可重试错误: %s" % log[-600:]
    assert "网络瞬时错误" in app.text(ID_HISTORY), "界面应提示[网络瞬时错误]"
    assert mock_count("resetconn") == 2
    assert time.time() - t0 > 1.5, "应有退避等待 (基数 2s)"


@case("trunc")
def T019_半截回复_重试前回删(app):
    """流式上屏的半截内容必须在重试前被回删: 最终历史无残留、无重复段"""
    app.send_message("截断我")
    app.wait_hist("重试后拿到完整回复", timeout=40)
    hist = app.text(ID_HISTORY)
    assert "半截回复" not in hist, "半截内容未被回删, 仍残留在历史区"
    assert hist.count("重试后拿到完整回复") == 1, "完整回复出现 %d 次 (重试拼接重复)" % \
        hist.count("重试后拿到完整回复")
    assert "响应流中断" in hist
    assert mock_count("trunc") == 2


@case("chat2")
def T020_窗口拉伸后布局与功能(app):
    """缩到极小 -> 被 WM_GETMINMAXINFO 钳制不破版; 拉大 -> 控件随 layout 重排;
    两个尺寸下键入回车都正常"""
    app.resize(400, 320)   # 低于最小跟踪尺寸, 应被钳制而非溢出
    wr = app.window_rect()
    w_min, h_min = wr.right - wr.left, wr.bottom - wr.top
    assert w_min >= 560 and h_min >= 400, "极小尺寸未被钳制: %dx%d" % (w_min, h_min)
    for cid in (ID_INPUT, ID_SEND, ID_HISTORY):
        h, r = app.ctrls[cid]
        assert r.right > r.left and r.bottom > r.top, "控件 %d 退化为零面积" % cid
        assert r.left >= wr.left and r.right <= wr.right and \
               r.top >= wr.top and r.bottom <= wr.bottom, "控件 %d 越界" % cid
    app.send_message("拉伸小")
    app.wait_hist("第二次对话回复 OK", timeout=20)
    app.resize(1600, 1000)
    wr = app.window_rect()
    for cid in (ID_INPUT, ID_SEND, ID_HISTORY):
        h, r = app.ctrls[cid]
        assert r.left >= wr.left and r.right <= wr.right and \
               r.top >= wr.top and r.bottom <= wr.bottom, "拉大后控件 %d 越界" % cid
    app.send_message("拉伸大")
    app.wait_hist("第二次对话回复 OK", timeout=20)
    assert mock_count("chat2") == 2
    app.screenshot("T020_resized.bmp")


@case("chat3", env={"CAGENT_SIMULATE_DPI": "96"})
def T021_DPICHANGED_四步链路(app):
    """CAGENT_SIMULATE_DPI=96: cagent 自发送 WM_DPICHANGED (与真实跨屏拖动同一条
    处理链路: 重算外框 -> layout -> 字体重建 -> parafmt)。断言窗口按新 DPI 缩放、
    控件全部重排且在窗口内、字体重建后键入回车仍正常。"""
    # 系统当前 240 DPI (250%); 切到 96 后客户区宽应 ≈ 720*(96/96) + 外框
    r1 = app.window_rect()
    w = r1.right - r1.left
    expected = 720 * 96 // 96 + 40      # dp(720)@96 + 外框余量
    assert abs(w - expected) < 80, "DPI 切到 96 后窗口宽 %d, 期望 ≈%d" % (w, expected)
    assert "[ui] DPI 切换" in app.log_text(), "日志无 DPI 切换记录"
    assert ID_INPUT in app.ctrls and ID_SEND in app.ctrls, "DPI 切换后控件丢失"
    wr = app.window_rect()
    for cid in (ID_INPUT, ID_SEND, ID_HISTORY):
        h, cr = app.ctrls[cid]
        assert cr.left >= wr.left and cr.right <= wr.right and \
               cr.top >= wr.top and cr.bottom <= wr.bottom, "DPI 切换后控件 %d 越界" % cid
    app.send_message("DPI后发送")
    app.wait_hist("DPI 切换后的回复", timeout=20)
    assert mock_count("chat3") == 1
    app.screenshot("T021_dpi96.bmp")


# ================================================================ 运行器
def start_mock():
    proc = subprocess.Popen([sys.executable, os.path.join(REPO, "test", "mock_server.py"),
                             str(PORT)], cwd=REPO)
    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            urllib.request.urlopen(MOCK + "/ping", timeout=2).read()
            return proc
        except Exception:
            time.sleep(0.2)
    raise RuntimeError("mock 服务未启动")


def main():
    flt = sys.argv[1] if len(sys.argv) > 1 else ""
    os.makedirs(ART, exist_ok=True)
    mock = start_mock()
    passed, failed = [], []
    try:
        for name, scenario, fn, env in CASES:
            if flt and flt not in name:
                continue
            print("== %s (scenario=%s) ==" % (name, scenario), flush=True)
            app = None
            try:
                app = App(scenario, name, env_extra=env)
                fn(app)
                passed.append(name)
                print("   PASS", flush=True)
            except Exception as e:
                failed.append((name, str(e)))
                print("   FAIL: %s" % e, flush=True)
            finally:
                if app:
                    app.close()
    finally:
        mock.terminate()
    print("\n===== %d 通过, %d 失败 =====" % (len(passed), len(failed)))
    for n, e in failed:
        print("FAIL %s: %s" % (n, e))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
