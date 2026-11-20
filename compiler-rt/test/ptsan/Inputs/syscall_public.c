//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise the shared syscall dispatcher with zero through six arguments,
// nested tagged buffers, raw success counts, errno, and mapping lifetimes.
#define _GNU_SOURCE
#include "ptsan_test_interface.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

int main(int argc, char **argv) {
  if (argc > 1) {
    char *buffer = malloc(4);
    assert(buffer);
    syscall(SYS_write, -1, buffer, (size_t)5);
    return 1;
  }

  errno = EDOM;
  assert(syscall(SYS_getpid) == getpid());
  assert(errno == EDOM);
  assert(syscall(SYS_close, -1) == -1);
  assert(errno == EBADF);
  assert(syscall(-1L) == -1);
  assert(errno == ENOSYS);

  int pipes[2];
  assert(syscall(SYS_pipe2, pipes, 0) == 0);
  char message[] = "shared syscall";
  struct iovec vectors[] = {{message, 6}, {message + 6, sizeof(message) - 6}};
  assert(syscall(SYS_writev, pipes[1], vectors, 2) == sizeof(message));
  char output[sizeof(message)];
  assert(syscall(SYS_read, pipes[0], output, sizeof(output)) == sizeof(output));
  assert(memcmp(message, output, sizeof(message)) == 0);
  assert(syscall(SYS_close, pipes[0]) == 0);
  assert(syscall(SYS_close, pipes[1]) == 0);

  char path[] = "/dev/null";
  long fd = syscall(SYS_openat, AT_FDCWD, path, O_RDONLY, 0);
  assert(fd >= 0);
  struct statx status;
  assert(syscall(SYS_statx, AT_FDCWD, path, 0, STATX_TYPE, &status) == 0);
  assert(syscall(SYS_close, fd) == 0);

  size_t mask_size = 4096;
  void *mask = malloc(mask_size);
  assert(mask && get_id(mask) != 0);
  long mask_bytes = syscall(SYS_sched_getaffinity, 0, mask_size, mask);
  assert(mask_bytes > 0 && mask_bytes <= mask_size);
  assert(syscall(SYS_sched_setaffinity, 0, (size_t)mask_bytes, mask) == 0);
  free(mask);

  size_t page_size = sysconf(_SC_PAGESIZE);
  errno = EDOM;
  char *mapping = (char *)syscall(SYS_mmap, NULL, page_size,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, (long)0);
  assert(mapping != MAP_FAILED && get_id(mapping) != 0);
  assert(errno == EDOM);
  mapping[0] = 42;
  assert(syscall(SYS_mprotect, mapping, page_size, PROT_READ) == 0);
  assert(mapping[0] == 42);
  assert(syscall(SYS_munmap, mapping, page_size) == 0);
  return 0;
}
