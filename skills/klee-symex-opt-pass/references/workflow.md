# Workflow for Adding a New SymEx Optimization Pass

Follow these steps to add a new pass to the KLEE SymEx optimization infrastructure.

## 1. Create the Pass Source File
Create a new `.cpp` file in `lib/SymExOptPasses/`.
Use the boilerplate from `template-pass.cpp` as a starting point.

## 2. Register the Pass in CMake
Update `lib/SymExOptPasses/CMakeLists.txt` to include your new `.cpp` file in the `add_library` command.

```cmake
add_library(kleeSymExOptPasses
  Passes.cpp
  YourNewPass.cpp  # Add this line
)
```

## 3. Declare the Factory Function
If your pass is in an anonymous namespace (recommended), declare a factory function in a internal header or directly in `Passes.cpp` (if simple).
Ideally, add it to `lib/SymExOptPasses/Passes.h` (the internal one, if created) or keep it local if only used in `Passes.cpp`.

## 4. Update the Pass Manager Logic
Modify `lib/SymExOptPasses/Passes.cpp` to recognize your pass name.

Inside `applySymExOptPasses`:
```cpp
    if (passName == "your-pass-name") {
      PM.add(createYourNewPass());
      added = true;
    }
```

## 5. Build and Verify
1. Rebuild KLEE: `cd build && make klee`
2. Run KLEE with your new pass: `./bin/klee --symex-opts=your-pass-name your_bitcode.bc`
