"""Round-trip a real JSONL session across the Rust and C++ executables."""

import argparse
import json
import pathlib
import subprocess
import tempfile


def invoke(binary, root, home, *args):
    result = subprocess.run(
        [str(binary), "--cwd", str(root), "--home", str(home), "--sandbox", "none",
         "--model", "mock", *args],
        capture_output=True, text=True, encoding="utf-8", timeout=15,
    )
    if result.returncode:
        raise RuntimeError(f"{binary} exited with {result.returncode}: {result.stderr}")


def events(log):
    return [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rust", type=pathlib.Path, required=True)
    parser.add_argument("--cpp", type=pathlib.Path, required=True)
    options = parser.parse_args()
    rust, cpp = options.rust.resolve(), options.cpp.resolve()
    with tempfile.TemporaryDirectory(prefix="dsh-compat-") as temporary:
        root = pathlib.Path(temporary)
        home = root / "data"
        (root / "fixture.txt").write_text("fixture contents\n", encoding="utf-8")
        prompt = '!read_file {"path":"fixture.txt"}'
        invoke(rust, root, home, prompt)
        log, = (home / "sessions").glob("*.jsonl")
        original = events(log)
        invoke(cpp, root, home, "--resume", log.stem, prompt)
        combined = events(log)
        assert combined[:len(original)] == original
        assert combined[len(original)]["seq"] == original[-1]["seq"] + 1
        assert combined[-1]["data"] == {"turn": 2, "reason": "completed"}

        invoke(rust, root, home, "--resume", log.stem, prompt)
        final = events(log)
        assert final[:len(combined)] == combined
        assert final[len(combined)]["seq"] == combined[-1]["seq"] + 1
        assert final[-1]["data"] == {"turn": 3, "reason": "completed"}
        assert len([item for item in final if item["type"] == "system/message"]) == 1
        print(f"Rust → C++ → Rust passed: {len(final)} events, 3 turns, 1 system message")


if __name__ == "__main__":
    main()
