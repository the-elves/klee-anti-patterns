#!/usr/bin/env python3

import os
import shutil
import sys
import argparse
import subprocess
import itertools
import concurrent.futures
from datetime import datetime
import json
import csv
from pathlib import Path

BASE_KLEE_COMMAND = [
    "klee",
    "--simplify-sym-indices",
    "--write-cvcs",
    "--write-cov",
    "--output-module",
    "--max-memory=1000",
    "--disable-preinline",
    "--optimize",
    "--use-forked-solver",
    "--use-cex-cache",
    "--libc=uclibc",
    "--posix-runtime",
    "--external-calls=all",
    "--only-output-states-covering-new",
    "--env-file=test.env",
    "--run-in-dir=/tmp/sandbox",
    "--max-sym-array-size=4096",
    "--max-solver-time=30s",
    "--max-time=30s",
    "--watchdog",
    "--max-memory-inhibit=false",
    "--max-static-fork-pct=1",
    "--max-static-solve-pct=1",
    "--max-static-cpfork-pct=1",
    "--switch-type=internal",
    "--search=random-path",
    "--search=nurs:covnew",
    "--use-batching-search",
    "--batch-instructions=10000"
]

KLEE_ARGS_POST = [
    "--sym-args", "0", "1", "10",
    "--sym-args", "0", "2", "2",
    "--sym-files", "1", "8",
    "--sym-stdout"
]

def get_pass_combinations(passes, permute_all):
    if not permute_all:
        return [tuple(passes)]
    
    combos = []
    for r in range(1, len(passes) + 1):
        combos.extend(itertools.combinations(passes, r))
    return combos

def run_klee(bc_path, pass_combo, order, out_root, max_time):
    bench_name = bc_path.stem
    pass_str = ",".join(pass_combo) if pass_combo else "none"
    run_name = f"{pass_str}_{order}"
    run_dir = out_root / bench_name / run_name
    run_dir.parent.mkdir(parents=True, exist_ok=True)

    cmd = [c for c in BASE_KLEE_COMMAND if not c.startswith("--max-time=")]
    cmd.append(f"--max-time={max_time}")
    cmd.append(f"--output-dir={run_dir}")
    
    if pass_combo:
        cmd.append(f"--symex-opts={','.join(pass_combo)}")
    
    if order == "after":
        cmd.append("--symex-opts-after-klee")
    
    cmd.append(str(bc_path))
    cmd.extend(KLEE_ARGS_POST)

    # Run KLEE
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        status = "Success" if proc.returncode == 0 else f"Exit Code {proc.returncode}"
        stdout_log = proc.stdout
        stderr_log = proc.stderr
    except Exception as e:
        status = f"Error: {str(e)}"
        stdout_log = ""
        stderr_log = str(e)

    # Ensure run_dir exists (it should if KLEE ran, but just in case of early failure)
    run_dir.mkdir(parents=True, exist_ok=True)

    # Save logs and config
    with open(run_dir / "klee_stdout.log", "w") as f:
        f.write(stdout_log)
    with open(run_dir / "klee_stderr.log", "w") as f:
        f.write(stderr_log)

    config = {
        "timestamp": datetime.now().isoformat(),
        "bc_file": str(bc_path),
        "passes": pass_combo,
        "order": order,
        "command": cmd,
        "status": status
    }
    with open(run_dir / "config.json", "w") as f:
        json.dump(config, f, indent=2)

    # Collect stats
    stats_csv = run_dir / "stats.csv"
    try:
        # klee-stats --table-format=csv prints processed stats with ICov(%), BCov(%), etc.
        stats_proc = subprocess.run(["klee-stats", "--print-all", "--table-format=csv", str(run_dir)], 
                                    capture_output=True, text=True, check=True)
        stats_data = stats_proc.stdout
        with open(stats_csv, "w") as f:
            f.write(stats_data)
        
        # Parse coverage from stats_data
        reader = csv.DictReader(stats_data.splitlines())
        row = next(reader)
        return {
            "Benchmark": bench_name,
            "Passes": pass_str,
            "Order": order,
            "ICov(%)": row.get("ICov(%)", "0.0"),
            "BCov(%)": row.get("BCov(%)", "0.0"),
            "Paths": row.get("TermExit", row.get("Paths", "0")),
            "Time(s)": row.get("Time(s)", "0.0"),
            "SolverTime(s)": row.get("TSolver(s)", "0.0"),
            "Status": status
        }
    except Exception as e:
        return {
            "Benchmark": bench_name,
            "Passes": pass_str,
            "Order": order,
            "ICov(%)": "N/A",
            "BCov(%)": "N/A",
            "Paths": "N/A",
            "Time(s)": "N/A",
            "SolverTime(s)": "N/A",
            "Status": f"Stats Error: {str(e)}"
        }

def create_environment(out_dir: Path, assets_path:Path):
    shutil.copy(assets_path/"testing-env.sh", out_dir)
    shutil.copy(assets_path/"sandbox.tgz", "/tmp")
    os.chdir("/tmp")
    subprocess.run(["tar", "xfv", "sandbox.tgz"])
    os.chdir(out_dir)
    subprocess.run("env -i /bin/bash -c".split(" ")+["(source testing-env.sh; env >test.env)"])

def main():
    parser = argparse.ArgumentParser(description="Automate KLEE optimization pass evaluation.")
    parser.add_argument("--bench-dir", required=True, help="Directory containing .bc files")
    parser.add_argument("--passes", nargs="+", required=True, help="Names of optimization passes")
    parser.add_argument("--out-dir", help="Output root directory")
    parser.add_argument("--permute-all", action="store_true", help="Run all combinations of passes")
    parser.add_argument("--both-before-after", action="store_true", help="Run passes both before and after KLEE opts")
    parser.add_argument("--jobs", type=int, default=os.cpu_count(), help="Number of parallel jobs")
    parser.add_argument("--assets-dir", type=Path, default=Path(Path.home()/"Workspace/klee/assets"))
    parser.add_argument("--max-time", default="60min", help="Max time for KLEE (e.g., 60s, 1h, 60mins)")

    args = parser.parse_args()

    bench_dir = Path(args.bench_dir).expanduser().resolve()
    if not bench_dir.is_dir():
        print(f"Error: {bench_dir} is not a directory.")
        sys.exit(1)

    if args.out_dir:
        out_root = Path(args.out_dir).expanduser().resolve()
    else:
        timestamp = datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
        out_root = Path("~/Workspace/AntiPasses-Evaluation").expanduser() / timestamp
    
    out_root.mkdir(parents=True, exist_ok=True)
    print(f"Output directory: {out_root}")

    bc_files = list(bench_dir.glob("*.bc"))
    if not bc_files:
        print(f"No .bc files found in {bench_dir}")
        sys.exit(1)

    create_environment(out_root, args.assets_dir)

    pass_combos = get_pass_combinations(args.passes, args.permute_all)
    # Add a baseline run with no passes
    pass_combos.insert(0, tuple())

    orders = ["before"]
    if args.both_before_after:
        orders.append("after")

    tasks = []
    for bc_file in bc_files:
        for combo in pass_combos:
            for order in orders:
                tasks.append((bc_file, combo, order, out_root, args.max_time))

    results = []
    print(f"Starting {len(tasks)} tasks with {args.jobs} parallel jobs...")
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as executor:
        future_to_task = {executor.submit(run_klee, *task): task for task in tasks}
        for future in concurrent.futures.as_completed(future_to_task):
            res = future.result()
            results.append(res)
            print(f"Finished: {res['Benchmark']} | {res['Passes']} | {res['Order']} | ICov: {res['ICov(%)']}%")

    # Aggregate by benchmark
    benchmarks = sorted(list(set(r['Benchmark'] for r in results)))
    headers = ["Benchmark", "Passes", "Order", "ICov(%)", "BCov(%)", "Paths", "Time(s)", "SolverTime(s)", "Status"]
    
    for bench in benchmarks:
        bench_results = [r for r in results if r['Benchmark'] == bench]
        bench_csv = out_root / bench / "coverage.csv"
        with open(bench_csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=headers)
            writer.writeheader()
            writer.writerows(bench_results)

    # Aggregate Master CSV
    master_csv = out_root / "master_coverage.csv"
    with open(master_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=headers)
        writer.writeheader()
        writer.writerows(results)

    print(f"\nEvaluation complete. Master results saved to: {master_csv}")

if __name__ == "__main__":
    main()
