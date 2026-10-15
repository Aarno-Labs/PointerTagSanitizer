//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt 2>&1
// RUN: cat out.txt | FileCheck %s

// getopt_long_only() accepts long options with a single '-'. Same env/argv/
// longopts tag-stripping path as getopt_long: with a ptsan-tagged environ +
// tagged argv the interceptor swaps in an untagged environ (PTSAN_ENV_GUARD),
// deep-untags argv, and rebuilds an untagged longopts copy before
// REAL(getopt_long_only). Regression for the getopt_long_only env-guard.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <getopt.h>
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

int main(void) {
  tag_environ();

  // tagged argv with single-dash long options: "prog -alpha -beta val"
  char **argv = (char **)malloc(5 * sizeof(char *));
  argv[0] = dup_str("prog");
  argv[1] = dup_str("-alpha");
  argv[2] = dup_str("-beta");
  argv[3] = dup_str("val");
  argv[4] = NULL;
  int argc = 4;

  static struct option longopts[] = {
      {"alpha", no_argument, 0, 'a'},
      {"beta", required_argument, 0, 'b'},
      {0, 0, 0, 0},
  };

  int c, idx;
  while ((c = getopt_long_only(argc, argv, "ab:", longopts, &idx)) != -1) {
    if (c == 'a')
      printf("opt a\n");
    else if (c == 'b')
      printf("opt b arg %s\n", optarg);
    else
      printf("opt ?\n");
  }
  printf("done\n");
  // CHECK: opt a
  // CHECK: opt b arg val
  // CHECK: done
  return 0;
}
