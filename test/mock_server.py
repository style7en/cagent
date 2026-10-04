# -*- coding: utf-8 -*-
"""test/mock_server.py - 场景化 OpenAI 兼容 mock LLM 服务 (仅标准库)

按请求的 Bearer token 路由到预编排的场景 (SCENARIOS)。UI E2E 驱动器
(e2e_ui.py) 启动 cagent.exe 时把 api_key 写成场景名, 于是每个测试用例
各自路由到自己的响应脚本 —— 失败注入 (429/401/402/5xx/截断/慢流) 全部
可精确复现, 不烧真实额度。

辅助接口 (供驱动器断言):
  GET  /reset?token=X      清空该场景的请求计数与抓到的请求体
  GET  /last_body?token=X  返回该场景最后一次 /chat/completions 请求体
  GET  /count?token=X      返回该场景已被请求的次数
  GET  /ping               存活探测

用法: python mock_server.py <port>
"""
import json
import time
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ---------------------------------------------------------------- 场景编排
# 每个 step 是一次 HTTP 响应:
#   {"sse": [delta_dict...], "finish": "stop", "delay": 0.05, "usage": {...}}
#       -> 200 SSE 流 (delta 逐个下发, 结尾补 finish_reason + [DONE])
#   {"status": 429, "body": "...", "headers": {"Retry-After": "8"}}
#       -> 非 200 错误响应
#   {"sse": [...], "truncate": True}
#       -> 200 但不发 [DONE] 直接断流 (流被截断)
#   {"reset": True}
#       -> 收到请求后一字节不回直接关连接 (服务端断开, TCP 层异常)
#   step 为 "hang" 时挂住连接 (慢流用 sse + 大 delay 实现即可, 不再单独做)


def sse_step(content=None, tool_calls=None, finish="stop", delay=0.03,
             usage=None, truncate=False):
    """构造一个 200 SSE 响应 step。content 为整段回复文本 (自动按 ~12 字符切块)"""
    deltas = []
    if content is not None:
        for i in range(0, len(content), 12):
            deltas.append({"content": content[i:i + 12]})
    if tool_calls:
        # tool_calls: [(name, args_json_str), ...] 并行调用
        for idx, (name, args) in enumerate(tool_calls):
            cid = "call_%d" % (idx + 1)
            deltas.append({"role": "assistant", "tool_calls": [
                {"index": idx, "id": cid, "type": "function",
                 "function": {"name": name, "arguments": ""}}]})
            deltas.append({"tool_calls": [
                {"index": idx, "function": {"arguments": args}}]})
    step = {"deltas": deltas, "finish": finish, "delay": delay, "truncate": truncate}
    if usage:
        step["usage"] = usage
    return step


def err_step(status, body, headers=None):
    return {"status": status, "body": body, "headers": headers or {}}


SCENARIOS = {
    # 普通对话: 一段中文回复
    "chat": [sse_step("你好，我是 mock 模型，这条回复用于端到端测试。")],
    "chat2": [sse_step("第二次对话回复 OK。")],
    # 工具往返: 先发 read_file 调用, 第二次请求 (带工具结果) 回最终答案
    "tool": [sse_step(tool_calls=[("read_file", '{"path":"note.txt"}')]),
             sse_step("文件内容是 HELLO-FROM-MOCK，读取成功。")],
    # 并行工具: 两个 read_file
    "tool2": [sse_step(tool_calls=[("read_file", '{"path":"a.txt"}'),
                                   ("read_file", '{"path":"b.txt"}')]),
              sse_step("两个文件都读完了。")],
    # 慢速滴流 (停止按钮测试): 40 块 × 0.3s = 12s 流
    "drip": [sse_step("慢慢输出" * 30, delay=0.3)],
    # 重试: 500 ×2 后成功
    "err500x2": [err_step(500, "internal server error"),
                 err_step(500, "internal server error"),
                 sse_step("第三次尝试成功，重试链路 OK。")],
    # 限流 + Retry-After: 服务端要求等 8 秒
    "err429_ra": [err_step(429, '{"error":{"message":"rate limit exceeded"}}',
                           {"Retry-After": "8"}),
                  sse_step("限流恢复后的回复。")],
    # 额度用完: 429 + insufficient_quota 报文 -> 不应重试
    "err429_quota": [err_step(429, '{"error":{"code":"insufficient_quota",'
                                   '"message":"You exceeded your current quota"}}')],
    # 欠费 402 -> 不应重试
    "err402": [err_step(402, '{"error":{"message":"Insufficient Balance"}}')],
    # 鉴权失败 401 -> 不应重试
    "err401": [err_step(401, '{"error":{"message":"Invalid API key"}}')],
    # 200 但流被截断 (无 [DONE]) -> 应重试; 半截内容先上屏, 用于验证重试前回删
    "trunc": [sse_step(content="半截回复", truncate=True),
              sse_step("重试后拿到完整回复。")],
    # 请求发出后服务端直接断开 (无任何响应字节) -> 网络类错误, 应重试
    "resetconn": [{"reset": True},
                  sse_step("断线重连后的回复。")],
    # 大参数工具: write_file 参数 ~30KB (含尾部标记), 回归 b0e0f41 参数截断修复
    "bigargs": [sse_step(tool_calls=[("write_file", json.dumps(
        {"path": "big.txt", "content": "A" * 30000 + "\nTAIL-MARKER-9f8e7d"},
        ensure_ascii=False))]),
        sse_step("大文件写入完成。")],
    # DPI 注入后的功能验证回复
    "chat3": [sse_step("DPI 切换后的回复。")],
}

# ---------------------------------------------------------------- 运行状态
LOCK = __import__("threading").Lock()
STATE = {}   # token -> {"n": int, "bodies": [bytes...]}


def state_for(token):
    with LOCK:
        return STATE.setdefault(token, {"n": 0, "bodies": []})


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):   # 静默: 测试输出要干净
        pass

    def _token(self):
        auth = self.headers.get("Authorization", "")
        return auth[7:] if auth.startswith("Bearer ") else auth

    def _json(self, code, obj):
        b = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        from urllib.parse import urlparse, parse_qs
        u = urlparse(self.path)
        q = parse_qs(u.query)
        token = q.get("token", [""])[0]
        if u.path == "/ping":
            return self._json(200, {"ok": True})
        st = state_for(token)
        if u.path == "/reset":
            st["n"] = 0
            st["bodies"] = []
            return self._json(200, {"ok": True})
        if u.path == "/count":
            return self._json(200, {"count": st["n"]})
        if u.path == "/last_body":
            body = st["bodies"][-1] if st["bodies"] else None
            if body is None:
                return self._json(200, {"body": None})
            return self._json(200, {"body": json.loads(body.decode("utf-8"))})
        self._json(404, {"error": "not found"})

    def do_POST(self):
        from urllib.parse import urlparse, parse_qs
        u = urlparse(self.path)
        token = self._token()
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length) if length else b""

        if u.path == "/reset":
            st = state_for(token)
            st["n"] = 0
            st["bodies"] = []
            return self._json(200, {"ok": True})

        if u.path != "/chat/completions" and not u.path.endswith("/chat/completions"):
            return self._json(404, {"error": "not found"})

        st = state_for(token)
        with LOCK:
            st["n"] += 1
            st["bodies"].append(body)
        steps = SCENARIOS.get(token)
        if not steps:
            return self._json(404, {"error": "unknown scenario"})
        step = steps[min(st["n"] - 1, len(steps) - 1)]

        if step.get("reset"):
            # 一字节不回直接关连接: 客户端看到的是连接在响应前被服务端关闭
            self.close_connection = True
            return

        if "status" in step:
            b = step["body"].encode("utf-8")
            self.send_response(step["status"])
            for k, v in step.get("headers", {}).items():
                self.send_header(k, v)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
            return

        # 200 SSE 流
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        if step.get("truncate"):
            self.send_header("Connection", "close")   # 无 Content-Length: 读完即断
            self.end_headers()
            self._write_sse(step, done=False)
            self.close_connection = True
            return
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self._write_sse(step, done=True)
        except (BrokenPipeError, ConnectionResetError):
            pass   # 客户端 (停止) 提前断开: 正常
        self.close_connection = True

    def _write_sse(self, step, done):
        delay = step.get("delay", 0.03)
        for d in step["deltas"]:
            self._data({"choices": [{"index": 0, "delta": d, "finish_reason": None}]})
            if delay:
                time.sleep(delay)
        if done:
            fin = {"choices": [{"index": 0, "delta": {},
                                "finish_reason": step.get("finish", "stop")}]}
            if step.get("usage"):
                fin["usage"] = step["usage"]
            self._data(fin)
            self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()

    def _data(self, obj):
        self.wfile.write(b"data: " + json.dumps(obj, ensure_ascii=False).encode("utf-8") + b"\n\n")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print("mock server on %d" % port, flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
