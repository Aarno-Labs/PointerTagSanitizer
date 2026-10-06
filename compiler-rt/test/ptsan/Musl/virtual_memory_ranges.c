//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise public musl APIs that operate on virtual-memory ranges without
// creating or retiring allocation metadata.
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t

#define _GNU_SOURCE

#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

static int lock_failure_is_environmental(int error) {
  return error == EAGAIN || error == EINVAL || error == ENOMEM ||
         error == ENOSYS || error == EPERM;
}

int main(void) {
  long page_size_value = sysconf(_SC_PAGESIZE);
  if (page_size_value <= 0) {
    return 1;
  }

  size_t page_size = (size_t)page_size_value;
  size_t mapping_size = 2 * page_size;
  char *mapping = (char *)mmap(NULL, mapping_size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mapping == MAP_FAILED) {
    return 2;
  }

  mapping[0] = 1;
  if (mprotect(mapping, page_size, PROT_READ) != 0 ||
      mprotect(mapping, page_size, PROT_READ | PROT_WRITE) != 0) {
    munmap(mapping, mapping_size);
    return 3;
  }

  if (madvise(mapping, mapping_size, MADV_NORMAL) != 0 ||
      msync(mapping, mapping_size, MS_SYNC) != 0) {
    munmap(mapping, mapping_size);
    return 4;
  }

  if (mlock(mapping, page_size) == 0) {
    if (munlock(mapping, page_size) != 0) {
      munmap(mapping, mapping_size);
      return 5;
    }
  } else if (!lock_failure_is_environmental(errno)) {
    munmap(mapping, mapping_size);
    return 6;
  }

  if (mlock2(mapping, page_size, MLOCK_ONFAULT) == 0) {
    if (munlock(mapping, page_size) != 0) {
      munmap(mapping, mapping_size);
      return 7;
    }
  } else if (!lock_failure_is_environmental(errno)) {
    munmap(mapping, mapping_size);
    return 8;
  }

  errno = 0;
  if (remap_file_pages(mapping, 0, 0, 0, 0) != 0 && errno != EINVAL &&
      errno != ENOSYS && errno != EPERM) {
    munmap(mapping, mapping_size);
    return 9;
  }

  // A VM range may extend beyond one PTSan object's bounds. The kernel remains
  // responsible for accepting or rejecting the actual mapped-page range.
  (void)madvise(mapping + page_size, mapping_size, MADV_NORMAL);

  if (munmap(mapping, mapping_size) != 0) {
    return 10;
  }
  return 0;
}
