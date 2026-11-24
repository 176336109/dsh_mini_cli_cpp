"""Compare public Linux behavior using the same Mock input on both implementations."""

import argparse
import json
import pathlib
import shlex
import subprocess
import tempfile
from dataclasses import dataclass, field


@dataclass(frozen=True)
class Case:
    name: str
    tool: str | None
    args: dict = field(default_factory=dict)
    fragments: tuple[str, ...] = ()
    changes: dict[str, bytes] = field(default_factory=dict)
    options: tuple[str, ...] = ()
    error: bool = False
    steps: int = 2
    reason: str = "completed"


FIXTURE = {
    "note.txt": "First\r\n第二行".encode(),
    "src/demo.txt": b"alpha\nBETA\n",
    "src/deep/extra.txt": b"more\n",
    "repeated.txt": b"same same\n",
    "target/cache.txt": b"BETA\n",
}


def cases():
    return (
        Case("read_crlf", "read_file", {"path": "note.txt", "offset": 2, "limit": 1},
             ("2| 第二行", "末尾没有换行符")),
        Case("read_missing", "read_file", {"path": "missing.txt"}, error=True),
        Case("read_bad_args", "read_file", {}, error=True),
        Case("write_nested", "write_file", {"path": "new/out.txt", "content": "新的结果\n"},
             changes={"new/out.txt": "新的结果\n".encode()}),
        Case("write_parent_denied", "write_file", {"path": "../outside.txt", "content": "changed"},
             error=True),
        Case("replace_preserves_crlf", "str_replace",
             {"path": "note.txt", "old_string": "First\n第二行", "new_string": "One\nTwo"},
             changes={"note.txt": b"One\r\nTwo"}),
        Case("replace_duplicate_denied", "str_replace",
             {"path": "repeated.txt", "old_string": "same", "new_string": "change"}, error=True),
        Case("list", "list_dir", {"path": "src"}, ("deep/", "demo.txt")),
        Case("grep", "grep", {"path": ".", "pattern": "beta", "include": "src/**/*.txt",
                             "ignore_case": True}, ("src/demo.txt:2: BETA",)),
        Case("glob", "glob", {"path": ".", "pattern": "src/**/*.txt"},
             ("src/demo.txt", "src/deep/extra.txt")),
        Case("bash", "bash", {"command": "printf ok > shell.txt; cat shell.txt; printf diagnostic >&2"},
             ("exit_code: 0", "ok", "diagnostic"), changes={"shell.txt": b"ok"}),
        Case("bash_exit", "bash", {"command": "printf failure >&2; exit 7"},
             ("exit_code: 7", "failure")),
        Case("bash_timeout", "bash", {"command": "sleep 10"}, ("超时",),
             options=("--tool-timeout", "1"), error=True),
        Case("bash_workspace_boundary", "bash",
             {"command": "printf blocked > " + shlex.quote("../outside.txt")}),
        Case("mcp_echo", "mcp__demo__echo", {"text": "parity echo"}, ("parity echo",)),
        Case("unknown_tool", "not_registered", {}, error=True),
        Case("step_limit", "read_file", {"path": "note.txt"}, ("First",),
             options=("--max-steps", "1"), steps=1, reason="max_steps"),
        Case("empty_turn", None, steps=0, reason="empty"),
    )


def inspect(binary, mcp_binary, case, root):
    work, home = root / "work", root / "session"
    work.mkdir(parents=True)
    protected = root / "outside.txt"
    protected.write_bytes(b"untouched\n")
    for relative, content in FIXTURE.items():
        path = work / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    options = list(case.options)
    if case.tool and case.tool.startswith("mcp__"):
        config = root / "mcp.json"
        config.write_text(json.dumps({"servers": [{"name": "demo", "command": str(mcp_binary)}]}))
        options.extend(["--mcp", str(config)])
    prompt = f"!{case.tool} {json.dumps(case.args, ensure_ascii=False)}" if case.tool else ""
    result = subprocess.run(
        [str(binary), "--cwd", str(work), "--home", str(home), "--model", "mock",
         "--sandbox", "landlock", *options, "--", prompt],
        capture_output=True, text=True, encoding="utf-8", timeout=20,
    )
    if result.returncode:
        raise AssertionError(f"CLI returned {result.returncode}: {result.stderr.strip()}")
    log, = (home / "sessions").glob("*.jsonl")
    events = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
    steps = sum(item["type"] == "step/start" for item in events)
    turns = [item["data"] for item in events if item["type"] == "turn/end"]
    results = [item["data"] for item in events if item["type"] == "tool/result"]
    assert steps == case.steps, (steps, case.steps)
    assert turns == [{"turn": 1, "reason": case.reason}], turns
    assert len(results) == int(case.tool is not None), results
    observed = {"returncode": result.returncode, "steps": steps, "reason": turns[0]["reason"]}
    if results:
        value, = results
        calls = [item["data"] for item in events if item["type"] == "tool/call"]
        assert len(calls) == 1, calls
        assert calls[0]["callId"] == value["callId"], (calls, results)
        assert value["name"] == case.tool, value
        assert value["isError"] == case.error, value
        for fragment in case.fragments:
            assert fragment in value["content"], (fragment, value["content"])
        if case.name == "bash_workspace_boundary":
            assert "exit_code: 0" not in value["content"], value
        if case.name == "grep":
            assert "target/cache.txt" not in value["content"], value
        observed["result_error"] = value["isError"]
    assert protected.read_bytes() == b"untouched\n", "outside.txt changed"
    actual_files = {str(path.relative_to(work)): path.read_bytes()
                    for path in work.rglob("*") if path.is_file()}
    assert actual_files == (FIXTURE | case.changes), sorted(actual_files)
    return observed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("rust", "cpp", "mcp-server", "report"):
        parser.add_argument("--" + name, type=pathlib.Path, required=True)
    args = parser.parse_args()
    binaries = {"rust": args.rust.resolve(strict=True), "cpp": args.cpp.resolve(strict=True)}
    server = args.mcp_server.resolve(strict=True)
    results = []
    with tempfile.TemporaryDirectory(prefix="dsh-parity-") as directory:
        root = pathlib.Path(directory)
        for case in cases():
            row = {"case": case.name}
            for name, binary in binaries.items():
                try:
                    data = inspect(binary, server, case, root / case.name / name)
                    row[name] = {"pass": True, **data}
                except (AssertionError, OSError, ValueError, subprocess.TimeoutExpired) as error:
                    row[name] = {"pass": False, "error": str(error)}
            row["parity_pass"] = row["rust"]["pass"] and row["rust"] == row["cpp"]
            results.append(row)
            print(f"{case.name:28} Rust {'PASS' if row['rust']['pass'] else 'FAIL'}"
                  f"  C++ {'PASS' if row['cpp']['pass'] else 'FAIL'}")
            for name in binaries:
                if not row[name]["pass"]:
                    print(f"  {name}: {row[name]['error']}")
    report = {
        "mode": "linux_mock_common_contract",
        "binaries": {name: str(path) for name, path in binaries.items()},
        "same_pass": sum(row["parity_pass"] for row in results),
        "total": len(results),
        "cases": results,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{report['same_pass']}/{report['total']} common contract cases passed: {args.report}")
    return int(report["same_pass"] != report["total"])


if __name__ == "__main__":
    raise SystemExit(main())
