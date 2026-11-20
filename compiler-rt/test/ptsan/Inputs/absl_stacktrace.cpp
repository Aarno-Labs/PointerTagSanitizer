//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Standalone Fleetbench reproducer: link against the matching instrumented
// Abseil libraries. Kept in Inputs because check-ptsan does not build Abseil.
// No Cordz sampling, Protobuf, threads, or benchmark loop is needed.
// Fully checked strict mode reports an ID-0 read in UnwindImpl when it reads
// the saved return address. Compiling only Abseil's stacktrace.cc with
// -ptsan-nochecks permits this with a marked representation while retaining
// conversions for the tagged output buffer. It is insufficient for markerless
// strict full32: raw frame addresses are still decoded, causing a crash.
// In strict indicator mode, add -ptsan-may-decode-raw only to stacktrace.cc.
// Its output buffer remains encoded. Non-strict mode also returns frames.

#include "absl/debugging/stacktrace.h"
#include <assert.h>
#include <stdio.h>

__attribute__((noinline)) static void capture_stack() {
  void *frames[16] = {};
  int count = absl::GetStackTrace(frames, 16, 0);
  assert(count > 0 && count <= 16);
  assert(frames[0] != nullptr);
  puts("stack trace captured");
}

int main() {
  capture_stack();
  return 0;
}
