// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o strong %s
// RUN: ./strong > strong.out 2> strong.err
// RUN: FileCheck %s --check-prefix=STRONG < strong.out
// RUN: %clang_ptsan -g -O2 -mllvm -ptsan-stack-temporal-safety=0 -o weak %s
// RUN: ./weak > weak.out 2> weak.err
// RUN: FileCheck %s --check-prefix=WEAK < weak.out

// STRONG: delta=1 pending=1
// WEAK: delta=0 pending=0

#include <stdint.h>
#include <stdio.h>

typedef uint16_t u16;

extern u16 ptsan_next_stack_id;
extern __thread u16 ptsan_pending_stack_ids;

static volatile char Sink;

__attribute__((noinline))
static void touch(char *ptr) {
  ptr[0] = 'x';
  Sink = ptr[0];
}

__attribute__((noinline))
static void one_stack_id(void) {
  char buf[17];
  touch(buf);
}

int main(void) {
  ptsan_pending_stack_ids = 0;
  u16 before = ptsan_next_stack_id;

  one_stack_id();

  u16 after = ptsan_next_stack_id;
  printf("delta=%u pending=%u\n",
         (unsigned)((u16)(after - before)),
         (unsigned)ptsan_pending_stack_ids);
  return Sink == 'x' ? 0 : 1;
}
