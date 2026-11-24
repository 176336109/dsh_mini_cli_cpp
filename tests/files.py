import json
import pathlib
import subprocess
import sys
import tempfile


def main():
    binary = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="dsh-tools-") as directory:
        root = pathlib.Path(directory)
        workspace = root / "work"
        workspace.mkdir()
        skills = root / "skills"
        (skills / "demo").mkdir(parents=True)
        source = skills / "demo" / "SKILL.md"
        source.write_text("---\nname: demo\ndescription: Sample description\n---\nPrivate body token\n",
                          encoding="utf-8")
        (workspace / "AGENTS.md").write_text("Project instruction", encoding="utf-8")
        extra = root / "extra.txt"
        extra.write_text("Extra instruction", encoding="utf-8")
        home = root / "home"
        options = [str(binary), "--cwd", str(workspace),
                   "--sandbox", "none", "--skills-dir", str(skills),
                   "--system-file", str(extra), "--system-prompt", "ignored flag"]

        def run(name, args, error=False):
            prompt = f"!{name} {json.dumps(args, ensure_ascii=False)}"
            proc = subprocess.run([*options, "--home", str(home), prompt], capture_output=True, text=True,
                                  encoding="utf-8", timeout=10)
            assert proc.returncode == 0, proc.stderr
            log = home / "sessions" / (proc.stderr.strip().rsplit("=", 1)[-1] + ".jsonl")
            events = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]
            result = next(event["data"] for event in events if event["type"] == "tool/result")
            assert result["isError"] is error, result
            return result["content"], events[1]["data"]["content"][0]["text"]

        run("write_file", {"path": "nested/deep/example.txt", "content": "one\r\ntwo\r\n"})
        run("write_file", {"path": "nested/top.txt", "content": "other"})
        run("str_replace", {"path": "nested/deep/example.txt", "old_string": "one\ntwo",
                            "new_string": "new\nTEXT"})
        assert (workspace / "nested/deep/example.txt").read_bytes() == b"new\r\nTEXT\r\n"
        result, _ = run("grep", {"pattern": "text", "ignore_case": True, "include": "nested/**/*.txt"})
        assert "nested/deep/example.txt:2: TEXT" in result
        result, _ = run("glob", {"pattern": "nested/**/*.txt"})
        assert set(result.splitlines()) == {"nested/deep/example.txt", "nested/top.txt"}
        result, _ = run("list_dir", {"path": "nested"})
        assert set(result.splitlines()) == {"deep/", "top.txt"}
        result, prompt = run("read_file", {"path": str(source)})
        assert "Private body token" in result
        assert "Project instruction" in prompt
        assert "Sample description" in prompt and "Private body token" not in prompt
        assert "Extra instruction" in prompt and "ignored flag" not in prompt
        run("write_file", {"path": str(source), "content": "denied"}, error=True)
        run("write_file", {"path": "../outside.txt", "content": "denied"}, error=True)
        assert "Private body token" in source.read_text(encoding="utf-8")
        assert not (root / "outside.txt").exists()
        run("write_file", {"path": "repeated.txt", "content": "same same"})
        run("str_replace", {"path": "repeated.txt", "old_string": "same", "new_string": "other"}, error=True)
        assert (workspace / "repeated.txt").read_text(encoding="utf-8") == "same same"


if __name__ == "__main__":
    main()
