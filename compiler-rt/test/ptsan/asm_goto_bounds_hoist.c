//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_raw -O1 -fsanitize=ptsan -fsanitize-ptsan-strict-mode -c %s -o %t.o

// Reproduces the failure in PHP's asm-goto arithmetic helpers. Bounds for p
// must be loaded after the asm, but callbr has no insertion point after it in
// its own block. This used to assert in HoistTarget::getIP().
void repro(long **slot) {
  long *p = *slot;
  asm goto("" : : : "memory" : done);
  *p = 0;
done:
  return;
}
