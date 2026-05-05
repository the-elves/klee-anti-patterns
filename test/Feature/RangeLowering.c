// RUN: %clang %s %O0opt -I %include -emit-llvm -g -c -o %t.bc
// RUN: %klee --symex-opts=range-lowering %t.bc
// RUN: test -f range_lowering_stats.txt
// RUN: cat range_lowering_stats.txt | %FileCheck %s

#include "klee/klee.h"

int main() {
  unsigned char x = klee_range(0, 4, "x");
  unsigned char y = klee_range(0, 4, "y");
  
  // Even at -O0, these might be loaded/stored, but klee_range result 
  // might be used directly in some cases or we can hope LVI sees through it.
  // Actually, LVI is good at seeing through loads/stores if they are simple.
  
  unsigned int ix = x;
  unsigned int iy = y;
  unsigned int iz = ix + iy; 
  
  // CHECK: Lowerings: {{[1-9][0-9]*}}
  return iz;
}
