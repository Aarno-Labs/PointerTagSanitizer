// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -S -emit-llvm -o %exe.ll %s
// RUN: cat %exe.ll | FileCheck %s --check-prefix=IR-COND
// RUN: cat %exe.ll | FileCheck %s --check-prefix=IR-DOM
// RUN: cat %exe.ll | FileCheck %s --check-prefix=IR-POSTDOM

// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 0 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD
// RUN: ./%exe 16 2> out.txt
// RUN: cat out.txt | FileCheck %s --check-prefix=GOOD
// RUN: for idx in 17 20; do \
// RUN:   not ./%exe $idx 1 2> out.txt && FileCheck %s --check-prefix=BAD-POINT < out.txt && \
// RUN:   not ./%exe $idx 2 2> out.txt && FileCheck %s --check-prefix=BAD-COMPOUND < out.txt && \
// RUN:   not ./%exe $idx 3 2> out.txt && FileCheck %s --check-prefix=BAD-COMPOUND < out.txt || exit 1; \
// RUN: done

// BAD-POINT: Found bad POINT access
// BAD-COMPOUND: Found bad COMPOUND access

// GOOD-NOT: Found bad access

// IR-COND-LABEL: define dso_local void @wider_conditional
// IR-COND-DAG: @ptsan_complain(i32 0,
// IR-COND-DAG: @ptsan_complain(i32 1,
// IR-COND: define dso_local

// IR-DOM-LABEL: define dso_local void @wider_above
// IR-DOM: @ptsan_complain(i32 0,
// IR-DOM-NOT: @ptsan_complain(i32 1,
// IR-DOM: define dso_local

// IR-POSTDOM-LABEL: define dso_local void @wider_unconditional
// IR-POSTDOM: @ptsan_complain(i32 0,
// IR-POSTDOM-NOT: @ptsan_complain(i32 1,
// IR-POSTDOM: define dso_local

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// The wider access is conditional, so no optimization should be possible
void __attribute__((noinline)) wider_conditional(size_t idx, char *buf, bool flag) {
  char *addr = buf + idx;
  char c = *((char*)addr);
  if (flag) {
    *(int *)addr = 0;
  }
  printf("buf[%zu] = %c\n", idx, c);
}

// The wider access dominates the smaller, so optimization is allowed
void __attribute__((noinline)) wider_above(size_t idx, char *buf, bool flag) {
  char *addr = buf + idx;
  int c = *(int *)addr;
  if (flag) {
    *((char*)addr) = 0;
  }
  printf("buf[%zu] = %c\n", idx, c);
}

// The wider postdominates the smaller, so optimization is allowed
void __attribute__((noinline)) wider_unconditional(size_t idx, char *buf) {
  char *addr = buf + idx;
  char c = *((char*)addr);
  printf("buf[%zu] = %c\n", idx, c);
  *(int *)addr = 0;
}

void __attribute__((noinline)) outer(int idx, int which) {
  char buf[20];
  for (int i = 0; i < 20; i++) {
    buf[i] = '-' + i;
  }
  if (which == 0 || which == 1) {
    wider_conditional(idx, buf, true);
  }
  if (which == 0 || which == 2) {
    wider_above(idx, buf, true);
  }
  if (which == 0 || which == 3) {
    wider_unconditional(idx, buf);
  }
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  int idx = atoi(argv[1]);
  outer(idx, argc > 2 ? atoi(argv[2]) : 0);

  return 0;
}
