---
name: klee-symex-opt-pass
description: Guidance for adding new symbolic execution optimization passes to KLEE. Use this skill when you need to implement a new function pass under lib/SymExOptPasses and register it for use with the --symex-opts flag.
---

# KLEE Symbolic Execution Optimization Passes

This skill provides the procedural knowledge and templates required to extend KLEE with new optimization passes that run before or after KLEE's built-in optimizations.

## Core Infrastructure

Custom passes live in `lib/SymExOptPasses/` and are managed by the `applySymExOptPasses` function in `lib/SymExOptPasses/Passes.cpp`.

## Adding a New Pass

To add a new pass, follow the structured workflow provided in the reference:

1.  **Workflow Guide**: See [references/workflow.md](references/workflow.md) for step-by-step instructions on file creation, CMake registration, and integration into the `Passes.cpp` logic.
2.  **Code Template**: Use [references/template-pass.cpp](references/template-pass.cpp) as a starting point for your new LLVM `FunctionPass`.

## Key Files to Modify

- `lib/SymExOptPasses/CMakeLists.txt`: To add your new source file to the library.
- `lib/SymExOptPasses/Passes.cpp`: To map a string name (e.g., "my-opt") to your pass instance in the `applySymExOptPasses` function.

## Usage

Once registered, your pass can be invoked via KLEE's command line:

```bash
klee --symex-opts=my-opt,another-opt ...
```

To run your passes *after* KLEE's default optimizations:

```bash
klee --symex-opts=my-opt --symex-opts-after-klee ...
```
