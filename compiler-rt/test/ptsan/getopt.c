//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt 2>&1
// RUN: cat out.txt | FileCheck %s

// getopt() reads environ internally (getenv("POSIXLY_CORRECT")) and scans the
// argv strings. With a ptsan-tagged environ (reassigned to malloc-backed
// entries, as PostgreSQL's ps_status does) and tagged argv, the interceptor
// must swap in an untagged environ (PTSAN_ENV_GUARD) and deep-untag argv before
// REAL(getopt) -- otherwise the uninstrumented libc walk of environ faults.
// Regression for the getopt env-guard.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
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

// Reassign environ to a fresh malloc-backed (ptsan-tagged) vector.
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

int main(void) {
  tag_environ();

  // tagged argv: "prog -a -b hello"
  char **argv = (char **)malloc(5 * sizeof(char *));
  argv[0] = dup_str("prog");
  argv[1] = dup_str("-a");
  argv[2] = dup_str("-b");
  argv[3] = dup_str("hello");
  argv[4] = NULL;
  int argc = 4;

  int c;
  while ((c = getopt(argc, argv, "ab:")) != -1) {
    if (c == 'a')
      printf("opt a\n");
    else if (c == 'b')
      printf("opt b arg %s\n", optarg);
    else
      printf("opt ?\n");
  }
  printf("done\n");
  // CHECK: opt a
  // CHECK: opt b arg hello
  // CHECK: done
  return 0;
}
