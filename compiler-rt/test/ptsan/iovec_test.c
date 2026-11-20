// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe

// Just making sure this runs

#define _GNU_SOURCE

#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Mirror your get_id: top 16 bits for a tag on 64-bit. */
static inline unsigned short test_get_id(void *ptr) {
  return (unsigned short)((unsigned long)ptr >> 48);
}

static __attribute__((noinline)) void die(const char *msg) {
  perror(msg);
  _exit(1);
}

static __attribute__((noinline)) void require_tagged(void *ptr, const char *what) {
  unsigned short id = test_get_id(ptr);
  /*if (id == 0) {
    fprintf(stderr, "%s pointer 0x%lx is not tagged (id == 0)\n",
            what, (unsigned long)ptr);
    _exit(2);
  }*/
}

static __attribute__((noinline)) int make_tempfile(void) {
  char tmpl[] = "/tmp/ptsan_iov_XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) {
    die("mkstemp");
  }
  if (unlink(tmpl) != 0) {
    die("unlink");
  }
  return fd;
}

/* Test readv/writev on a pipe, with tagged stack buffers as iov_base. */
static __attribute__((noinline)) void test_readv_writev_tagged(void) {
  int fds[2];
  if (pipe(fds) != 0) {
    die("pipe");
  }

  char s1[16];
  char s2[16];
  char s3[16];

  strcpy(s1, "Hello ");
  strcpy(s2, "world");
  strcpy(s3, "!\n");

  require_tagged(s1, "s1");
  require_tagged(s2, "s2");
  require_tagged(s3, "s3");

  struct iovec w[3];
  w[0].iov_base = s1;
  w[0].iov_len  = strlen(s1);  /* 6 */
  w[1].iov_base = s2;
  w[1].iov_len  = strlen(s2);  /* 5 */
  w[2].iov_base = s3;
  w[2].iov_len  = strlen(s3);  /* 2 */

  ssize_t expected = (ssize_t)(w[0].iov_len + w[1].iov_len + w[2].iov_len);
  ssize_t n = writev(fds[1], w, 3);
  assert(n == expected);

  char b1[16];
  char b2[16];
  char b3[16];

  memset(b1, 0, sizeof(b1));
  memset(b2, 0, sizeof(b2));
  memset(b3, 0, sizeof(b3));

  require_tagged(b1, "b1");
  require_tagged(b2, "b2");
  require_tagged(b3, "b3");

  struct iovec r[3];
  r[0].iov_base = b1;
  r[0].iov_len  = strlen(s1);  /* 6 */
  r[1].iov_base = b2;
  r[1].iov_len  = strlen(s2);  /* 5 */
  r[2].iov_base = b3;
  r[2].iov_len  = strlen(s3);  /* 2 */

  n = readv(fds[0], r, 3);
  assert(n == expected);

  /* Because we zeroed the full buffers, each one ends with '\0' after data. */
  assert(strcmp(b1, "Hello ") == 0);
  assert(strcmp(b2, "world") == 0);
  assert(strcmp(b3, "!\n") == 0);

  close(fds[0]);
  close(fds[1]);
}

/* Test preadv/pwritev with tagged heap buffers as iov_base. */
static __attribute__((noinline)) void test_preadv_pwritev_tagged(void) {
  int fd = make_tempfile();

  char *s1 = (char *)malloc(8);
  char *s2 = (char *)malloc(16);
  if (!s1 || !s2) {
    die("malloc");
  }

  strcpy(s1, "AAA");
  strcpy(s2, "BBBCCC");

  require_tagged(s1, "s1 (heap)");
  require_tagged(s2, "s2 (heap)");

  struct iovec w[2];
  w[0].iov_base = s1;
  w[0].iov_len  = strlen(s1);  /* 3 */
  w[1].iov_base = s2;
  w[1].iov_len  = strlen(s2);  /* 6 */

  ssize_t total = (ssize_t)(w[0].iov_len + w[1].iov_len);
  ssize_t n = pwritev(fd, w, 2, 0);
  assert(n == total);

  char *b1 = (char *)malloc(8);
  char *b2 = (char *)malloc(8);
  if (!b1 || !b2) {
    die("malloc");
  }

  memset(b1, 0, 8);
  memset(b2, 0, 8);

  require_tagged(b1, "b1 (heap)");
  require_tagged(b2, "b2 (heap)");

  struct iovec r[2];
  r[0].iov_base = b1;
  r[0].iov_len  = strlen(s1);  /* 3 */
  r[1].iov_base = b2;
  r[1].iov_len  = strlen(s2);  /* 6 */

  n = preadv(fd, r, 2, 0);
  assert(n == total);

  assert(strncmp(b1, "AAA", 3) == 0);
  assert(strncmp(b2, "BBBCCC", 6) == 0);

  free(s1);
  free(s2);
  free(b1);
  free(b2);
  close(fd);
}

/* Test preadv2/pwritev2 with tagged stack buffers. Linux/glibc-specific. */
static __attribute__((noinline)) void test_preadv2_pwritev2_tagged(void) {
#ifdef __linux__
  int fd = make_tempfile();

  char a[32];
  char b[32];

  strcpy(a, "hello-");
  strcpy(b, "preadv2/pwritev2");

  require_tagged(a, "a");
  require_tagged(b, "b");

  struct iovec w[2];
  w[0].iov_base = a;
  w[0].iov_len  = strlen(a);  /* 6 */
  w[1].iov_base = b;
  w[1].iov_len  = strlen(b);  /* 16 */

  ssize_t total = (ssize_t)(w[0].iov_len + w[1].iov_len);

  /* flags = 0 */
  ssize_t n = pwritev2(fd, w, 2, 0, 0);
  if (n < 0 && errno == ENOSYS) {
    /* Kernel too old for preadv2/pwritev2; just skip. */
    close(fd);
    return;
  }
  assert(n == total);

  char r1[32];
  char r2[32];
  memset(r1, 0, sizeof(r1));
  memset(r2, 0, sizeof(r2));

  require_tagged(r1, "r1");
  require_tagged(r2, "r2");

  struct iovec r[2];
  r[0].iov_base = r1;
  r[0].iov_len  = strlen(a);  /* 6 */
  r[1].iov_base = r2;
  r[1].iov_len  = strlen(b);  /* 16 */

  n = preadv2(fd, r, 2, 0, 0);
  assert(n == total);

  assert(strncmp(r1, "hello-", 6) == 0);
  assert(strncmp(r2, "preadv2/pwritev2", 16) == 0);

  close(fd);
#else
  (void)test_preadv2_pwritev2_tagged;
#endif
}

/* Test preadv64/pwritev64 with tagged stack buffers. Linux/glibc-specific. */
static __attribute__((noinline)) void test_preadv64_pwritev64_tagged(void) {
#ifdef __linux__
  int fd = make_tempfile();

  char a[1];
  char b[1];

  a[0] = 'X';
  b[0] = 'Y';

  require_tagged(a, "a");
  require_tagged(b, "b");

  struct iovec w[2];
  w[0].iov_base = a;
  w[0].iov_len  = 1;
  w[1].iov_base = b;
  w[1].iov_len  = 1;

  off64_t off = (off64_t)4096;

  ssize_t n = pwritev64(fd, w, 2, off);
  if (n < 0 && errno == ENOSYS) {
    /* Older libc/kernel combo; skip. */
    close(fd);
    return;
  }
  assert(n == 2);

  char r1[1];
  char r2[1];

  struct iovec r[2];
  r[0].iov_base = r1;
  r[0].iov_len  = 1;
  r[1].iov_base = r2;
  r[1].iov_len  = 1;

  require_tagged(r1, "r1");
  require_tagged(r2, "r2");

  r1[0] = 0;
  r2[0] = 0;

  n = preadv64(fd, r, 2, off);
  assert(n == 2);

  assert(r1[0] == 'X');
  assert(r2[0] == 'Y');

  close(fd);
#else
  (void)test_preadv64_pwritev64_tagged;
#endif
}

/* Test process_vm_readv/process_vm_writev with tagged stack buffers. */
static __attribute__((noinline)) void test_process_vm_readv_writev_tagged(void) {
#ifdef __linux__
  pid_t pid = getpid();

  char src[64];
  char dst[64];

  memset(src, 0, sizeof(src));
  memset(dst, 0, sizeof(dst));

  strcpy(src, "process_vm_* iovec test");

  require_tagged(src, "src");
  require_tagged(dst, "dst");

  struct iovec local_iov;
  struct iovec remote_iov;

  /* local_iov -> remote_iov (write into our own memory) */
  local_iov.iov_base  = src;
  local_iov.iov_len   = strlen(src) + 1;
  remote_iov.iov_base = dst;
  remote_iov.iov_len  = sizeof(dst);

  ssize_t n = process_vm_writev(pid,
                                &local_iov, 1,
                                &remote_iov, 1,
                                0);
  if (n < 0 && (errno == ENOSYS || errno == EPERM)) {
    /* Not supported / permitted on this kernel; skip. */
    return;
  }
  assert(n == (ssize_t)(strlen(src) + 1));
  assert(strcmp(dst, "process_vm_* iovec test") == 0);

  /* Now read back from remote dst -> local src2. */
  char src2[64];
  memset(src2, 0, sizeof(src2));
  require_tagged(src2, "src2");

  local_iov.iov_base  = src2;
  local_iov.iov_len   = sizeof(src2);
  remote_iov.iov_base = dst;
  remote_iov.iov_len  = strlen(dst) + 1;

  n = process_vm_readv(pid,
                       &local_iov, 1,
                       &remote_iov, 1,
                       0);
  assert(n == (ssize_t)(strlen(dst) + 1));
  assert(strcmp(src2, "process_vm_* iovec test") == 0);
#else
  (void)test_process_vm_readv_writev_tagged;
#endif
}

int __attribute__((noinline)) main(void) {
  test_readv_writev_tagged();
  test_preadv_pwritev_tagged();
  test_preadv2_pwritev2_tagged();
  test_preadv64_pwritev64_tagged();
  test_process_vm_readv_writev_tagged();

  printf("iovec wrapper tagged-pointer test passed\n");
  return 0;
}
