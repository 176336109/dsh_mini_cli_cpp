"""Run the original evaluation fixtures and assertions against both Linux binaries."""

import argparse
import importlib.util
import json
import os
import pathlib
from concurrent.futures import ThreadPoolExecutor


def load_runner(path):
    spec = importlib.util.spec_from_file_location("dsh_reference_evals", path)
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    return runner


def summarize(rows):
    return {
        name: {
            "passed": sum(row[name]["pass"] for row in rows),
            "steps": sum(row[name]["steps"] for row in rows),
            "tool_errors": sum(row[name]["tool_errors"] for row in rows),
            "tokens_in": sum(row[name]["tokens_in"] for row in rows),
            "tokens_out": sum(row[name]["tokens_out"] for row in rows),
        }
        for name in ("rust", "cpp")
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "rust", "cpp", "mcp-server", "work", "report"):
        parser.add_argument("--" + name, type=pathlib.Path, required=True)
    parser.add_argument("--model", default="deepseek-flash")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    runner = load_runner(args.runner.resolve(strict=True))
    binaries = {"rust": args.rust.resolve(strict=True), "cpp": args.cpp.resolve(strict=True)}
    mcp_server = args.mcp_server.resolve(strict=True)
    runner.mock_mcp_binary = lambda: mcp_server
    cases = runner.load_cases(None)
    args.work.mkdir(parents=True, exist_ok=False)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    rows = []
    api_key = os.environ.get("DEEPSEEK_API_KEY")

    def run(name, case):
        result = runner.run_case(case, args, binaries[name], args.work / name)
        if api_key:
            for field in ("stdout_tail", "stderr_tail"):
                result[field] = result[field].replace(api_key, "[redacted]")
            result["reasons"] = [reason.replace(api_key, "[redacted]") for reason in result["reasons"]]
        return result

    def write_report():
        report = {
            "model": args.model,
            "total_cases": len(cases),
            "completed_pairs": len(rows),
            "binaries": {name: str(path) for name, path in binaries.items()},
            "totals": summarize(rows),
            "rust_only": [row["id"] for row in rows if row["rust"]["pass"] and not row["cpp"]["pass"]],
            "cpp_only": [row["id"] for row in rows if row["cpp"]["pass"] and not row["rust"]["pass"]],
            "both_failed": [row["id"] for row in rows if not row["rust"]["pass"] and not row["cpp"]["pass"]],
            "cases": rows,
        }
        draft = args.report.with_suffix(".tmp")
        draft.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        draft.replace(args.report)
        return report

    with ThreadPoolExecutor(max_workers=2) as pool:
        for case in cases:
            jobs = {name: pool.submit(run, name, case) for name in binaries}
            row = {"id": case["id"], "category": case["category"]}
            for name, job in jobs.items():
                row[name] = job.result()
            rows.append(row)
            report = write_report()
            statuses = " · ".join(
                f"{name} {'PASS' if row[name]['pass'] else 'FAIL'} ({row[name]['steps']} steps)"
                for name in binaries
            )
            print(f"{len(rows):02}/{len(cases)} {row['id']} {statuses}", flush=True)
            if len(rows) == 1 and all(
                row[name]["exit_code"] != 0 and row[name]["tokens_in"] == 0 for name in binaries
            ):
                raise SystemExit("Both agents failed before receiving model usage. Inspect the first pair before retrying.")
    totals = report["totals"]
    print(f"Rust {totals['rust']['passed']}/{len(cases)}, "
          f"C++ {totals['cpp']['passed']}/{len(cases)} · {args.report}", flush=True)


if __name__ == "__main__":
    main()
