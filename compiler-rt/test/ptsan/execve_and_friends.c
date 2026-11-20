// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe > out.txt
// RUN: cat out.txt | FileCheck %s

//CHECK: ok 1 - execve
//CHECK: ok 2 - fexecve
//CHECK: ok 3 - execv
//CHECK: ok 4 - execvp
//CHECK: ok 5 - execvpe
//CHECK: ok 6 - execl
//CHECK: ok 7 - execlp
//CHECK: ok 8 - execle

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---------- config ---------- */
static const char *ECHO_ABS = "/bin/echo";   /* absolute for execve/execv/execle */
static const char *ECHO_BARE = "echo";       /* PATH-search for execvp/execlp */
extern char **environ;

/* ---------- tiny util ---------- */
__attribute__((noinline))
static char *xstrdup_heap(const char *s) {
  size_t n = strlen(s);
  char *p = (char *)malloc(n + 1);
  if (p == NULL) {
    perror("malloc");
    exit(2);
  }
  memcpy(p, s, n + 1);
  return p;
}

__attribute__((noinline))
static void free_strv(char **v) {
  if (v == NULL) {
    return;
  }
  for (size_t i = 0; v[i] != NULL; i++) {
    free(v[i]);
  }
  free(v);
}

__attribute__((noinline))
static char **make_heap_argv3(const char *a0, const char *a1, const char *a2) {
  char **v = (char **)malloc(4 * sizeof(char *));
  if (v == NULL) {
    perror("malloc");
    exit(2);
  }
  v[0] = xstrdup_heap(a0);
  v[1] = xstrdup_heap(a1);
  v[2] = xstrdup_heap(a2);
  v[3] = NULL;
  return v;
}

__attribute__((noinline))
static char **make_heap_env2(const char *e0, const char *e1) {
  char **v = (char **)malloc(3 * sizeof(char *));
  if (v == NULL) {
    perror("malloc");
    exit(2);
  }
  v[0] = xstrdup_heap(e0);
  v[1] = xstrdup_heap(e1);
  v[2] = NULL;
  return v;
}

__attribute__((noinline))
static bool read_all_into_buf(int fd, char *buf, size_t cap, size_t *out_n) {
  size_t n = 0;
  for (;;) {
    if (n == cap) {
      break;
    }
    ssize_t r = read(fd, buf + n, cap - n);
    if (r == 0) {
      break;
    }
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    n += (size_t)r;
  }
  if (out_n != NULL) {
    *out_n = n;
  }
  if (n < cap) {
    buf[n] = '\0';
  } else {
    buf[cap - 1] = '\0';
  }
  return true;
}

__attribute__((noinline))
static bool run_and_capture(char *const argv[], char *const envp[], const char *expect_token, bool use_fd_exec) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    return false;
  }

  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(pipefd[0]);
    close(pipefd[1]);
    return false;
  }

  if (pid == 0) {
    /* child */
    if (dup2(pipefd[1], 1) < 0) {
      _exit(111);
    }
    close(pipefd[0]);
    close(pipefd[1]);

    if (use_fd_exec) {
      /* argv[0] is absolute path; open and fexecve */
      int fd = open(argv[0], O_RDONLY);
      if (fd < 0) {
        _exit(112);
      }
      fexecve(fd, argv, envp);
      _exit(113);
    } else {
      /* decide which exact call by argv[0]/argv[1] markers */
      /* We will not come back here on success. */
      execve(argv[0], argv, envp);
      _exit(114);
    }
  }

  /* parent */
  close(pipefd[1]);
  char buf[4096];
  size_t n = 0;
  bool ok_read = read_all_into_buf(pipefd[0], buf, sizeof(buf), &n);
  close(pipefd[0]);

  int status = 0;
  (void)waitpid(pid, &status, 0);

  if (!ok_read) {
    return false;
  }
  if (strstr(buf, expect_token) == NULL) {
    return false;
  }
  if (!WIFEXITED(status)) {
    return false;
  }
  /* echo typically exits 0 */
  if (WEXITSTATUS(status) != 0) {
    return false;
  }
  return true;
}

/* ---------- individual tests (each noinline) ---------- */

__attribute__((noinline))
static bool test_execve(void) {
  char **argv = make_heap_argv3(ECHO_ABS, "execve-ok", "X");
  /* minimal envp; prove we handle non-const strings */
  char **envp = make_heap_env2("A=1", "B=2");
  bool ok = run_and_capture(argv, envp, "execve-ok", false);
  free_strv(argv);
  free_strv(envp);
  return ok;
}

__attribute__((noinline))
static bool test_fexecve(void) {
  char **argv = make_heap_argv3(ECHO_ABS, "fexecve-ok", "Y");
  char **envp = make_heap_env2("C=3", "D=4");
  bool ok = run_and_capture(argv, envp, "fexecve-ok", true);
  free_strv(argv);
  free_strv(envp);
  return ok;
}

__attribute__((noinline))
static bool test_execv(void) {
  /* execv(path, argv) uses current environ; still build heap argv */
  char **argv = make_heap_argv3(ECHO_ABS, "execv-ok", "Z");
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    free_strv(argv);
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    free_strv(argv);
    close(pipefd[0]); close(pipefd[1]);
    return false;
  }
  if (pid == 0) {
    if (dup2(pipefd[1], 1) < 0) { _exit(121); }
    close(pipefd[0]); close(pipefd[1]);
    execv(argv[0], argv);
    _exit(122);
  }
  close(pipefd[1]);
  char buf[4096]; size_t n = 0;
  bool ok_read = read_all_into_buf(pipefd[0], buf, sizeof(buf), &n);
  close(pipefd[0]);
  int status = 0; (void)waitpid(pid, &status, 0);
  free_strv(argv);
  if (!ok_read) { return false; }
  if (strstr(buf, "execv-ok") == NULL) { return false; }
  if (!WIFEXITED(status)) { return false; }
  if (WEXITSTATUS(status) != 0) { return false; }
  return true;
}

__attribute__((noinline))
static bool test_execvp(void) {
  char **argv = make_heap_argv3(ECHO_BARE, "execvp-ok", "W");
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    free_strv(argv);
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    free_strv(argv);
    close(pipefd[0]); close(pipefd[1]);
    return false;
  }
  if (pid == 0) {
    if (dup2(pipefd[1], 1) < 0) { _exit(131); }
    close(pipefd[0]); close(pipefd[1]);
    execvp(argv[0], argv);
    _exit(132);
  }
  close(pipefd[1]);
  char buf[4096]; size_t n = 0;
  bool ok_read = read_all_into_buf(pipefd[0], buf, sizeof(buf), &n);
  close(pipefd[0]);
  int status = 0; (void)waitpid(pid, &status, 0);
  free_strv(argv);
  if (!ok_read) { return false; }
  if (strstr(buf, "execvp-ok") == NULL) { return false; }
  if (!WIFEXITED(status)) { return false; }
  if (WEXITSTATUS(status) != 0) { return false; }
  return true;
}

__attribute__((noinline))
static bool test_execvpe(void) {
  /* Provide custom env to ensure envp deep-scrubbing path is exercised. */
  char **argv = make_heap_argv3(ECHO_BARE, "execvpe-ok", "V");
  char **envp = make_heap_env2("E=5", "F=6");
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    free_strv(argv); free_strv(envp);
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    free_strv(argv); free_strv(envp);
    close(pipefd[0]); close(pipefd[1]);
    return false;
  }
  if (pid == 0) {
    if (dup2(pipefd[1], 1) < 0) { _exit(141); }
    close(pipefd[0]); close(pipefd[1]);
    extern int execvpe(const char *, char *const [], char *const []);
    /* cast away const on argv/envp vector type for the prototype */
    execvpe(argv[0], argv, envp);
    _exit(142);
  }
  close(pipefd[1]);
  char buf[4096]; size_t n = 0;
  bool ok_read = read_all_into_buf(pipefd[0], buf, sizeof(buf), &n);
  close(pipefd[0]);
  int status = 0; (void)waitpid(pid, &status, 0);
  free_strv(argv); free_strv(envp);
  if (!ok_read) { return false; }
  if (strstr(buf, "execvpe-ok") == NULL) { return false; }
  if (!WIFEXITED(status)) { return false; }
  if (WEXITSTATUS(status) != 0) { return false; }
  return true;
}

__attribute__((noinline))
static bool test_execl(void) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(pipefd[0]); close(pipefd[1]);
    return false;
  }
  if (pid == 0) {
    if (dup2(pipefd[1], 1) < 0) { _exit(151); }
    close(pipefd[0]); close(pipefd[1]);
    /* varargs must end with (char*)0; pass heap-backed strings first */
    char *a0 = xstrdup_heap(ECHO_ABS);
    char *a1 = xstrdup_heap("execl-ok");
    char *a2 = xstrdup_heap("U");
    execl(a0, a0, a1, a2, (char *)0);
    _exit(152);
  }
  close(pipefd[1]);
  char buf[4096]; size_t n = 0;
  bool ok_read = read_all_into_buf(pipefd[0], buf, sizeof(buf), &n);
  close(pipefd[0]);
  int status = 0; (void)waitpid(pid, &status, 0);
  if (!ok_read) { return false; }
  if (strstr(buf, "execl-ok") == NULL) { return false; }
  if (!WIFEXITED(status)) { return false; }
  if (WEXITSTATUS(status) != 0) { return false; }
  return true;
}

__attribute__((noinline))
static bool test_execlp(void) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(pipefd[0]); close(pipefd[1]);
    return false;
  }
  if (pid == 0) {
    if (dup2(pipefd[1], 1) < 0) { _exit(161); }
    close(pipefd[0]); close(pipefd[1]);
    char *a0 = xstrdup_heap(ECHO_BARE);
    char *a1 = xstrdup_heap("execlp-ok");
    char *a2 = xstrdup_heap("T");
    execlp(a0, a0, a1, a2, (char *)0);
    _exit(162);
  }
  close(pipefd[1]);
  char buf[4096]; size_t n = 0;
  bool ok_read = read_all_into_buf(pipefd[0], buf, sizeof(buf), &n);
  close(pipefd[0]);
  int status = 0; (void)waitpid(pid, &status, 0);
  if (!ok_read) { return false; }
  if (strstr(buf, "execlp-ok") == NULL) { return false; }
  if (!WIFEXITED(status)) { return false; }
  if (WEXITSTATUS(status) != 0) { return false; }
  return true;
}

__attribute__((noinline))
static bool test_execle(void) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    perror("pipe");
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    close(pipefd[0]); close(pipefd[1]);
    return false;
  }
  if (pid == 0) {
    if (dup2(pipefd[1], 1) < 0) { _exit(171); }
    close(pipefd[0]); close(pipefd[1]);
    char **envp = make_heap_env2("G=7", "H=8");
    char *a0 = xstrdup_heap(ECHO_ABS);
    char *a1 = xstrdup_heap("execle-ok");
    char *a2 = xstrdup_heap("S");
    execle(a0, a0, a1, a2, (char *)0, envp);
    _exit(172);
  }
  close(pipefd[1]);
  char buf[4096]; size_t n = 0;
  bool ok_read = read_all_into_buf(pipefd[0], buf, sizeof(buf), &n);
  close(pipefd[0]);
  int status = 0; (void)waitpid(pid, &status, 0);
  if (!ok_read) { return false; }
  if (strstr(buf, "execle-ok") == NULL) { return false; }
  if (!WIFEXITED(status)) { return false; }
  if (WEXITSTATUS(status) != 0) { return false; }
  return true;
}

/* ---------- TAP driver ---------- */

int main(void) {
  int ntests = 8;
  printf("1..%d\n", ntests);

  int n = 1;
#define RUN(name, fn) do { \
  bool ok = (fn)(); \
  printf("%s %d - %s\n", ok ? "ok" : "not ok", n, name); \
  fflush(stdout); \
  n++; \
} while (0)

  RUN("execve",  test_execve);
  RUN("fexecve", test_fexecve);
  RUN("execv",   test_execv);
  RUN("execvp",  test_execvp);
  RUN("execvpe", test_execvpe);
  RUN("execl",   test_execl);
  RUN("execlp",  test_execlp);
  RUN("execle",  test_execle);

  return 0;
}
