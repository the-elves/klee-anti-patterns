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

def get_stats(run_dir, bench_name, pass_combo, order, status):
    pass_str = ",".join(pass_combo) if pass_combo else "none"
    stats_csv = run_dir / "stats.csv"
    try:
        # klee-stats --print-all --to-csv prints processed stats
        stats_cmd = ["klee-stats", "--print-all", "--to-csv", str(run_dir)]
        stats_proc = subprocess.run(stats_cmd, capture_output=True, text=True, check=True)
        stats_data = stats_proc.stdout
        with open(stats_csv, "w") as f:
            f.write(stats_data)
        
        lines = stats_data.splitlines()
        if len(lines) < 2:
            raise ValueError("klee-stats output too short")
            
        reader = csv.DictReader(lines)
        rows = list(reader)
        if not rows:
            raise ValueError("klee-stats output has no data")

        # When running on a single directory, klee-stats might only return one row 
        # (which is both the directory stats and the total).
        # Usually it's: Row 0 = Total, Row 1 = Directory 1, ...
        # If there's only one directory, we take the last row.
        row = rows[-1]
        
        # Calculate percentages
        try:
            covered_insts = float(row.get("CoveredInstructions", 0))
            uncovered_insts = float(row.get("UncoveredInstructions", 0))
            total_static_insts = covered_insts + uncovered_insts
            icov = (covered_insts / total_static_insts * 100) if total_static_insts > 0 else 0.0
        except:
            icov = 0.0
        
        try:
            total_branches = float(row.get("NumBranches", 1))
            full_branches = float(row.get("FullBranches", 0))
            partial_branches = float(row.get("PartialBranches", 0))
            bcov = ((full_branches + 0.5 * partial_branches) / total_branches * 100) if total_branches > 0 else 0.0
        except:
            bcov = 0.0

        def to_seconds(val):
            try:
                return f"{float(val) / 1000000.0:.2f}"
            except:
                return "0.00"

        return {
            "Benchmark": bench_name,
            "Passes": pass_str,
            "Order": order,
            "ICov(%)": f"{icov:.2f}",
            "BCov(%)": f"{bcov:.2f}",
            "Paths": row.get("TerminationExit", "0"),
            "Time(s)": to_seconds(row.get("WallTime", 0)),
            "SolverTime(s)": to_seconds(row.get("SolverTime", 0)),
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

def run_klee(bc_path, pass_combo, order, out_root, max_time):
    bench_name = bc_path.stem
    pass_str = "_".join(pass_combo) if pass_combo else "none"
    run_name = f"{pass_str}_{order}"
    run_dir = out_root / bench_name / run_name
    run_dir.parent.mkdir(parents=True, exist_ok=True)
    
    transformed_bc_dir = out_root / "transformed_bitcodes"

    cmd = [c for c in BASE_KLEE_COMMAND if not c.startswith("--max-time=") and not c.startswith("--env-file=")]
    cmd.append(f"--max-time={max_time}")
    cmd.append(f"--output-dir={run_dir}")
    cmd.append(f"--env-file={out_root / 'test.env'}")
    
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

    # Ensure run_dir exists
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

    # Capture transformed bitcode
    assembly_ll = run_dir / "assembly.ll"
    final_bc = run_dir / "final.bc"
    transformed_bc = transformed_bc_dir / f"{bench_name}_{run_name}.bc"
    
    if final_bc.exists():
        shutil.copy(str(final_bc), str(transformed_bc))
    elif assembly_ll.exists():
        subprocess.run(["llvm-as-16", str(assembly_ll), "-o", str(transformed_bc)], check=False)

    return get_stats(run_dir, bench_name, pass_combo, order, status)

def create_environment(out_dir: Path, assets_path:Path):
    if not (assets_path/"testing-env.sh").exists():
        print(f"Warning: assets-dir {assets_path} does not contain testing-env.sh")
        return
    shutil.copy(assets_path/"testing-env.sh", out_dir)
    shutil.copy(assets_path/"sandbox.tgz", "/tmp")
    os.chdir("/tmp")
    subprocess.run(["tar", "xfv", "sandbox.tgz"])
    os.chdir(out_dir)
    subprocess.run("env -i /bin/bash -c".split(" ")+["(source testing-env.sh; env >test.env)"])

def aggregate_results(results, out_root):
    # Unpivoted Results
    headers = ["Benchmark", "Passes", "Order", "ICov(%)", "BCov(%)", "Paths", "Time(s)", "SolverTime(s)", "Status"]
    raw_csv = out_root / "raw_coverage.csv"
    with open(raw_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=headers)
        writer.writeheader()
        writer.writerows(results)
    
    # Per-benchmark CSVs
    benchmarks = sorted(list(set(r['Benchmark'] for r in results)))
    for bench in benchmarks:
        bench_results = [r for r in results if r['Benchmark'] == bench]
        bench_csv = out_root / bench / "coverage.csv"
        bench_csv.parent.mkdir(parents=True, exist_ok=True)
        with open(bench_csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=headers)
            writer.writeheader()
            writer.writerows(bench_results)

    # Master Pivoted CSV
    master_csv = out_root / "master_coverage.csv"
    
    all_configs = sorted(list(set((r["Passes"], r["Order"]) for r in results)))
    
    metrics = ["ICov(%)", "BCov(%)", "Paths", "Time(s)", "SolverTime(s)", "Status"]
    numeric_metrics = ["ICov(%)", "BCov(%)", "Paths", "Time(s)", "SolverTime(s)"]
    baseline_config = ("none", "before")
    
    master_headers = ["Benchmark"]
    for p, o in all_configs:
        for m in metrics:
            master_headers.append(f"{p}_{o}_{m}")
        for m in numeric_metrics:
            master_headers.append(f"{p}_{o}_{m}_%")
    
    pivoted_results = {}
    for r in results:
        bench = r["Benchmark"]
        if bench not in pivoted_results:
            pivoted_results[bench] = {"Benchmark": bench}
        
        p = r["Passes"]
        o = r["Order"]
        for m in metrics:
            pivoted_results[bench][f"{p}_{o}_{m}"] = r[m]

    # Calculate % changes relative to baseline
    for bench in pivoted_results:
        row = pivoted_results[bench]
        bp, bo = baseline_config
        for p, o in all_configs:
            for m in numeric_metrics:
                col_name = f"{p}_{o}_{m}"
                pct_col_name = f"{p}_{o}_{m}_%"
                
                val = row.get(col_name)
                base_val = row.get(f"{bp}_{bo}_{m}")
                
                try:
                    v = float(val)
                    bv = float(base_val)
                    if bv == 0:
                        row[pct_col_name] = "0.0" if v == 0 else "inf"
                    else:
                        row[pct_col_name] = f"{(v - bv) / bv * 100:.2f}"
                except (ValueError, TypeError, ZeroDivisionError):
                    row[pct_col_name] = "N/A"

    with open(master_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=master_headers)
        writer.writeheader()
        for bench in sorted(pivoted_results.keys()):
            writer.writerow(pivoted_results[bench])

    print(f"\nAggregation complete.")
    print(f"Raw results (unpivoted): {raw_csv}")
    print(f"Master results (pivoted): {master_csv}")

def collect_existing(out_root, jobs):
    out_root = Path(out_root).expanduser().resolve()
    print(f"Collecting results from {out_root}")
    
    # Find all config.json files
    config_files = list(out_root.glob("**/config.json"))
    print(f"Found {len(config_files)} run directories.")
    
    results = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as executor:
        futures = []
        for config_path in config_files:
            run_dir = config_path.parent
            with open(config_path, "r") as f:
                config = json.load(f)
            
            bench_name = Path(config["bc_file"]).stem
            pass_combo = config["passes"]
            order = config["order"]
            status = config.get("status", "Unknown")
            
            futures.append(executor.submit(get_stats, run_dir, bench_name, pass_combo, order, status))
        
        for future in concurrent.futures.as_completed(futures):
            try:
                res = future.result()
                if res:
                    results.append(res)
                    print(f"Collected: {res['Benchmark']} | {res['Passes']} | {res['Order']}")
            except Exception as e:
                print(f"Error collecting result: {e}")
    
    if results:
        aggregate_results(results, out_root)
    else:
        print("No results collected.")

def main():
    parser = argparse.ArgumentParser(description="Automate KLEE optimization pass evaluation.")
    subparsers = parser.add_subparsers(dest="command", help="Command to run")
    subparsers.required = True

    # Run command
    run_parser = subparsers.add_parser("run", help="Run KLEE and collect stats")
    run_parser.add_argument("--bench-dir", required=True, help="Directory containing .bc files")
    run_parser.add_argument("--passes", nargs="+", required=True, help="Names of optimization passes")
    run_parser.add_argument("--out-dir", help="Output root directory")
    run_parser.add_argument("--permute-all", action="store_true", help="Run all combinations of passes")
    run_parser.add_argument("--both-before-after", action="store_true", help="Run passes both before and after KLEE opts")
    run_parser.add_argument("--jobs", type=int, default=os.cpu_count(), help="Number of parallel jobs")
    run_parser.add_argument("--assets-dir", type=Path, default=Path(Path.home()/"Workspace/klee/assets"))
    run_parser.add_argument("--max-time", default="60min", help="Max time for KLEE (e.g., 60s, 1h, 60mins)")

    # Collect command
    collect_parser = subparsers.add_parser("collect", help="Collect stats from an existing output folder")
    collect_parser.add_argument("--out-dir", required=True, help="Output root directory to collect from")
    collect_parser.add_argument("--jobs", type=int, default=os.cpu_count(), help="Number of parallel jobs")

    args = parser.parse_args()

    if args.command == "collect":
        collect_existing(args.out_dir, args.jobs)
        return

    # Run command logic
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
    (out_root / "transformed_bitcodes").mkdir(exist_ok=True)
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
            try:
                res = future.result()
                if res:
                    results.append(res)
                    print(f"Finished: {res['Benchmark']} | {res['Passes']} | {res['Order']} | ICov: {res['ICov(%)']}%")
            except Exception as e:
                print(f"Task failed: {e}")

    if results:
        aggregate_results(results, out_root)
    else:
        print("No results generated.")

if __name__ == "__main__":
    main()
