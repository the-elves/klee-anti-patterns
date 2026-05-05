// RUN: %clang %s -emit-llvm -g -c -O0 -o %t.bc
// RUN: %klee --symex-opts=heap-object-linearization %t.bc 2>&1 | %FileCheck %s

#include <stdlib.h>
#include <stdio.h>

int main() {
    int *a = (int*)malloc(4);
    int *b = (int*)malloc(4);
    *a = 10;
    *b = 20;
    // CHECK: Heap-Object-linearization: Linearizing 2 heap objects into 8 bytes
    if (*a + *b == 30) return 0;
    return 1;
}
