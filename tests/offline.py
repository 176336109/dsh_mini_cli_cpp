"""Exercise the real CLI and the JSONL replay boundary without credentials."""

import json
import pathlib
import subprocess
import sys
import tempfile


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="dsh-cpp-") as directory:
        root = pathlib.Path(directory)
        fixture = root / "workspace"
        fixture.mkdir()
        (fixture / "input.txt").write_bytes("first\r\n第二行".encode("utf-8"))

        def invoke(home, *args, expected=0, stdin=None):
            result = subprocess.run(
                [str(binary), "--cwd", str(fixture), "--home", str(home), "--sandbox", "none", *args],
                input=stdin, capture_output=True, text=True, encoding="utf-8", timeout=10,
            )
            assert result.returncode == expected, (result.returncode, result.stderr)
            return result

        def load(path):
            return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]

        def saved_log(home):
            return next((home / "sessions").glob("*.jsonl"))

        interactive = root / "interactive"
        invoke(interactive, stdin='!read_file {"path":"input.txt"}\n\n/quit\n')
        turns = [item["data"] for item in load(saved_log(interactive)) if item["type"] == "turn/end"]
        assert turns == [{"turn": 1, "reason": "completed"}, {"turn": 2, "reason": "empty"}]

        home = root / "mock"
        invocation = '!read_file {"path":"input.txt","offset":2,"limit":1}'
        result = invoke(home, "--model", "mock", invocation)
        log = saved_log(home)
        events = load(log)
        assert [item["type"] for item in events] == [
            "session/start", "system/message", "turn/start", "user/message", "step/start",
            "assistant/message", "tool/call", "tool/result", "step/end", "step/start",
            "assistant/message", "step/end", "turn/end",
        ]
        call = events[5]["data"]["toolCalls"][0]
        outcome = events[7]["data"]
        assert call["id"] == outcome["callId"]
        assert outcome["isError"] is False and "2| 第二行\n" in outcome["content"]
        assert "末尾没有换行符" in outcome["content"]
        assert call["id"] in result.stdout
        assert events[-1]["data"] == {"turn": 1, "reason": "completed"}

        invoke(home, "--resume", log.stem, "")
        resumed = load(log)
        assert len(resumed) == len(events) + 2
        assert resumed[-1]["data"] == {"turn": 2, "reason": "empty"}
        assert [event["seq"] for event in resumed] == list(range(len(resumed)))

        invoke(home, "--resume", "--", "!no_such_tool {}")
        bad_tool = load(log)[-6]["data"]
        assert bad_tool["isError"] and "未知工具" in bad_tool["content"]

        invoke(home, "--resume", "--max-steps", "1", invocation)
        assert load(log)[-1]["data"]["reason"] == "max_steps"

        invoke(home, "--resume", "--", '!read_file {"path":"../outside.txt"}')
        assert load(log)[-6]["data"]["isError"]

        interrupted_home = root / "interrupted"
        sessions = interrupted_home / "sessions"
        sessions.mkdir(parents=True)
        crashed = sessions / "crashed.jsonl"
        with crashed.open("wb") as file:
            for item in events[:7]:
                file.write((json.dumps(item, ensure_ascii=False) + "\n").encode("utf-8"))
            file.write(b'{"type":')
        invoke(interrupted_home, "--resume", "crashed", "follow up")
        recovered = load(crashed)
        assert recovered[7]["type"] == "tool/result"
        assert recovered[7]["data"]["callId"] == call["id"]
        assert recovered[7]["data"]["isError"] is True
        assert recovered[8]["data"]["reason"] == "cancelled"
        assert recovered[-1]["data"]["turn"] == 2

        with crashed.open("a", encoding="utf-8") as file:
            seq = recovered[-1]["seq"] + 1
            file.write(json.dumps({"type": "future/event", "seq": seq, "time": 0,
                                   "data": {}, "ignorable": True}) + "\n")
        invoke(interrupted_home, "--resume", "crashed", "")
        assert load(crashed)[-2]["seq"] == seq + 1
        with crashed.open("a", encoding="utf-8") as file:
            seq = load(crashed)[-1]["seq"] + 1
            file.write(json.dumps({"type": "future/event", "seq": seq, "time": 0, "data": {}}) + "\n")
        failed = invoke(interrupted_home, "--resume", "crashed", "", expected=1)
        assert "未知且不可忽略" in failed.stderr


if __name__ == "__main__":
    main()
