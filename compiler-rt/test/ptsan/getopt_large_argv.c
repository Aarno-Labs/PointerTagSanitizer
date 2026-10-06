//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt 2>&1
// RUN: cat out.txt | FileCheck %s

// Companion to getopt_restore_tags.c for argc above the interceptor's
// fixed stack save buffer (kPTSanArgvSaveStackCap = 128), forcing the
// heap-allocated save array. Every argv slot must still get its tag back
// after parsing, and optarg must be re-tagged. Also pins the no-fallback
// contract: there is no argc where getopt* silently strips tags.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NOPTS 300

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

  // argv[0], NOPTS "-a" flags, then "-b hello": argc far above the stack cap.
  int argc = NOPTS + 3;
  char **argv = (char **)malloc((argc + 1) * sizeof(char *));
  argv[0] = dup_str("prog");
  for (int i = 1; i <= NOPTS; i++)
    argv[i] = dup_str("-a");
  argv[NOPTS + 1] = dup_str("-b");
  argv[NOPTS + 2] = dup_str("hello");
  argv[argc] = NULL;

  // Precondition: the relocated argv really is tagged before parsing.
  if (!tagged(argv[1]) || !tagged(argv[NOPTS + 2])) {
    printf("precondition failed: argv not tagged\n");
    return 1;
  }

  int c;
  int a_count = 0;
  char *b_arg = NULL;
  while ((c = getopt(argc, argv, "ab:")) != -1) {
    if (c == 'a')
      a_count++;
    if (c == 'b')
      b_arg = optarg;
  }

  if (a_count != NOPTS) {
    printf("parse failed: a_count=%d want=%d\n", a_count, NOPTS);
    return 1;
  }
  printf("parsed %d options\n", a_count);
  // CHECK: parsed 300 options

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
