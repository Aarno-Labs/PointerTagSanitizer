//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt
// RUN: cat err.txt | FileCheck %s

// The sendfile / sendfile64 interceptors bracket the REAL call with
// ptsan_in_libc++/--. A missing decrement (regression 266e938d083b) leaves the
// counter non-zero, which makes tag_with_next_id take its "inside libc"
// early-return and stop tagging: a later allocation comes back untagged (id 0),
// and an OOB through an id-0 pointer is silently missed (ptsan_bounds[0] spans
// the whole address space). After exercising both interceptors, this test
// confirms the counter is balanced by checking that a fresh allocation is still
// tagged -- i.e. the OOB read below is still reported.

// CHECK: in_libc after sendfile: 0
// CHECK: Found bad

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <threads.h>

extern thread_local int ptsan_in_libc;

// sendfile64 isn't declared without _LARGEFILE64_SOURCE; declare it directly to
// match the distinct sendfile64 interceptor ptsan registers.
extern ssize_t sendfile64(int out_fd, int in_fd, off_t *offset, size_t count);

int __attribute__((noinline)) do_access(char *buf, int idx) {
  return buf[idx];
}

int main(void) {
  // Back the source with an in-memory file so the test never touches disk.
  // (fmemopen -- as in fread.c -- can't be used here: its stream has no
  // underlying fd, and sendfile needs a real mmap-able source descriptor.
  // memfd_create is the in-memory equivalent that yields one.)
  int in = memfd_create("payload", 0);
  if (in < 0) { perror("memfd_create"); return 1; }
  if (write(in, "hello sendfile", 14) != 14) { perror("write"); return 1; }

  int p[2];
  if (pipe(p) != 0) { perror("pipe"); return 1; }

  off_t off = 0;
  sendfile(p[1], in, &off, 4);
  off = 0;
  sendfile64(p[1], in, &off, 4);
  close(in);

  fprintf(stderr, "in_libc after sendfile: %d\n", ptsan_in_libc);

  // Tagging must still be live: this allocation has to come back tagged, so the
  // OOB read is reported. Pre-fix the leak made it id 0 and silently passed.
  char *buf = (char *)malloc(20);
  for (int i = 0; i < 20; i++) buf[i] = 'A';
  fprintf(stderr, "oob = %c\n", do_access(buf, 23));

  fprintf(stderr, "reached end without a report!\n");
  return 0;
}
