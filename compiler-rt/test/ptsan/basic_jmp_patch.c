// RUN: rm -rf %t && mkdir -p %t && cd %t

// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-patch-all-with=2 -o %exe.all.patched %s
// RUN: ./%exe.all.patched 23 2> log.txt
// RUN: cat log.txt | FileCheck %s --check-prefix=PATCHED

// PATCHED: jmp patch catch

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>

extern jmp_buf *ptsan_get_jmp_env();

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
  inner(idx + 3, buf);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  volatile int j = setjmp(*ptsan_get_jmp_env());

  printf("Setjmp_value %d\n", j);

  if(j > 0) {
    fprintf(stderr, "jmp patch catch\n");
    return 0;
  }

  int idx = atoi(argv[1]);
  outer(idx);

  fprintf(stderr, "got to the end. Aborting!\n");
  abort();
}