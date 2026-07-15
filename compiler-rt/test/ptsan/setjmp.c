// RUN: rm -rf %t && mkdir -p %t && cd %t

// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 23 2> err.txt
// RUN: cat err.txt | FileCheck %s


// First one, from f0
// CHECK: Found bad

// CHECK: before jmp in libc: 0

// CHECK: jmp catch

// CHECK: after-jmp in libc: 0

// Second one, from post-longjmp
// CHECK: Found bad

// CHECK-NOT: got to the end

#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <threads.h>

extern thread_local int ptsan_in_libc;

jmp_buf env;

int __attribute__((noinline)) do_access(char *buf, int idx) {
  return buf[idx];
}

void __attribute__((noinline)) init(char *buf) {
  for (int i = 0; i < 20; i++) {
    buf[i] = 'A' + (i % 26);
  }
}

void __attribute__((noinline)) f0(char *buf, int idx) {
  char c = do_access(buf, idx);
  printf("buf[%d] = %c\n", idx, c);
  fprintf(stderr, "before jmp in libc: %d\n", ptsan_in_libc);
  longjmp(env, 2);
}

void __attribute__((noinline)) f1(char *buf, int idx) {
  init(buf);
  f0(buf, idx);
}

void __attribute__((noinline)) f2(int idx){
  char buf[20];
  int j = setjmp(env);
  printf("Setjmp_value %d\n", j);

  if(j > 0) {
    fprintf(stderr, "jmp catch\n");
    char *shouldBeTagged = (char *)malloc(20);
    init(shouldBeTagged);
    fprintf(stderr, "after-jmp in libc: %d\n", ptsan_in_libc);

    printf("after-jmp index access is %c\n", do_access(shouldBeTagged, idx));
    exit(0);
  }

  f1(buf, idx);
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