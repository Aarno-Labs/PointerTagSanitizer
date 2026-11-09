// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe | FileCheck %s

// CHECK: ---------RELPATH DOES ALLOC-----------
// CHECK: P id: 0x{{[0-9a-f]+}}
// CHECK: Res id: 0x{{[0-9a-f]+}}
// CHECK: -----------EXTERNAL ALLOC-------------
// CHECK: P id: 0x{{[0-9a-f]+}}
// CHECK: Res id: 0x[[OUT:[0-9a-f]+]]
// CHECK: Out id: 0x[[OUT]]


#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include "Inputs/ptsan_test_interface.h"

void relpath_does_alloc() {
  char *p = malloc(2);
  p[0] = '.';
  p[1] = 0;
  char *res = realpath(p, NULL);
  assert(res);
  assert(get_id(p) != 0 && get_id(p) != ptsan_default_id());
  assert(get_id(res) != 0 && get_id(res) != ptsan_default_id());
  assert(get_id(p) != get_id(res));
  printf("P id: 0x%x\n", get_id(p));
  printf("Res id: 0x%x\n", get_id(res));
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
  assert(res == out);
  assert(get_id(p) != 0 && get_id(p) != ptsan_default_id());
  assert(get_id(out) != 0 && get_id(out) != ptsan_default_id());
  assert(get_id(p) != get_id(out));
  printf("P id: 0x%x\n", get_id(p));
  printf("Res id: 0x%x\n", get_id(res));
  printf("Out id: 0x%x\n", get_id(out));
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
