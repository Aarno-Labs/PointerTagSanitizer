// Forces the slow-path stack ID cases and checks that all stack ID helpers
// write id-tagged bases, so in-bounds accesses do not get reported as bad.

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -mllvm -ptsan-instrument-globals=0 -O2 -o %exe %s
// RUN: ./%exe 1 2> err.txt
// RUN: FileCheck %s --check-prefix=GOOD < err.txt
// RUN: ./%exe 2 2> err.txt
// RUN: FileCheck %s --check-prefix=GOOD < err.txt
// RUN: ./%exe 3 2> err.txt
// RUN: FileCheck %s --check-prefix=GOOD < err.txt
// RUN: ./%exe 4 2> err.txt
// RUN: FileCheck %s --check-prefix=GOOD < err.txt
// RUN: ./%exe n 2> err.txt
// RUN: FileCheck %s --check-prefix=GOOD < err.txt

// GOOD-NOT: Found bad {{.*}} access

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint16_t u16;

extern u16 ptsan_next_stack_id;
extern u16 ptsan_stack_id_limit;

__attribute__((noinline))
static void init(char *ptr) {
  ptr[0] = 'x';
  ptr[1] = '\0';
}

__attribute__((noinline))
static void one(void) {
  char a[23];
  init(a);
  printf("%s\n", a);
}

__attribute__((noinline))
static void two(void) {
  char a[23];
  char b[43];
  init(a);
  init(b);
  printf("%s%s\n", a, b);
}

__attribute__((noinline))
static void three(void) {
  char a[23];
  char b[43];
  char c[453];
  init(a);
  init(b);
  init(c);
  printf("%s%s%s\n", a, b, c);
}

__attribute__((noinline))
static void four(void) {
  char a[23];
  char b[43];
  char c[453];
  char d[64];
  init(a);
  init(b);
  init(c);
  init(d);
  printf("%s%s%s%s\n", a, b, c, d);
}

__attribute__((noinline))
static void n(void) {
  char a[23];
  char b[43];
  char c[453];
  char d[64];
  char e[67];
  init(a);
  init(b);
  init(c);
  init(d);
  init(e);
  printf("%s%s%s%s%s\n", a, b, c, d, e);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <1|2|3|4|n>\n", argv[0]);
    return 1;
  }

  ptsan_next_stack_id = ptsan_stack_id_limit;

  if (strcmp(argv[1], "1") == 0) {
    one();
  } else if (strcmp(argv[1], "2") == 0) {
    two();
  } else if (strcmp(argv[1], "3") == 0) {
    three();
  } else if (strcmp(argv[1], "4") == 0) {
    four();
  } else if (strcmp(argv[1], "n") == 0) {
    n();
  } else {
    fprintf(stderr, "unknown case: %s\n", argv[1]);
    return 1;
  }

  return 0;
}
