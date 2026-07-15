// dlsym_rtld_next_test.c

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s


// CHECK: dlsym error: ./a.out: undefined symbol: ompt_start_tool
// CHECK: No next ompt_start_tool found

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <threads.h>

extern thread_local int ptsan_in_libc;

__attribute__((noinline))
static void *do_lookup(void) {
  dlerror();

  void *p = dlsym(RTLD_NEXT, "ompt_start_tool");

  const char *e = dlerror();
  if (e) {
    printf("dlsym error: %s\n", e);
  } else {
    printf("dlsym returned %p\n", p);
  }
  return p;
}

__attribute__((noinline))
__attribute__((weak))
void *ompt_start_tool(unsigned int omp_version, const char *runtime_version) {
  (void)omp_version;
  (void)runtime_version;

  // This is the exact pattern from libomp.
  void *next = do_lookup();
  if (next) {
    printf("Found next ompt_start_tool at %p\n", next);
  } else {
    printf("No next ompt_start_tool found\n");
  }
  return NULL;
}

__attribute__((noinline))
int main(void) {
  (void)ompt_start_tool(0, "test");
  return 0;
}
