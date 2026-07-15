// REQUIRES: ptsan-lam-u48-runtime
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-lam -o %exe %s
// RUN: ./%exe 30 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 21 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 20 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=BAD
// RUN: ./%exe 19 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD

// BAD: Found bad POINT access
// GOOD-NOT: Found bad POINT access


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

uint32_t ptsan_id_bits();
uint32_t ptsan_id_bits();
uint64_t ptsan_num_ids() {
  return  (1 << ptsan_id_bits()) - 1;
}
uint64_t ptsan_max_id() {
  return ptsan_num_ids() - 1;
}
uint64_t ptsan_default_id() {
  return ptsan_num_ids();
}

void __attribute__((noinline)) inner(int idx, char *buf) {
  char c = buf[idx];
  printf("buf[%d] = %c\n", idx, c);
}

void __attribute__((noinline)) outer(int idx) {
  char buf[20];
  for (int i = 0; i < 20; i++) {
    buf[i] = 'A' + (i % 26);
  }
  inner(idx, buf);
}

int main(int argc, char *argv[]) {
  assert(ptsan_id_bits() == 15);
  assert(ptsan_default_id() == 0x7fff);

  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  int idx = atoi(argv[1]);
  outer(idx);

  return 0;
}
