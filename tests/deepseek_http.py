"""Send actual HTTP/SSE traffic to the dsh-mini model adapter over loopback."""

import http.server
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import threading


def frame(delta, finished=None):
    return {"choices": [{"delta": delta, "finish_reason": finished}]}


class MockGateway(http.server.BaseHTTPRequestHandler):
    requests = []
    authorization = []

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.requests.append((self.path, request))
        self.authorization.append(self.headers["Authorization"])
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        messages = request["messages"]
        latest = messages[-1]
        done = latest.get("content") != "disconnect"
        if not done:
            frames = [frame({"content": "partial"})]
        elif latest["role"] == "tool":
            frames = [frame({"content": "已完成"}, "stop"), {"choices": [], "usage": {
                "prompt_tokens": 13, "completion_tokens": 5}}]
        else:
            frames = [frame({"reasoning_content": "先读文件", "tool_calls": [
                {"index": 0, "id": "test_call", "function": {
                    "name": "read_file", "arguments": '{"pa'}}]}),
                frame({"tool_calls": [
                    {"index": 0, "function": {"arguments": 'th":"a.txt"}'}}]}, "tool_calls")]
        output = "".join("data: " + json.dumps(block, ensure_ascii=False) + "\r\n\r\n"
                         for block in frames)
        if done:
            output += "data: [DONE]\r\n\r\n"
        data = output.encode("utf-8")
        for offset in range(0, len(data), 3):
            self.wfile.write(data[offset:offset + 3])
            self.wfile.flush()

    def log_message(self, *_):
        pass


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="dsh-http-") as directory:
        root = pathlib.Path(directory)
        (root / "a.txt").write_text("contents\n")
        data = root / "data"
        key = root / "key"
        key.write_text("\nfixture-key\n")
        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), MockGateway) as server:
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            address = f"http://127.0.0.1:{server.server_port}"
            options = [str(binary), "--cwd", str(root), "--home", str(data), "--model", "test-model",
                       "--sandbox", "none", "--thinking", "--key-file", str(key), "--base-url", address]
            env = dict(os.environ)
            for key_name in ("DEEPSEEK_API_KEY", "DEEPSEEK_BASE_URL", "DSH_PROXY"):
                env.pop(key_name, None)
            env["DEEPSEEK_BASE_URL"] = address
            env["NO_PROXY"] = "127.0.0.1"
            try:
                proc = subprocess.run([*options, "read"], capture_output=True, text=True,
                                      encoding="utf-8", env=env, timeout=15)
                assert proc.returncode == 0 and "已完成" in proc.stdout, proc.stderr
                assert MockGateway.authorization == ["Bearer fixture-key", "Bearer fixture-key"]
                requests = MockGateway.requests
                assert len(requests) == 2 and requests[0][0] == "/chat/completions"
                replay = requests[1][1]
                assert replay["thinking"]["type"] == "enabled"
                assistant, tool = replay["messages"][-2:]
                assert assistant["reasoning_content"] == "先读文件"
                assert assistant["tool_calls"][0]["id"] == tool["tool_call_id"] == "test_call"
                assert "contents" in tool["content"]
                log = next((data / "sessions").glob("*.jsonl"))
                source = log.read_text(encoding="utf-8")
                assert "fixture-key" not in source and '"inputTokens":13' in source

                broken = subprocess.run([*options, "disconnect"], capture_output=True, text=True,
                                        encoding="utf-8", env=env, timeout=15)
                assert broken.returncode == 1 and "终止事件" in broken.stderr, broken.stderr

                (data / ".env").write_text(
                    "export DEEPSEEK_API_KEY='dotenv-fixture-key'\n"
                    "DEEPSEEK_BASE_URL=http://fixture.invalid\n", encoding="utf-8")
                proxy_env = dict(env)
                proxy_env.pop("DEEPSEEK_BASE_URL")
                proxy_options = [str(binary), "--cwd", str(root), "--home", str(data),
                                 "--model", "test-model", "--sandbox", "none",
                                 "--base-url", address, "--proxy", address, "read"]
                proxied = subprocess.run(proxy_options, capture_output=True, text=True,
                                         encoding="utf-8", env=proxy_env, timeout=15)
                assert proxied.returncode == 0, proxied.stderr
                assert requests[-1][0] == "http://fixture.invalid/chat/completions"
                assert MockGateway.authorization[-2:] == ["Bearer dotenv-fixture-key"] * 2
                assert requests[-1][1]["thinking"]["type"] == "disabled"
            finally:
                server.shutdown()
                worker.join()


if __name__ == "__main__":
    main()
