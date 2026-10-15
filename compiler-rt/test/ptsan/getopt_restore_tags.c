//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt 2>&1
// RUN: cat out.txt | FileCheck %s

// getopt() strips ptsan ids from the caller's argv for the uninstrumented libc
// call, but the application keeps using argv (and optarg) afterward -- those
// accesses must stay bounds-checked. The interceptor therefore restores the tags
// after parsing. Here argv is relocated into tagged heap (as ps_status does);
// after the getopt loop every argv[i] -- and optarg -- must still carry a ptsan
// id in its high bits, and dereferencing optarg must still work.
// Regression for ptsan_argv_restore_tags.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

__attribute__((noinline)) static char *dup_str(const char *s) {
  size_t n = strlen(s) + 1;
  char *p = (char *)malloc(n);
  if (!p) {
    perror("malloc");
    _exit(2);
  }
  memcpy(p, s, n);
  return p;
}

__attribute__((noinline)) static char *dup_kv(const char *k, const char *v) {
  size_t kl = strlen(k), vl = strlen(v);
  char *s = (char *)malloc(kl + 1 + vl + 1);
  if (!s) {
    perror("malloc");
    _exit(2);
  }
  memcpy(s, k, kl);
  s[kl] = '=';
  memcpy(s + kl + 1, v, vl + 1);
  return s;
}

__attribute__((noinline)) static void tag_environ(void) {
  const char *path = getenv("PATH");
  if (!path)
    path = "/usr/bin:/bin";
  char **e = (char **)malloc(2 * sizeof(char *));
  if (!e) {
    perror("malloc");
    _exit(2);
  }
  e[0] = dup_kv("PATH", path);
  e[1] = NULL;
  environ = e;
}

// True if p carries a ptsan id in its high 16 bits.
__attribute__((noinline)) static int tagged(const void *p) {
  return p && ((uintptr_t)p >> 48) != 0;
}

int main(void) {
  tag_environ();

  char **argv = (char **)malloc(5 * sizeof(char *));
  argv[0] = dup_str("prog");
  argv[1] = dup_str("-a");
  argv[2] = dup_str("-b");
  argv[3] = dup_str("hello");
  argv[4] = NULL;
  int argc = 4;

  // Precondition: the relocated argv really is tagged before parsing.
  if (!tagged(argv[1])) {
    printf("precondition failed: argv not tagged\n");
    return 1;
  }

  int c;
  char *b_arg = NULL;
  while ((c = getopt(argc, argv, "ab:")) != -1) {
    if (c == 'b')
      b_arg = optarg;
  }

  // After parsing, the tags must be back on every caller slot.
  for (int i = 0; i < argc; i++) {
    if (!tagged(argv[i])) {
      printf("argv[%d] tag NOT restored\n", i);
      return 1;
    }
  }
  printf("argv tags restored\n");
  // CHECK: argv tags restored

  // optarg must be re-tagged AND still point at the right (valid) bytes.
  if (!tagged(b_arg)) {
    printf("optarg tag NOT restored\n");
    return 1;
  }
  printf("optarg restored: %s\n", b_arg);
  // CHECK: optarg restored: hello
  return 0;
}
