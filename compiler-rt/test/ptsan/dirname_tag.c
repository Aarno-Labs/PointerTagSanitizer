//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: [TEST] dirname_basic
// CHECK: [OK]   dirname("/usr/lib") content: got "/usr"
// CHECK: [OK]   result is the input pointer (tag preserved)

// CHECK: [TEST] dirname_trailing_slash
// CHECK: [OK]   dirname("/a/b/") content: got "/a"
// CHECK: [OK]   result is the input pointer (tag preserved)

// CHECK: [TEST] dirname_no_slash
// CHECK: [OK]   dirname("hello") content: got "."

// CHECK: [TEST] dirname_null
// CHECK: [OK]   dirname(NULL) content: got "."

// CHECK: OK: dirname tests passed

#include <libgen.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *g_current_test = "(none)";

__attribute__((noinline))
static void sig_handler(int sig) {
  fprintf(stderr, "\n[CRASH] Caught signal %d while running test: %s\n", sig, g_current_test);
  fflush(stderr);
  _exit(128 + sig);
}

__attribute__((noinline))
static void install_signal_handlers(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sig_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESETHAND;
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}

__attribute__((noinline))
static int check_str_is(const char *what, const char *got, const char *want) {
  if (got == NULL || strcmp(got, want) != 0) {
    fprintf(stderr, "[FAIL] %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
    return 1;
  }
  fprintf(stderr, "[OK]   %s: got \"%s\"\n", what, got);
  return 0;
}

__attribute__((noinline))
static unsigned tag_of(char *p) {
  return (unsigned)((uintptr_t)p >> 48);
}

/* Pointer == is tag-blind under ptsan (the pass normalizes comparisons), so check the tag bits explicitly; a 0 tag on the malloc'd input means tagging is off and the test is vacuous. */
__attribute__((noinline))
static int check_same_ptr(char *got, char *want) {
  if (got != want || tag_of(got) != tag_of(want) || tag_of(want) == 0) {
    fprintf(stderr, "[FAIL] result pointer mismatch: got %p (tag 0x%04x), want %p (tag 0x%04x)\n",
            (void *)got, tag_of(got), (void *)want, tag_of(want));
    return 1;
  }
  fprintf(stderr, "[OK]   result is the input pointer (tag preserved)\n");
  return 0;
}

__attribute__((noinline))
static char *dup_tagged(const char *s) {
  char *p = (char *)malloc(strlen(s) + 1);
  if (p == NULL) {
    perror("malloc");
    _exit(2);
  }
  strcpy(p, s);
  return p;
}

/* glibc dirname truncates in place and returns path itself; the interceptor must hand back the caller's tagged pointer, and reads through it must pass bounds checks. */
__attribute__((noinline))
static int test_dirname_basic(void) {
  g_current_test = "dirname_basic";
  fprintf(stderr, "\n[TEST] %s\n", g_current_test);
  char *buf = dup_tagged("/usr/lib");
  char *r = dirname(buf);
  int rc = check_str_is("dirname(\"/usr/lib\") content", r, "/usr");
  rc |= check_same_ptr(r, buf);
  free(buf);
  return rc;
}

__attribute__((noinline))
static int test_dirname_trailing_slash(void) {
  g_current_test = "dirname_trailing_slash";
  fprintf(stderr, "\n[TEST] %s\n", g_current_test);
  char *buf = dup_tagged("/a/b/");
  char *r = dirname(buf);
  int rc = check_str_is("dirname(\"/a/b/\") content", r, "/a");
  rc |= check_same_ptr(r, buf);
  free(buf);
  return rc;
}

/* No slash: glibc returns static "." storage, which is not caller memory and must come back readable and un-retagged. */
__attribute__((noinline))
static int test_dirname_no_slash(void) {
  g_current_test = "dirname_no_slash";
  fprintf(stderr, "\n[TEST] %s\n", g_current_test);
  char *buf = dup_tagged("hello");
  char *r = dirname(buf);
  int rc = check_str_is("dirname(\"hello\") content", r, ".");
  // the static "." is libc memory, not the caller's: re-tagging it would make every deref an id/bounds mismatch
  if (tag_of(r) != 0) {
    fprintf(stderr, "[FAIL] static \".\" came back tagged (0x%04x)\n", tag_of(r));
    rc = 1;
  }
  free(buf);
  return rc;
}

__attribute__((noinline))
static int test_dirname_null(void) {
  g_current_test = "dirname_null";
  fprintf(stderr, "\n[TEST] %s\n", g_current_test);
  char *r = dirname(NULL);
  int rc = check_str_is("dirname(NULL) content", r, ".");
  // the static "." is libc memory, not the caller's: re-tagging it would make every deref an id/bounds mismatch
  if (tag_of(r) != 0) {
    fprintf(stderr, "[FAIL] static \".\" came back tagged (0x%04x)\n", tag_of(r));
    rc = 1;
  }
  return rc;
}

int main(void) {
  install_signal_handlers();

  int rc = 0;
  rc |= test_dirname_basic();
  rc |= test_dirname_trailing_slash();
  rc |= test_dirname_no_slash();
  rc |= test_dirname_null();

  if (rc == 0) {
    fprintf(stderr, "\nOK: dirname tests passed\n");
    return 0;
  }
  fprintf(stderr, "\nFAILED: some dirname tests failed (rc=%d)\n", rc);
  return 1;
}
