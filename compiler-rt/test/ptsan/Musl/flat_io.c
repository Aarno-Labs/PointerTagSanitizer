//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise flat buffer syscalls through musl's public APIs.
//
// Public pwrite() is deliberately not used here: musl first issues pwritev2
// with a nested iovec.  That syscall needs a separate nested-memory model.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t read-good
// RUN: %run %t pread-good
// RUN: %run %t getrandom-good
// RUN: %run %t stdio-read-good
// RUN: not %run %t read-bad 2> %t.read
// RUN: FileCheck %s < %t.read
// RUN: not %run %t pread-bad 2> %t.pread
// RUN: FileCheck %s < %t.pread
// RUN: not %run %t getrandom-bad 2> %t.getrandom
// RUN: FileCheck %s < %t.getrandom
//
// CHECK: Found bad

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <unistd.h>

static size_t requested_size(const char *mode) {
  return strstr(mode, "-bad") ? 8 : 4;
}

static char *make_buffer(void) {
  char *buffer = malloc(4);
  if (buffer)
    memcpy(buffer, "data", 4);
  return buffer;
}

static int make_file(void) { return memfd_create("ptsan-flat-io", 0); }

static int test_read(const char *mode) {
  int fds[2];
  if (pipe(fds))
    return 1;
  if (write(fds[1], "datadata", 8) != 8)
    return 2;

  char *buffer = make_buffer();
  if (!buffer)
    return 3;
  ssize_t result = read(fds[0], buffer, requested_size(mode));
  free(buffer);
  return result == 4 ? 0 : 4;
}

static int test_pread(const char *mode) {
  int fd = make_file();
  if (fd < 0)
    return 1;
  if (write(fd, "datadata", 8) != 8)
    return 2;

  char *buffer = make_buffer();
  if (!buffer)
    return 3;
  ssize_t result = pread(fd, buffer, requested_size(mode), 0);
  free(buffer);
  close(fd);
  return result == 4 ? 0 : 4;
}

static int test_getrandom(const char *mode) {
  char *buffer = make_buffer();
  if (!buffer)
    return 1;
  ssize_t result = getrandom(buffer, requested_size(mode), 0);
  free(buffer);
  return result == 4 ? 0 : 2;
}

static int test_stdio_read(void) {
  int fds[2];
  if (pipe(fds))
    return 1;
  if (write(fds[1], "x", 1) != 1)
    return 2;
  FILE *stream = fdopen(fds[0], "r");
  if (!stream)
    return 3;
  int result = fgetc(stream);
  fclose(stream);
  close(fds[1]);
  return result == 'x' ? 0 : 4;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 1;
  if (!strncmp(argv[1], "read-", 5))
    return test_read(argv[1]);
  if (!strncmp(argv[1], "pread-", 6))
    return test_pread(argv[1]);
  if (!strncmp(argv[1], "getrandom-", 10))
    return test_getrandom(argv[1]);
  if (!strcmp(argv[1], "stdio-read-good"))
    return test_stdio_read();
  return 2;
}
