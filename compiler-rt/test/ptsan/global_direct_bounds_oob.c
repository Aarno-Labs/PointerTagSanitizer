// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 19 2> good.txt
// RUN: cat good.txt | FileCheck %s --check-prefix=GOOD
// RUN: ./%exe 20 2> bad.txt
// RUN: cat bad.txt | FileCheck %s --check-prefix=BAD
// RUN: %clang_raw -fsanitize=ptsan -O2 -g -fno-discard-value-names -S -emit-llvm -o - %s | FileCheck %s --check-prefix=IR

// BAD: Found bad POINT access
// GOOD-NOT: Found bad POINT access

// IR-LABEL: define {{.*}}@read_direct_static_global_bounds(
// IR: [[TAGGED:%[A-Za-z0-9_.]+]] = load ptr, ptr @buf.ptsan.ptr
// IR-NOT: @ptsan_bounds
// IR: ptrtoint ptr [[TAGGED]] to i64
// IR-NOT: @ptsan_bounds
// IR: call void @ptsan_complain
// IR-NOT: @ptsan_bounds
// IR: ret

#include <stdio.h>
#include <stdlib.h>

char buf[20];

__attribute__((noinline))
char read_direct_static_global_bounds(int idx) {
  return buf[idx];
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  for (int i = 0; i < 20; i++) {
    buf[i] = 'A' + i;
  }

  int idx = atoi(argv[1]);
  printf("buf[%d] = %c\n", idx, read_direct_static_global_bounds(idx));
  return 0;
}
