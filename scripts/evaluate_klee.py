#!/usr/bin/env python3

import argparse
import os
import random
import subprocess
import csv
import shutil
import io
from concurrent.futures import ProcessPoolExecutor, as_completed

def check_runtime(klee_bin):
    """Check if POSIX runtime and uClibc are available."""
    klee_dir = os.path.dirname(os.path.dirname(os.path.abspath(klee_bin)))
    # Search in build/runtime/lib
    search_paths = [
        os.path.join(klee_dir, "build", "runtime", "lib"),
        os.path.join(klee_dir, "runtime", "lib")
    ]
    
    posix_available = False
    uclibc_available = False
    
    for runtime_dir in search_paths:
        if os.path.exists(runtime_dir):
            files = os.listdir(runtime_dir)
            if any("POSIX" in f for f in files):
                posix_available = True
            if any("klee-uclibc.bca" in f for f in files):
                uclibc_available = True
    
    return posix_available, uclibc_available

def run_klee(config_name, bc_file, output_dir, timeout, klee_path, use_posix, use_uclibc):
    file_basename = os.path.splitext(os.path.basename(bc_file))[0]
    # Put KLEE outputs in a nested klee-out directory
    specific_out_dir = os.path.abspath(os.path.join(output_dir, "klee-out", file_basename, config_name))
    
    if os.path.exists(specific_out_dir):
        shutil.rmtree(specific_out_dir)
    os.makedirs(os.path.dirname(specific_out_dir), exist_ok=True)

    # Coreutils recommended flags
    coreutils_flags = [
        "--optimize",
        "--only-output-states-covering-new",
        f"--output-dir={specific_out_dir}",
        f"--max-time={timeout}s"
    ]
    
    if use_uclibc:
        coreutils_flags.append("--libc=uclibc")
    if use_posix:
        coreutils_flags.append("--posix-runtime")

    # Target program flags for coreutils
    target_flags = ["--sym-args", "0", "3", "10", "--sym-files", "1", "8", "--sym-stdin", "8"]

    cmd = [os.path.abspath(klee_path)] + coreutils_flags

    if "basic" not in config_name:
        cmd += ["--summarize-loops"]
        if "sym-iter" in config_name:
            cmd += ["--summarize-loops-approach=symbolic-iteration"]
        elif "auto" in config_name:
            cmd += ["--summarize-loops-approach=automata"]
        
        if "linear" in config_name:
            cmd += ["--summarize-loops-complexity=linear"]
        elif "matrix" in config_name:
            cmd += ["--summarize-loops-complexity=matrix"]

    cmd += [os.path.abspath(bc_file)] + target_flags

    print(f"[{config_name}] Running: {' '.join(cmd)}")
    try:
        # Increase timeout slightly to allow KLEE to finish cleanup
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout + 60)
        status = "Success" if result.returncode == 0 else f"Failed (Exit {result.returncode})"
        return config_name, bc_file, specific_out_dir, status
    except subprocess.TimeoutExpired:
        return config_name, bc_file, specific_out_dir, "Timeout"
    except Exception as e:
        return config_name, bc_file, specific_out_dir, f"Error: {str(e)}"

def get_stats(specific_out_dir, klee_stats_path):
    if not os.path.exists(os.path.join(specific_out_dir, "run.stats")):
        return {}
    try:
        cmd = [os.path.abspath(klee_stats_path), "--to-csv", specific_out_dir]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode == 0:
            f = io.StringIO(result.stdout.strip())
            reader = csv.DictReader(f)
            data = list(reader)
            if data:
                return data[0]
    except Exception as e:
        print(f"Error getting stats for {specific_out_dir}: {e}")
    return {}

def main():
    parser = argparse.ArgumentParser(description="Evaluate KLEE Loop Summarization")
    parser.add_argument("--input-dir", required=True, help="Directory containing .bc files")
    parser.add_argument("--output-dir", required=True, help="Output directory")
    parser.add_argument("-n", type=int, default=5, help="Number of random files to select")
    parser.add_argument("--timeout", type=int, default=60, help="Timeout in seconds per run")
    parser.add_argument("--klee-bin", default="./build/bin/klee", help="Path to klee binary")
    parser.add_argument("--klee-stats-bin", default="./build/bin/klee-stats", help="Path to klee-stats binary")
    parser.add_argument("--parallel", type=int, default=4, help="Number of parallel processes")
    parser.add_argument("--force-runtime", action="store_true", help="Force use of POSIX/uClibc even if not detected")

    args = parser.parse_args()

    input_dir = os.path.abspath(args.input_dir)
    output_dir = os.path.abspath(args.output_dir)

    bc_files = [os.path.join(input_dir, f) for f in os.listdir(input_dir) if f.endswith(".bc")]
    if not bc_files:
        print(f"No .bc files found in {input_dir}")
        return

    selected_files = random.sample(bc_files, min(len(bc_files), args.n))
    print(f"Selected files: {[os.path.basename(f) for f in selected_files]}")
    
    posix_avail, uclibc_avail = check_runtime(args.klee_bin)
    use_posix = posix_avail or args.force_runtime
    use_uclibc = uclibc_avail or args.force_runtime

    if not use_posix or not use_uclibc:
        print("Warning: POSIX runtime or uClibc not found. Using host libraries (may cause failures).")
        print(f"  POSIX: {'Using' if use_posix else 'Disabled'}")
        print(f"  uClibc: {'Using' if use_uclibc else 'Disabled'}")

    configs = [
        "basic",
        "sym-iter-linear",
        "sym-iter-matrix",
        "auto-linear",
        "auto-matrix"
    ]

    all_results = []

    with ProcessPoolExecutor(max_workers=args.parallel) as executor:
        futures = []
        for bc_file in selected_files:
            for config in configs:
                futures.append(executor.submit(run_klee, config, bc_file, output_dir, args.timeout, args.klee_bin, use_posix, use_uclibc))

        for future in as_completed(futures):
            config_name, bc_file, specific_out_dir, status = future.result()
            print(f"Finished {os.path.basename(bc_file)} [{config_name}]: {status}")
            stats = get_stats(specific_out_dir, args.klee_stats_bin)
            
            # Merge identity info
            entry = {
                'config': config_name,
                'file': os.path.basename(bc_file),
                'status': status
            }
            entry.update(stats)
            all_results.append(entry)

    if all_results:
        # Collect all unique keys for CSV header
        all_keys = set()
        for r in all_results:
            all_keys.update(r.keys())
        
        # Prioritize config, file, status at the beginning
        keys = ['config', 'file', 'status'] + sorted([k for k in all_keys if k not in ['config', 'file', 'status']])
        
        output_csv = os.path.join(output_dir, "results.csv")
        with open(output_csv, 'w', newline='') as f:
            dict_writer = csv.DictWriter(f, keys)
            dict_writer.writeheader()
            dict_writer.writerows(all_results)
        print(f"\nEvaluation complete.")
        print(f"Summary report written to: {output_csv}")
        print(f"Detailed KLEE outputs preserved in: {os.path.join(output_dir, 'klee-out')}")

if __name__ == "__main__":
    main()
