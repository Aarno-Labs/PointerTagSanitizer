// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -fno-discard-value-names -o normal %s
// RUN: ./normal 2> normal.err
// RUN: FileCheck %s --check-prefix=NORMAL --allow-empty < normal.err
// RUN: %clang_raw -fsanitize=ptsan -g -O2 -fno-discard-value-names -mllvm -ptsan-runtime-validate -mllvm -ptsan-print-stats -o validate %s
// RUN: ./validate 2> validate.err
// RUN: FileCheck %s --check-prefix=VALIDATE < validate.err

// LAM runtime variants are exercised by runtime_validate_stats_lam.c.

// NORMAL-NOT: Pointer Tag Sanitizer Exit Stats:
// NORMAL-NOT: Unreclaimed IDs at exit:
// VALIDATE: Pointer Tag Sanitizer Exit Stats:
// VALIDATE: Unreclaimed IDs at exit: 0

#include <stdlib.h>

int main(void) {
  char *buf = (char *)malloc(16);
  if (!buf)
    return 1;
  buf[0] = 'x';
  free(buf);
  return 0;
}
