//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Runtime CHECK failures must fail the process, not just print a diagnostic.
// RUN: %ptsan_musl_strict -o %t %s
// RUN: not %run %t 2>&1 | FileCheck %s
// CHECK: CHECK failed:
// CHECK-SAME: ptsan_init_is_running

extern void __ptsan_finish_instrumented_libc_init(void);

int main(void) {
  // Startup has already finished. Calling its completion hook again triggers
  // a runtime CHECK, rather than a PTSan memory-access violation.
  __ptsan_finish_instrumented_libc_init();
  return 0;
}
