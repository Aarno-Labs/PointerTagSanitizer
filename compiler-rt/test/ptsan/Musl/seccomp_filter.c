// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// RUN: %ptsan_musl_strict -fno-builtin %s -o %t
// RUN: %run %t good
// RUN: %run %t syscall
// RUN: %run %t queries
// RUN: not %run %t bad-filter 2>&1 | FileCheck %s
// RUN: not %run %t bad-syscall-filter 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict -fno-builtin %s -o %t.id32
// RUN: %run %t.id32 good
// RUN: %run %t.id32 syscall
// RUN: %run %t.id32 queries
// RUN: %run %t.id32 errors
// RUN: not %run %t.id32 bad-program 2>&1 | FileCheck %s
// RUN: not %run %t.id32 bad-filter 2>&1 | FileCheck %s
// RUN: not %run %t.id32 bad-syscall-program 2>&1 | FileCheck %s
// RUN: not %run %t.id32 bad-syscall-filter 2>&1 | FileCheck %s
// RUN: not %run %t.id32 bad-action 2>&1 | FileCheck %s
// RUN: not %run %t.id32 bad-sizes 2>&1 | FileCheck %s
// RUN: not %run %t.id32 bad-after 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_nonstrict -fno-builtin %s -o %t.ns
// RUN: %run %t.ns good
// CHECK: Found bad

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

// Linux classic-BPF ABI, independent of host kernel headers.
struct sock_filter {
  uint16_t code;
  uint8_t jt, jf;
  uint32_t k;
};
struct sock_fprog {
  uint16_t len;
  struct sock_filter *filter;
};
struct seccomp_notif_sizes {
  uint16_t notification;
  uint16_t response;
  uint16_t data;
};
enum { SECCOMP_MODE_FILTER = 2 };
enum {
  SECCOMP_SET_MODE_FILTER = 1,
  SECCOMP_GET_ACTION_AVAIL = 2,
  SECCOMP_GET_NOTIF_SIZES = 3,
  SECCOMP_RET_ALLOW = 0x7fff0000U,
};

static long install_filter(int via_syscall, struct sock_fprog *program) {
  if (via_syscall) {
    return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, program);
  }
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, (unsigned long)program, 0UL,
               0UL);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }

  if (!strcmp(argv[1], "queries")) {
    uint32_t action = SECCOMP_RET_ALLOW;
    if (syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action)) {
      return 2;
    }
    struct seccomp_notif_sizes sizes = {0};
    if (syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, &sizes) ||
        !sizes.notification || !sizes.response || !sizes.data) {
      return 3;
    }
    errno = 0;
    if (syscall(SYS_seccomp, 999, 0, &sizes) != -1 || errno != EINVAL) {
      return 4;
    }
    return 0;
  }
  if (!strcmp(argv[1], "bad-action")) {
    void *short_action = malloc(sizeof(uint32_t) - 1);
    if (!short_action) {
      return 4;
    }
    return syscall(SYS_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, short_action);
  }
  if (!strcmp(argv[1], "bad-sizes")) {
    void *short_sizes = malloc(sizeof(struct seccomp_notif_sizes) - 1);
    if (!short_sizes) {
      return 5;
    }
    return syscall(SYS_seccomp, SECCOMP_GET_NOTIF_SIZES, 0, short_sizes);
  }

  if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL)) {
    return 6;
  }
  struct sock_filter filter[] = {
      {0x20, 0, 0, 0},                   // LD W ABS: syscall number.
      {0x15, 0, 1, SYS_getppid},         // JMP JEQ K
      {0x06, 0, 0, 0x00050000U | EPERM}, // RET ERRNO
      {0x06, 0, 0, 0x7fff0000U},         // RET ALLOW
  };
  struct sock_fprog program = {sizeof(filter) / sizeof(filter[0]), filter};
  const int via_syscall = strstr(argv[1], "syscall") != NULL;
  if (!strcmp(argv[1], "bad-program") ||
      !strcmp(argv[1], "bad-syscall-program")) {
    void *short_program = malloc(sizeof(program) - 1);
    if (!short_program) {
      return 7;
    }
    return install_filter(via_syscall, short_program);
  }
  if (!strcmp(argv[1], "bad-filter") ||
      !strcmp(argv[1], "bad-syscall-filter")) {
    program.filter = malloc(sizeof(filter) - 1);
    if (!program.filter) {
      return 8;
    }
    return install_filter(via_syscall, &program);
  }
  if (!strcmp(argv[1], "errors")) {
    errno = 0;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, 0UL, 0UL, 0UL) != -1 ||
        errno != EFAULT) {
      return 5;
    }
    program.len = 0;
    errno = 0;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, (unsigned long)&program, 0UL,
              0UL) != -1 ||
        errno != EINVAL) {
      return 6;
    }
    program.len = 1;
    program.filter = NULL;
    errno = 0;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, (unsigned long)&program, 0UL,
              0UL) != -1 ||
        errno != EINVAL) {
      return 7;
    }
    errno = 0;
    if (prctl(PR_SET_SECCOMP, 999UL, 0UL, 0UL, 0UL) != -1 || errno != EINVAL) {
      return 8;
    }
    return 0;
  }
  struct sock_filter *saved_filter = program.filter;
  if (install_filter(via_syscall, &program)) {
    return 10;
  }
  if (program.filter != saved_filter || program.len != 4) {
    return 11;
  }
  if (prctl(PR_GET_SECCOMP, 0UL, 0UL, 0UL, 0UL) != SECCOMP_MODE_FILTER) {
    return 12;
  }
  errno = 0;
  if (syscall(SYS_getppid) != -1 || errno != EPERM) {
    return 13;
  }
  if (syscall(SYS_getpid) <= 0) {
    return 14;
  }
  if (!strcmp(argv[1], "bad-after")) {
    char *short_buffer = malloc(8);
    if (!short_buffer) {
      return 15;
    }
    memset(short_buffer, 0, 16);
    free(short_buffer);
  }
  return 0;
}
