// XFAIL: *

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: not --crash ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// CHECK: User code caught bad malloc
// CHECK: Found bad


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

char *__attribute__((noinline)) func_bad(size_t sz, size_t idx) {
  char *buf = malloc(sz);
  buf[idx] = '\0';
  return buf;
}

char *__attribute__((noinline)) func_good(size_t sz, size_t idx) {
  char *buf = malloc(sz);
  if (buf == 0) {
    fprintf(stderr, "User code caught bad malloc\n");
    return buf;
  }

  buf[idx] = '\0';
  return buf;
}


int main(int argc, char *argv[]) {
  char *b1 = func_good(0x0fffffffffffffff, 0x77ff00000000);
  char *b2 =  func_bad(0x0fffffffffffffff, 0x77ff00000000);

  if(b1) { free(b1); }
  if(b2) { free(b2); }
  return 0;
}