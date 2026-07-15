//===----------------------------------------------------------------------===//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clangxx_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// ERR: Unreclaimed IDs at exit: 0
// CHECK: -1

// Thanks chatGPT!
//===----------------------------------------------------------------------===//

#include <cstdio>
#include <cstdlib>

__attribute__((noinline)) void populate(int* buf, int n) {
  for (int i = 0; i < n; i++) {
    buf[i] = i;
  }
}


// ─────────────────────────────────────────────────────────────────────────────
// Helper that allocates an ID and throws (with n > 30)
// ─────────────────────────────────────────────────────────────────────────────
__attribute__((noinline)) int may_throw(int n) {
  int buf[30];
  if (n > 30) {
    throw -1;
  }
  populate(buf, n);

  return buf[n] + buf[n/2];
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test function – will contain the `invoke`
// ─────────────────────────────────────────────────────────────────────────────
__attribute__((noinline)) int test_invoke() {
  int result = 0;
  try {
    // this will throw, so we will leak an ID unless we handle the
    // throw case above
    result = may_throw(50);
  } catch (int) {
    return -1;
  }

  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
  std::printf("%d\n", test_invoke());
  return 0;
}
