// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Regression for exec shadow buffers surviving CLONE_VM posix_spawn children.
// RUN: %ptsan_musl_strict -O1 -g -o %t %s
// RUN: %run %t
// RUN: %ptsan_musl_id32_strict -O1 -g -o %t.id32 %s
// RUN: %run %t.id32
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t signal_exec_failed;
static void exec_from_signal(int unused) {
  (void)unused;
  char *const args[] = {"missing", 0};
  char *const env[] = {0};
  if (execve("/ptsan-no-such-executable", args, env) == -1 && errno == ENOENT) {
    signal_exec_failed = 1;
  }
}

static long rss_kib(void) {
  FILE *f = fopen("/proc/self/smaps_rollup", "r");
  if (!f) {
    return -1;
  }
  char line[256];
  long rss = -1;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "Rss: %ld kB", &rss) == 1) {
      break;
    }
  }
  fclose(f);
  return rss;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child")) {
    return 0;
  }
  struct sigaction action = {0};
  action.sa_handler = exec_from_signal;
  if (sigaction(SIGUSR1, &action, 0) || raise(SIGUSR1) || !signal_exec_failed) {
    return 1;
  }
  char *args[] = {"/proc/self/exe", "child", 0};
  char *env[] = {"PTSAN_EXEC_REUSE=1", 0};
  char *large_env[257];
  for (int i = 0; i < 256; i++) {
    large_env[i] = env[0];
  }
  large_env[256] = 0;
  long warm = -1;
  long grown = -1;
  for (int i = 1; i <= 4000; i++) {
    pid_t pid;
    if (posix_spawn(&pid, "/proc/self/exe", 0, 0, args,
                    i <= 2000 ? env : large_env)) {
      return 2;
    }
    int status;
    if (waitpid(pid, &status, 0) != pid || status) {
      return 3;
    }
    if (i == 100) {
      warm = rss_kib();
    }
    if (i == 2100) {
      grown = rss_kib();
    }
  }
  long final = rss_kib();
  // The old one-shot shadows retained over 4 MiB during the large-vector half
  // of this loop. Leave room for unrelated allocator and ID-bookkeeping pages.
  if (warm < 0 || grown < 0 || final < 0 || final - grown > 1024) {
    fprintf(stderr, "spawn RSS grew from %ld through %ld to %ld KiB\n", warm,
            grown, final);
    return 4;
  }
  return 0;
}
