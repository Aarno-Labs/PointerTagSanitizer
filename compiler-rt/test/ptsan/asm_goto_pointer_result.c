//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// RUN: %clang_ptsan -O1 -fsanitize-ptsan-strict-mode -mllvm -ptsan-abort-on-error=1 %s -o %t
// RUN: %run %t
// RUN: %clang_ptsan -O1 -mllvm -ptsan-no-opts -mllvm -ptsan-abort-on-error=1 %s -o %t.noopts
// RUN: %run %t.noopts

// Pass the tagged pointer bits through an integer register, so the inline asm
// itself does not dereference a tagged address. Both asm-goto successors must
// be able to extract the returned pointer's ID and check/strip its address.
__attribute__((noinline)) int read_result(unsigned long bits, int branch) {
  int *p;
  asm goto("mov %[bits], %[p]; test %[branch], %[branch]; jnz %l[taken]"
           : [p] "=&r"(p)
           : [bits] "r"(bits), [branch] "r"(branch)
           : "cc"
           : taken);
  return *p;
taken:
  return *p + 1;
}

int main(void) {
  int x = 41;
  return read_result((unsigned long)&x, 0) != 41 ||
         read_result((unsigned long)&x, 1) != 42;
}
