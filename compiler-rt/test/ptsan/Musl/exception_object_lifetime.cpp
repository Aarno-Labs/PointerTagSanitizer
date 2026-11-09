//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsanxx_musl_strict %s -o %t
// RUN: not %run %t 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %run %t retain 2>&1 | FileCheck %s --check-prefix=GOOD
// RUN: %ptsanxx_musl_id32_strict %s -o %t.id32
// RUN: not %run %t.id32 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: %run %t.id32 retain 2>&1 | FileCheck %s --check-prefix=GOOD
// BAD: Found bad POINT access
// GOOD-NOT: Found bad
// GOOD: exception value: 42

// Leaving the catch destroys the exception unless an exception_ptr retains it.
// Bounds must be loaded after __cxa_end_catch, not cached at __cxa_begin_catch.
#include <cstdio>
#include <exception>
struct Payload {
  volatile int value;
  ~Payload() {}
};
__attribute__((noinline)) static void throw_payload() { throw Payload{42}; }
int main(int argc, char **) {
  Payload *saved = nullptr;
  std::exception_ptr retained;
  try {
    throw_payload();
  } catch (Payload &payload) {
    saved = &payload;
    if (argc > 1)
      retained = std::current_exception();
  }
  int value = saved->value;
  std::printf("exception value: %d\n", value);
  return 0;
}
