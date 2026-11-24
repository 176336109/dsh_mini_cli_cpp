"""Linux end-to-end checks for filesystem isolation, processes, MCP and quotas."""

import json
import os
import pathlib
import shlex
import signal
import subprocess
import sys
import tempfile
import time


MOCK_MCP = """
import json
import os
import pathlib
import sys
import time

pathlib.Path("mcp.pid").write_text(str(os.getpid()))
for line in sys.stdin:
    msg = json.loads(line)
    method = msg["method"]
    if method == "initialize":
        result = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}},
                  "serverInfo": {"name": "fixture", "version": "1"}}
    elif method == "notifications/initialized":
        continue
    elif method == "tools/list":
        result = {"tools": [{"name": "echo", "description": "test",
                            "inputSchema": {"type": "object"}}]}
    elif method == "tools/call":
        text = msg["params"]["arguments"]["text"]
        if text == "wait":
            time.sleep(60)
        result = {"content": [{"type": "text", "text": text}]}
    else:
        raise ValueError(method)
    print(json.dumps({"jsonrpc": "2.0", "id": msg["id"], "result": result}), flush=True)
"""


def assert_stopped(pid):
    state = pathlib.Path(f"/proc/{pid}/stat")
    for _ in range(60):
        try:
            content = state.read_text()
        except FileNotFoundError:
            return
        if content.rsplit(")", 1)[-1].lstrip().startswith("Z"):
            return
        time.sleep(0.05)
    raise AssertionError(f"process still running: {pid}")


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="dsh-linux-") as name:
        root = pathlib.Path(name)
        workspace = root / "work"
        workspace.mkdir()
        outside = root / "outside.txt"
        outside.write_text("protected")
        (workspace / "external-link").symlink_to(outside)
        home = root / "home"
        command = [str(binary), "--cwd", str(workspace), "--home", str(home), "--sandbox", "landlock"]
        known = set()

        def invoke(tool, arguments, *options, returncode=0):
            job = [*command, *options, f"!{tool} {json.dumps(arguments)}"]
            proc = subprocess.run(job, capture_output=True, text=True, encoding="utf-8", timeout=15)
            assert proc.returncode == returncode, proc.stderr
            if returncode:
                return proc
            return result()

        def result():
            logs = set((home / "sessions").glob("*.jsonl"))
            path, = logs - known
            known.add(path)
            events = [json.loads(line) for line in path.read_text().splitlines()]
            return next(event["data"] for event in events if event["type"] == "tool/result")

        out = invoke("bash", {"command": "printf ok > inside.txt; cat inside.txt"})
        assert "exit_code: 0" in out["content"] and "ok" in out["content"]
        assert (workspace / "inside.txt").read_text() == "ok"
        out = invoke("bash", {"command": f"printf changed > {shlex.quote(str(outside))}"})
        assert "exit_code: 0" not in out["content"] and outside.read_text() == "protected"
        assert invoke("read_file", {"path": "external-link"})["isError"]
        assert invoke("write_file", {"path": "external-link", "content": "changed"})["isError"]
        assert outside.read_text() == "protected"

        out = invoke("bash", {"command": "sleep 60 & echo $! > child.pid; wait"}, "--tool-timeout", "1")
        assert out["isError"] and "超时" in out["content"]
        assert_stopped(int((workspace / "child.pid").read_text()))

        job = [*command, '!bash {"command":"sleep 60 & echo $! > cancel.pid; wait"}']
        with subprocess.Popen(job, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE) as proc:
            pid_file = workspace / "cancel.pid"
            for _ in range(100):
                if pid_file.exists() and pid_file.stat().st_size:
                    break
                time.sleep(0.05)
            proc.send_signal(signal.SIGINT)
            assert proc.wait(timeout=5) == 130, proc.stderr.read().decode()
            assert_stopped(int(pid_file.read_text()))
        assert result()["isError"]

        out = invoke("bash", {"command": "yes 'bounded-output'"})
        assert "output_limit" in out["content"] and len(out["content"].encode()) < 30_100

        script = workspace / "mock_mcp.py"
        script.write_text(MOCK_MCP)
        config = root / "mcp.json"
        config.write_text(json.dumps({"servers": [{"name": "demo", "command": sys.executable,
                                                   "args": ["-u", str(script)]}]}))
        out = invoke("mcp__demo__echo", {"text": "hello mcp"}, "--mcp", str(config))
        assert out["content"] == "hello mcp" and not out["isError"]
        assert_stopped(int((workspace / "mcp.pid").read_text()))
        out = invoke("mcp__demo__echo", {"text": "wait"}, "--mcp", str(config), "--tool-timeout", "2")
        assert out["isError"] and "超时" in out["content"]
        assert_stopped(int((workspace / "mcp.pid").read_text()))

        quoted_python = shlex.quote(sys.executable)
        out = invoke("bash", {"command": quoted_python + ' -c "import os,resource; print(len(os.sched_getaffinity(0)), resource.getrlimit(resource.RLIMIT_AS))"'},
                     "--resource-mode", "fixed", "--cpu-quota", "1", "--mem-quota", "128")
        assert f"1 ({128 * 1024 * 1024}, {128 * 1024 * 1024})" in out["content"], out
        failed = invoke("bash", {"command": "true"}, "--resource-mode", "free",
                        "--cpu-quota", "1", returncode=1)
        assert "冲突" in failed.stderr


if __name__ == "__main__":
    main()
