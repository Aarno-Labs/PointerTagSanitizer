// Reproduces the dbms shape where strstr returns NULL, then the generic
// interceptor return-ID restoration turns it into a non-null default-ID pointer.

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe | FileCheck %s

// CHECK: OK: strstr returned null
// CHECK-NOT: BUG: strstr returned non-null

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *haystack = strdup("Tarz");
  if (!haystack) {
    return 1;
  }

  char *needle = (char *)((uintptr_t)"9V" | 0xffff000000000000ULL);
  char *result = strstr(haystack, needle);

  if (result != NULL) {
    puts("BUG: strstr returned non-null");
    return 0;
  }

  puts("OK: strstr returned null");
  return 0;
}
