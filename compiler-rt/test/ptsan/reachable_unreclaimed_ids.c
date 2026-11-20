// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O1 -o ptsan %s
// RUN: ./ptsan 2> ptsan.err
// RUN: FileCheck %s --check-prefix=PTSAN < ptsan.err

#include <stdlib.h>

void *global_root;

__attribute__((noinline, noreturn)) static void exit_with_stack_root(
    void *stack_root) {
  asm volatile("" : : "r"(stack_root), "m"(global_root) : "memory");
  exit(0);
}

int main(void) {
  global_root = malloc(13);
  void *stack_root = malloc(17);
  if (!global_root || !stack_root)
    abort();

  exit_with_stack_root(stack_root);
}

// PTSAN: Unreclaimed IDs at exit: 2
