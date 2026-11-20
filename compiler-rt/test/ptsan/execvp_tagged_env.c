// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: hello world


// reassign_environ_execvp.c
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern char **environ;

__attribute__((noinline))
static char *dup_kv(const char *k, const char *v) {
  size_t kl = strlen(k);
  size_t vl = strlen(v);
  char *s = (char *)malloc(kl + 1 + vl + 1);
  if (s == NULL) {
    perror("malloc");
    _exit(2);
  }
  memcpy(s, k, kl);
  s[kl] = '=';
  memcpy(s + kl + 1, v, vl);
  s[kl + 1 + vl] = '\0';
  return s;
}

__attribute__((noinline))
static void ensure_path_minimal() {
  const char *p = getenv("PATH");
  if (p == NULL || *p == '\0') {
    if (setenv("PATH", "/usr/bin:/bin", 1) != 0) {
      perror("setenv(PATH)");
      _exit(3);
    }
  }
}

__attribute__((noinline))
static void reassign_environ_vector() {
  /* Build a brand-new env vector backed by malloc. */
  /* Two entries + NULL terminator; add more if you like. */
  char **newenv = (char **)malloc(3 * sizeof(char *));
  if (newenv == NULL) {
    perror("malloc");
    _exit(4);
  }

  /* Make sure execvp can find programs. */
  ensure_path_minimal();

  /* Copy PATH out of current environment so it’s not a literal. */
  const char *path = getenv("PATH");
  if (path == NULL) {
    path = "/usr/bin:/bin";
  }

  newenv[0] = dup_kv("PATH", path);                /* malloc-backed */
  newenv[1] = dup_kv("PTSANTAG", "from_malloc");   /* malloc-backed */
  newenv[2] = NULL;

  /* *** The critical store: reassign the global environ pointer. *** */
  environ = newenv;
}

__attribute__((noinline))
static void do_execvp_echo(void) {
  char *argv[4];
  argv[0] = (char *)"echo";
  argv[1] = (char *)"hello";
  argv[2] = (char *)"world";
  argv[3] = NULL;

  execvp("echo", argv);
  perror("execvp echo");  /* only runs on failure */
  _exit(127);
}

__attribute__((noinline))
static void getenv_post_reassign() {
  const char *path = getenv("PATH");
  printf("post reassign, got path: %s\n", path);
}


int main(void) {
  reassign_environ_vector();

  getenv_post_reassign();

  do_execvp_echo();
  return 0;
}
