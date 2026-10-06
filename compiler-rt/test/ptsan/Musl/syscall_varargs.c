//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
//
// Public syscall() unpacks six arguments even when fewer were supplied.
// Disabling checks on that unpacking must not disable syscall buffer checks.
// CHECK: Found bad libc access
// CHECK: Alloc size  0x4
// CHECK: Access size 0x5

#define _GNU_SOURCE
#include <sys/syscall.h>
#include <unistd.h>

__attribute__((noinline)) static long send_buffer(const char *buffer,
                                                 size_t size) {
  return syscall(SYS_write, STDOUT_FILENO, buffer, size);
}

int main(int argc, char **argv) {
  if (syscall(SYS_getpid) <= 0) {
    return 1;
  }
  char buffer[4] = {'t', 'e', 's', 't'};
  size_t size = argc == 1 ? sizeof(buffer) : sizeof(buffer) + 1;
  return send_buffer(buffer, size) == (long)size ? 0 : 2;
}
