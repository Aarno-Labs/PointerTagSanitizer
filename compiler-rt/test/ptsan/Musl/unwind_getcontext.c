//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s %S/Inputs/unwind_getcontext.S
// RUN: %run %t

// Fully instrumented libunwind passes a stack-allocated register context to
// the assembly __unw_getcontext implementation. The assembly cannot
// dereference PTSan-tagged pointers, so this boundary must strip the context
// pointer before entering the real implementation. Restoring the captured
// registers crosses the same kind of boundary in the opposite direction.

typedef struct {
  unsigned long registers[17];
} UnwindContext;

extern int __unw_getcontext(UnwindContext *context);
extern int unw_getcontext(UnwindContext *context);
extern void __libunwind_Registers_x86_64_jumpto(UnwindContext *context);

int main(void) {
  UnwindContext internal_context;
  UnwindContext public_context;
  if (__unw_getcontext(&internal_context) != 0) {
    return 1;
  }
  if (unw_getcontext(&public_context) != 0) {
    return 1;
  }
  __libunwind_Registers_x86_64_jumpto(&public_context);
  return 0;
}
