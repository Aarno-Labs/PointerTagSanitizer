// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe.ll %s -S -emit-llvm
// RUN: cat %exe.ll | FileCheck %s

// CHECK-LABEL:  @_Z1aPcb
// CHECK: call void @ptsan_complain(i32 0,
// CHECK-NOT: call void @ptsan_complain(i32 1,

#include <cstring>
#include <iostream>
#include <stdexcept>

__attribute__((noinline)) void b(bool shouldThrow) {
  if (shouldThrow)
    throw std::runtime_error("function b: something went wrong");

  std::cout << "function b: all good\n";
}

__attribute__((noinline)) void a(char *buf, bool callBWithError) {
  try {
    b(callBWithError);
    // If we insert bounds load naievely, it will be here which does not
    // dominate the actual use
    std::cout << "Great success\n";
  } catch (const std::exception &ex) {
    std::cerr << "function a caught exception: " << ex.what() << '\n';
  }

  const char *text = callBWithError ? "error path" : "success path";
  std::strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
}

//--------------------------------------------------------------
int main() {
  char buf[32];
  a(buf, false); // b succeeds
  std::cout << "On success:: \"" << buf << "\"\n\n";
  a(buf, true); // b throws
  std::cout << "On throw: \"" << buf << "\"\n\n";
}
