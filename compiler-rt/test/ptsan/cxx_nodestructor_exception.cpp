// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clangxx_ptsan -g -O2 -mllvm -ptsan-stack-temporal-safety=0 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s
// RUN: %clangxx_ptsan -g -O2 -mllvm -ptsan-stack-temporal-safety=0 \
// RUN:   -mllvm -ptsan-no-stack-id-fast-path -o slow %s
// RUN: ./slow 2> slow.err
// RUN: FileCheck %s < slow.err
// RUN: %clangxx_raw -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=BEFORE
// RUN: %clangxx_ptsan -O2 -S -emit-llvm -mllvm -ptsan-stop-after=discovery %s -o - | FileCheck %s --check-prefix=DISCOVERY
// RUN: %clangxx_ptsan -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=AFTER

// A plain call to a throwing helper must still run PTSan's frame cleanup.
// Disable deferred stack-ID reuse so the cursor must return to its old value.
// Exit statistics alone miss leaks from the inline fast stack-ID path.
// CHECK: fast stack ID delta: 0
// CHECK: Unreclaimed IDs at exit: 0
// BEFORE-LABEL: define {{.*}}void @_Z1gv()
// BEFORE: call void @_Z6throwsv()
// BEFORE: ret void
// DISCOVERY-LABEL: define {{.*}}void @_Z1gv()
// DISCOVERY: invoke void @_Z6throwsv()
// DISCOVERY: call void (...) @__ptsan_function_exit()
// DISCOVERY-NEXT: ret void
// DISCOVERY: landingpad
// DISCOVERY: call void (...) @__ptsan_function_exit()
// DISCOVERY-NEXT: resume
// AFTER-LABEL: define {{.*}}void @_Z1gv()
// AFTER: invoke void @_Z6throwsv()
// AFTER: landingpad
// AFTER: resume
#include <stdint.h>
#include <stdio.h>

extern "C" {
extern __thread uint16_t ptsan_next_stack_id;
}

// Exercise the throw, but keep the normal return paths in the generated IR.
static volatile bool should_throw = true;

__attribute__((noinline)) void use_buf(const volatile char *p) {
  printf("buf[0]=%d\n", (int)p[0]);
}

__attribute__((noinline)) void throws() {
  if (should_throw) {
    throw 42;
  }
}

__attribute__((noinline)) void g() {
  char buf[32];
  buf[0] = 17;
  buf[31] = 99;
  use_buf(buf);
  // No local catch or destructor: Clang emits a plain call, not an invoke.
  throws();
}


int main() {
  uint16_t before = ptsan_next_stack_id;
  try {
    g();
    printf("should not get here\n");
  } catch (int x) {
    printf("caught %d\n", x);
  }

  uint16_t after = ptsan_next_stack_id;
  fprintf(stderr, "fast stack ID delta: %d\n", (int)after - (int)before);
  printf("continued normally\n");
  return 0;
}
