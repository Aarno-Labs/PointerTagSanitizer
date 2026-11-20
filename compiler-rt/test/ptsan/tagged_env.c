// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> out.txt
// RUN: cat out.txt | FileCheck %s

// CHECK: [TEST] getenv
// CHECK: [OK]   getenv PTSANTAG after reassign environ: got "from_malloc"

// CHECK: [TEST] setenv_basic
// CHECK: [OK]   getenv ENVTEST_A: got "aaa"
// CHECK: [OK]   getenv ENVTEST_A (no replace): got "aaa"
// CHECK: [OK]   getenv ENVTEST_A (replace): got "bbb"

// CHECK: [TEST] unsetenv
// CHECK: [OK]   getenv ENVTEST_B before unset: got "xyz"
// CHECK: [OK]   getenv ENVTEST_B after unset: got NULL
// CHECK: [OK]   unsetenv("") -> -1/EINVAL
// CHECK: [OK]   unsetenv("BAD=NAME") -> -1/EINVAL

// CHECK: [TEST] putenv_aliasing
// CHECK: [OK]   getenv ENVTEST_C after putenv: got "hello"
// CHECK: [OK]   getenv ENVTEST_C sees mutated buffer: got "hullo"

// CHECK: [TEST] clearenv_roundtrip
// CHECK: [OK]   getenv ENVTEST_D after clearenv: got NULL


#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

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

static const char *g_current_test = "(none)";

__attribute__((noinline))
static void sig_handler(int sig) {
  const char *name = g_current_test ? g_current_test : "(null)";
  fprintf(stderr, "\n[CRASH] Caught signal %d while running test: %s\n", sig, name);
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
static int fail_errno(const char *where) {
  int e = errno;
  fprintf(stderr, "[FAIL] %s: errno=%d (%s)\n", where, e, strerror(e));
  return 1;
}

__attribute__((noinline))
static int check_int_eq(const char *what, int got, int want) {
  if (got != want) {
    fprintf(stderr, "[FAIL] %s: got=%d want=%d\n", what, got, want);
    return 1;
  }
  fprintf(stderr, "[OK]   %s: got=%d\n", what, got);
  return 0;
}

__attribute__((noinline))
static int check_str_is(const char *what, const char *got, const char *want) {
  if (want == NULL) {
    if (got != NULL) {
      fprintf(stderr, "[FAIL] %s: expected NULL, got \"%s\"\n", what, got);
      return 1;
    }
    fprintf(stderr, "[OK]   %s: got NULL\n", what);
    return 0;
  }
  if (got == NULL) {
    fprintf(stderr, "[FAIL] %s: expected \"%s\", got NULL\n", what, want);
    return 1;
  }
  if (strcmp(got, want) != 0) {
    fprintf(stderr, "[FAIL] %s: got \"%s\", want \"%s\"\n", what, got, want);
    return 1;
  }
  fprintf(stderr, "[OK]   %s: got \"%s\"\n", what, got);
  return 0;
}

/* ---------- Basic setenv / getenv ---------- */
__attribute__((noinline))
static int test_setenv_basic(void) {
  g_current_test = "setenv_basic";
  fprintf(stderr, "\n[TEST] %s\n", g_current_test);

  int rc = 0;
  errno = 0;
  if (unsetenv("ENVTEST_A") != 0) {
    /* ignore errors here; it may not exist */
    errno = 0;
  }

  errno = 0;
  if (setenv("ENVTEST_A", "aaa", 0) != 0) {
    return fail_errno("setenv ENVTEST_A=aaa replace=0");
  }
  rc |= check_str_is("getenv ENVTEST_A", getenv("ENVTEST_A"), "aaa");

  errno = 0;
  if (setenv("ENVTEST_A", "bbb", 0) != 0) {
    return fail_errno("setenv ENVTEST_A=bbb replace=0");
  }
  rc |= check_str_is("getenv ENVTEST_A (no replace)", getenv("ENVTEST_A"), "aaa");

  errno = 0;
  if (setenv("ENVTEST_A", "bbb", 1) != 0) {
    return fail_errno("setenv ENVTEST_A=bbb replace=1");
  }
  rc |= check_str_is("getenv ENVTEST_A (replace)", getenv("ENVTEST_A"), "bbb");

  return rc;
}

/* ---------- unsetenv semantics ---------- */
__attribute__((noinline))
static int test_unsetenv(void) {
  g_current_test = "unsetenv";
  fprintf(stderr, "\n[TEST] %s\n", g_current_test);

  int rc = 0;

  errno = 0;
  if (setenv("ENVTEST_B", "xyz", 1) != 0) {
    return fail_errno("setenv ENVTEST_B=xyz");
  }
  rc |= check_str_is("getenv ENVTEST_B before unset", getenv("ENVTEST_B"), "xyz");

  errno = 0;
  if (unsetenv("ENVTEST_B") != 0) {
    return fail_errno("unsetenv ENVTEST_B");
  }
  rc |= check_str_is("getenv ENVTEST_B after unset", getenv("ENVTEST_B"), NULL);

  errno = 0;
  int r = unsetenv("");
  if (!(r == -1 && errno == EINVAL)) {
    fprintf(stderr, "[FAIL] unsetenv(\"\"): r=%d errno=%d (%s); expected -1/EINVAL\n",
            r, errno, strerror(errno));
    rc = 1;
  } else {
    fprintf(stderr, "[OK]   unsetenv(\"\") -> -1/EINVAL\n");
  }

  errno = 0;
  r = unsetenv("BAD=NAME");
  if (!(r == -1 && errno == EINVAL)) {
    fprintf(stderr, "[FAIL] unsetenv(\"BAD=NAME\"): r=%d errno=%d (%s); expected -1/EINVAL\n",
            r, errno, strerror(errno));
    rc = 1;
  } else {
    fprintf(stderr, "[OK]   unsetenv(\"BAD=NAME\") -> -1/EINVAL\n");
  }

  return rc;
}

/* ---------- putenv aliasing semantics ---------- */
__attribute__((noinline))
static int test_putenv_aliasing(void) {
  g_current_test = "putenv_aliasing";
  fprintf(stderr, "\n[TEST] %s\n", g_current_test);

  int rc = 0;

  errno = 0;
  if (unsetenv("ENVTEST_C") != 0) {
    errno = 0;
  }

  const char *k = "ENVTEST_C";
  const char *v = "hello";
  size_t klen = strlen(k);
  size_t vlen = strlen(v);
  size_t len = klen + 1 + vlen + 1;

  char *buf = (char *)malloc(len);
  if (buf == NULL) {
    return fail_errno("malloc buf");
  }

  memcpy(buf, k, klen);
  buf[klen] = '=';
  memcpy(buf + klen + 1, v, vlen);
  buf[len - 1] = '\0';

  errno = 0;
  if (putenv(buf) != 0) {
    return fail_errno("putenv(\"ENVTEST_C=hello\")");
  }
  rc |= check_str_is("getenv ENVTEST_C after putenv", getenv("ENVTEST_C"), "hello");

  /* Mutate the buffer in place: "hello" -> "hullo" (if long enough) */
  char *valptr = strchr(buf, '=');
  if (valptr != NULL) {
    valptr++;
    if (strlen(valptr) >= 2) {
      valptr[1] = 'u';
      rc |= check_str_is("getenv ENVTEST_C sees mutated buffer", getenv("ENVTEST_C"), "hullo");
    } else {
      fprintf(stderr, "[WARN] ENVTEST_C value too short to mutate\n");
    }
  } else {
    fprintf(stderr, "[WARN] putenv buffer missing '=' unexpectedly\n");
  }

  errno = 0;
  if (unsetenv("ENVTEST_C") != 0) {
    return fail_errno("unsetenv ENVTEST_C after putenv");
  }

  /* Do NOT free(buf); environment may still reference it in some libcs. */
  return rc;
}

/* ---------- clearenv semantics ---------- */
__attribute__((noinline))
static int test_clearenv_roundtrip(void) {
  g_current_test = "clearenv_roundtrip";
  fprintf(stderr, "\n[TEST] %s\n", g_current_test);

  int rc = 0;

  errno = 0;
  if (setenv("ENVTEST_D", "persist", 1) != 0) {
    return fail_errno("setenv ENVTEST_D=persist");
  }

  errno = 0;
  if (clearenv() != 0) {
    return fail_errno("clearenv");
  }

  rc |= check_str_is("getenv ENVTEST_D after clearenv", getenv("ENVTEST_D"), NULL);

  /* Recreate minimal PATH so later execs/tests won’t fail unexpectedly. */
  errno = 0;
  if (setenv("PATH", "/usr/bin:/bin", 1) != 0) {
    return fail_errno("setenv PATH after clearenv");
  }

  return rc;
}

__attribute__((noinline))
static int test_getenv_tagged() {
  g_current_test = "getenv";
  fprintf(stderr, "\n[TEST] %s\n", g_current_test);

  int rc = check_str_is("getenv PTSANTAG after reassign environ", getenv("PTSANTAG"), "from_malloc");

  return 0;
}


int main(void) {
  reassign_environ_vector();

  install_signal_handlers();

  int rc = 0;
  rc |= test_getenv_tagged();
  rc |= test_setenv_basic();
  rc |= test_unsetenv();
  rc |= test_putenv_aliasing();
  rc |= test_clearenv_roundtrip();

  if (rc == 0) {
    printf("\nOK: getenv/setenv/unsetenv/putenv/clearenv tests passed\n");
    return 0;
  } else {
    printf("\nFAILED: some env API tests failed (rc=%d)\n", rc);
    return 1;
  }
}
