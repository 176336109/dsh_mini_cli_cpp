"""Compare Rust and C++ release binaries on one Linux host with an identical fixture."""

import argparse
import json
import os
import pathlib
import platform
import statistics
import subprocess
import tempfile
import time


def measure(binary, work, home, stdin, scratch):
    metrics = scratch / "time.txt"
    start = time.perf_counter_ns()
    subprocess.run(
        ["/usr/bin/time", "-f", "%U %S %M", "-o", str(metrics), str(binary),
         "--cwd", str(work), "--home", str(home), "--sandbox", "landlock", "--model", "mock"],
        input=stdin, text=True, encoding="utf-8", check=True, timeout=180,
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
    )
    elapsed = (time.perf_counter_ns() - start) / 1_000_000
    user, system, rss = metrics.read_text().split()
    sessions = list((home / "sessions").glob("*.jsonl"))
    total = sum(file.stat().st_size for file in sessions)
    turns = sum(
        json.loads(line)["type"] == "turn/end"
        for file in sessions
        for line in file.read_text(encoding="utf-8").splitlines()
    )
    return {"wall_ms": elapsed, "cpu_seconds": float(user) + float(system),
            "peak_rss_kib": int(rss), "log_bytes": total, "turns_logged": turns}


def binary_info(binary):
    linked = subprocess.run(["ldd", str(binary)], capture_output=True, text=True, check=False)
    return {"path": str(binary), "bytes": binary.stat().st_size,
            "ldd": (linked.stdout + linked.stderr).strip()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rust", type=pathlib.Path, required=True)
    parser.add_argument("--cpp", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--repeat", type=int, default=7)
    args = parser.parse_args()
    if platform.system() != "Linux" or args.repeat <= 0:
        parser.error("Linux and a positive repeat count are required")

    prompt = '!read_file {"path":"fixture.txt"}\n'
    report = {"kernel": platform.release(), "machine": platform.machine(),
              "cpu_affinity": sorted(os.sched_getaffinity(0)), "samples": {}}
    with tempfile.TemporaryDirectory(prefix="dsh-bench-") as directory:
        root = pathlib.Path(directory)
        work = root / "work"
        work.mkdir()
        with (work / "fixture.txt").open("w", encoding="utf-8") as file:
            for number in range(180):
                file.write(f"{number:04d} " + "fixture " * 15 + "\n")
        for name, binary in (("rust", args.rust.resolve()), ("cpp", args.cpp.resolve())):
            launches = [measure(binary, work, root / f"{name}-cold-{i}", prompt, root)
                        for i in range(args.repeat)]
            samples = {"binary": binary_info(binary), "cold_samples": launches,
                       "median_cold_ms": statistics.median(run["wall_ms"] for run in launches),
                       "long_sessions": {}}
            for turns in [10, 50, 150, 300]:
                home = root / f"{name}-long-{turns}"
                samples["long_sessions"][turns] = measure(binary, work, home, prompt * turns, root)
            report["samples"][name] = samples
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
