//===----------------------------------------------------------------------===//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clangxx_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// ERR: Unreclaimed IDs at exit: 0
// CHECK: 5

// Thanks chatGPT!
//===----------------------------------------------------------------------===//

#include <cstdio>
#include <cstdlib>

// ─────────────────────────────────────────────────────────────────────────────
// Helper that *may* throw   =>  caller must be an `invoke`
// ─────────────────────────────────────────────────────────────────────────────
__attribute__((noinline)) int *alloc(int n) {
  if (n < 0) {
    throw -1;
  }

  int *buf = (int*)malloc(n * sizeof(int));
  for (int i = 0; i < n; i++) {
    buf[i] = i;
  }
  return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test function – will contain the `invoke`
// ─────────────────────────────────────────────────────────────────────────────
__attribute__((noinline)) int test_invoke() {
  int *buf;
  try {
    buf = alloc(30);
  } catch (int) {
    buf = (int *)malloc(10);
    buf[5] = 10;
  }

  int result = buf[5];

  std::free(buf);
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
  std::printf("%d\n", test_invoke());
  return 0;
}
