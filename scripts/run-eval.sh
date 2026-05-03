set -xe
scripts/evaluate_passes.py run --bench-dir /home1/ajinkya/Workspace/Garuda2/benchmarks/build/coreutils-obj-llvm/bin --max-time $RUN_EVAL_TIME --out-dir $1 --passes $2
