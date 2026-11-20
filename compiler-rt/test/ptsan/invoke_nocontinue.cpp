//===----------------------------------------------------------------------===//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clangxx_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s

// ERR: Unreclaimed IDs at exit: 0
// CHECK: 34

// Thanks chatGPT!
//===----------------------------------------------------------------------===//

#include <cstdio>
#include <cstdlib>

// ─────────────────────────────────────────────────────────────────────────────
// Helper that *may* throw   =>  caller must be an `invoke`
// ─────────────────────────────────────────────────────────────────────────────
__attribute__((noinline)) int may_throw(int *buf, int idx) {
  if (idx < 0) {
    std::free(buf);
    throw idx;
  }

  return buf[idx];
}

// ─────────────────────────────────────────────────────────────────────────────
//  Test function – will contain the `invoke`
// ─────────────────────────────────────────────────────────────────────────────
__attribute__((noinline)) int test_invoke(int *buf) {
  int result = 0;
  try {
    result = may_throw(buf, 3); // lowered to `invoke`
  } catch (int) {
    return -1;
  }

  result += buf[5];

  std::free(buf);
  return result;
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
  int *buf = static_cast<int *>(std::malloc(8 * sizeof(int)));
  for (int i = 0; i < 8; ++i)
    buf[i] = i * i;

  std::printf("%d\n", test_invoke(buf));
  return 0;
}
