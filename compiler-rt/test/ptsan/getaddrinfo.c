//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt 2>&1
// RUN: cat out.txt | FileCheck %s

// getaddrinfo() reads environ internally (resolver / nss config). With a
// ptsan-tagged environ -- as PostgreSQL produces via ps_status before
// StreamServerPort/pgstat_init -- the interceptor must swap in an untagged
// environ (PTSAN_ENV_GUARD) before REAL(getaddrinfo), or the uninstrumented
// libc walk of environ faults. Regression for the getaddrinfo env-guard.
//
// Hermetic: NULL node + AI_PASSIVE + AI_NUMERICSERV resolves to the IPv4
// wildcard with no DNS/network access -> deterministic 0.0.0.0:8080.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

extern char **environ;

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

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

  struct addrinfo *res = NULL;
  int rc = getaddrinfo(NULL, "8080", &hints, &res);
  if (rc != 0) {
    printf("getaddrinfo failed: %s\n", gai_strerror(rc));
    return 1;
  }

  char ip[INET_ADDRSTRLEN];
  struct sockaddr_in *a = (struct sockaddr_in *)res->ai_addr;
  inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
  printf("getaddrinfo ok %s:%d\n", ip, ntohs(a->sin_port));
  // CHECK: getaddrinfo ok 0.0.0.0:8080

  freeaddrinfo(res);
  return 0;
}
