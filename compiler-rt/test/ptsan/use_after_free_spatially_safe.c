// RUN: rm -rf %t && mkdir -p %t && cd %t

// Check both O0 and O2
// RUN: %clang_ptsan -g -O0 -o %exe.O0 %s
// RUN: not ./%exe.O0 2> err.O0.txt
// RUN: cat err.O0.txt | FileCheck %s

// RUN: %clang_ptsan -g -O2 -o %exe.O2 %s
// RUN: not ./%exe.O2 2> err.O2.txt
// RUN: cat err.O2.txt | FileCheck %s

// RUN: %clang_ptsan_id32 -g -O0 -o %exe.O0 %s
// RUN: not ./%exe.O0 2> err.O0.txt
// RUN: cat err.O0.txt | FileCheck %s
// RUN: %clang_ptsan_id32 -g -O2 -o %exe.O2 %s
// RUN: not ./%exe.O2 2> err.O2.txt
// RUN: cat err.O2.txt | FileCheck %s

// CHECK: Found bad

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Inputs/ptsan_test_interface.h"

__attribute__((noinline))
uint64_t get_addr(char* ptr) {
    return ptsan_get_address(ptr);
}

__attribute__((noinline))
void use(char *name, char *ptr) {
  printf("%s (id %d) = %s\n", name, get_id(ptr), ptr);
}

__attribute__((noinline))
void func() {
  char *b1 = malloc(16);
  free(b1);
  char *b2 = malloc(16);

  // Make sure the UAF will actualy happen
  assert(get_addr(b1) == get_addr(b2));

  printf("Base: 0x%lx; size: 0x%lx\n",
          ptsan_get_base(get_id(b1)), ptsan_get_size(get_id(b1)));
  b1[0] = 'a';
  b1[1] = 'b';
  b1[2] = '\0';

  use("b1", b1);
  use("b2", b2);
}

int main(void) {
  func();
}
