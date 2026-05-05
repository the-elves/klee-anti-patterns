// RUN: %clang %s -I include -emit-llvm -g -c -O0 -o %t.bc
// RUN: %klee --symex-opts=branch-pruning %t.bc 2>&1 | %FileCheck %s

#include "klee/klee.h"

int main() {
  int x = klee_int("x");

  if (x < 5) {
    // x is in [min, 4]
    // The next condition x < 10 is always true.
    if (x < 10) {
      // CHECK: Branch-Pruning: Total Pruned={{.*}}, Total InstructionsRemoved={{.*}}, Total InputDependent={{.*}}
      return 1;
    } else {
      // This part should be pruned
      return 2;
    }
  }

  if (0) {
     return 3;
  }

  return 0;
}
