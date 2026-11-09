//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// REQUIRES: x86_64-target-arch
// XFAIL: *
// RUN: %clang_raw -fsanitize=ptsan -O0 -mllvm -ptsan-abort-on-error %s -o %t
// RUN: not %run %t single-checkpoint 2>&1 | FileCheck %s
// RUN: not %run %t 2>&1 | FileCheck %s
// CHECK: Found bad POINT access

// Two live environments saved by one setjmp call site need distinct PTSan
// checkpoints. At -O0, the later iteration overwrites the earlier cursor's
// stack spill. Jumping to envs[0] then leaves the first allocation's bounds
// live and misses the stale read. Skipping the second save detects it correctly.
// Optimized code can happen to work when the cursor stays in a saved register.
#include <alloca.h>
#include <setjmp.h>
#include <stdio.h>

static jmp_buf envs[2];
// State changed after setjmp must not depend on nonvolatile automatic locals.
static unsigned iteration;
static volatile unsigned length = 37;
static char *escaped;
static volatile char sink;

int main(int argc, char **argv) {
  for (iteration = 0; iteration < 2; ++iteration) {
    // With an argument, save only envs[0]: this is the passing control case.
    if (iteration == 0 || argc == 1) {
      if (setjmp(envs[iteration])) {
        // The first allocation was made after envs[0] saved its stack pointer.
        // Jumping back there discards its storage, so this read must fail.
        sink = escaped[0];
        fputs("stale read was not detected\n", stderr);
        return 0;
      }
    }
    // Keep the first allocation live across the second setjmp. Returning from
    // an allocating helper here would retire its IDs before the second save.
    char *buffer = alloca(length);
    buffer[0] = 'a';
    if (iteration == 0) {
      escaped = buffer;
    }
  }
  longjmp(envs[0], 1);
}
