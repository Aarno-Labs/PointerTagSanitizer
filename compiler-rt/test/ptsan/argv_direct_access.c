// Reproduces the cfrac shape where a direct access through argv previously used
// the untagged argv value as the check min, while argv bounds were id-tagged.

// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2>&1 | FileCheck %s

// CHECK-NOT: Found bad {{.*}} access

#include <stdio.h>

int main(int argc, char **argv) {
  if (argc < 1) {
    return 1;
  }

  puts(argv[0]);
  return 0;
}
