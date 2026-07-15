// RUN: rm -rf %t && mkdir -p %t && cd %t

// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 19 2> out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: jmp catch
// CHECK-NOT: got to the end

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>


void __attribute__((noinline)) f0(int idx, char *buf) {
  char c = buf[idx];
  printf("buf[%d] = %c\n", idx, c);
}

void __attribute__((noinline)) f1(int idx) {
  char buf[20];
  for (int i = 0; i < 20; i++) {
    buf[i] = 'A' + (i % 26);
  }

  f0(idx, buf);
}

void __attribute__((noinline)) f2(int idx){
  jmp_buf env;

  int j = setjmp(env);
  printf("Setjmp_value %d\n", j);

  if(j > 0) {
    fprintf(stderr, "jmp catch\n");
    exit(0);
  }

  f1(idx);

  longjmp(env, 2);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <index>\n", argv[0]);
    return 1;
  }

  int idx = atoi(argv[1]);
  f2(idx);

  fprintf(stderr, "got to the end. Aborting!\n");
  abort();
}