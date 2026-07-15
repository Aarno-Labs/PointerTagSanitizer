// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe | FileCheck %s

// CHECK: ---------RELPATH DOES ALLOC-----------
// CHECK: P id: 0x1801
// CHECK: Res id: 0x1802
// CHECK: -----------EXTERNAL ALLOC-------------
// CHECK: P id: 0x1803
// CHECK: Res id: 0x1804
// CHECK: Out id: 0x1804


#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

void relpath_does_alloc() {
  char *p = malloc(2);
  p[0] = '.';
  p[1] = 0;
  char *res = realpath(p, NULL);
  printf("P id: 0x%lx\n", (uint64_t)p >> 48);
  printf("Res id: 0x%lx\n", (uint64_t)res >> 48);
  //printf("Got path %s\n", res);
  // If realpath fails, 'res' is NULL; otherwise it's a malloc'ed buffer.
  if (res) {
    // Free the allocated buffer to avoid a leak.
    free(res);
  }
}

void external_alloc() {
  // Ask realpath to allocate space for the result.
  char *p = malloc(2);
  p[0] = '.';
  p[1] = 0;
  char *out = malloc(200);
  char *res = realpath(p, out);
  printf("P id: 0x%lx\n", (uint64_t)p >> 48);
  printf("Res id: 0x%lx\n", (uint64_t)res >> 48);
  printf("Out id: 0x%lx\n", (uint64_t)out >> 48);
  //printf("Got path %s\n", res);
  // If realpath fails, 'res' is NULL; otherwise it's a malloc'ed buffer.
  if (res) {
    // Free the allocated buffer to avoid a leak.
    free(res);
  }
}

int main(void) {
  printf("---------RELPATH DOES ALLOC-----------\n");
  relpath_does_alloc();
  printf("-----------EXTERNAL ALLOC-------------\n");
  external_alloc();
  return 0;
}