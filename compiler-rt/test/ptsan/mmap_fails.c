// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: not --crash ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// CHECK: User code caught bad mmap
// CHECK: Found bad

#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef MAP_ANONYMOUS
#  define MAP_ANONYMOUS MAP_ANON
#endif

__attribute__((noinline))
char *func_bad_mmap(size_t sz, size_t idx) {
  char *p = mmap(0, sz, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  // No check — deliberately bad:
  p[idx] = '\0';
  return p;
}

__attribute__((noinline))
char *func_good_mmap(size_t sz, size_t idx) {
  char *p = mmap(0, sz, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    fprintf(stderr, "User code caught bad mmap\n");
    return 0;
  }
  p[idx] = '\0';
  return p;
}

int main(void) {
  size_t huge = 0x0fffffffffffffffULL;

  char *b1 = func_good_mmap(huge, 30);
  char *b2 = func_bad_mmap(huge, 30);

  // If either improbably succeeded, clean up.
  if (b1) { munmap(b1, huge); }
  if (b2 && b2 != (char *)MAP_FAILED) { munmap(b2, huge); }

  return 0;
}
