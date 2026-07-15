// RUN: rm -rf %t && mkdir -p %t && cd %t
// RUN: %clang_ptsan -g -O2 -o %exe %s
// RUN: ./%exe 2> err.txt 1> out.txt
// RUN: cat err.txt | FileCheck %s --check-prefix=ERR
// RUN: cat out.txt | FileCheck %s --check-prefix=OUT

// ERR: Unreclaimed IDs at exit: 0

// OUT: uv__nonblock_ioctl tests passed

#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#define UV__ERR(x) (-(x))

__attribute__((noinline))
int uv__nonblock_ioctl(int fd, int set) {
  int r;

  do {
    r = ioctl(fd, FIONBIO, &set);
  } while (r == -1 && errno == EINTR);

  if (r) {
    return UV__ERR(errno);
  }

  return 0;
}

__attribute__((noinline))
static void test_uv_nonblock_ioctl_success(void) {
  int fds[2];
  int r;

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    perror("socketpair");
    exit(1);
  }

  int fd = fds[0];

  /* Initially, fd should be blocking (O_NONBLOCK not set). */
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    perror("fcntl(F_GETFL) initial");
    exit(1);
  }

  /* Set non-blocking. */
  r = uv__nonblock_ioctl(fd, 1);
  if (r != 0) {
    fprintf(stderr, "uv__nonblock_ioctl(fd, 1) failed: %d (errno=%d)\n", r, errno);
    exit(1);
  }

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    perror("fcntl(F_GETFL) after set");
    exit(1);
  }

  /* Verify that O_NONBLOCK is set. */
  assert((flags & O_NONBLOCK) != 0);

  /* Clear non-blocking. */
  r = uv__nonblock_ioctl(fd, 0);
  if (r != 0) {
    fprintf(stderr, "uv__nonblock_ioctl(fd, 0) failed: %d (errno=%d)\n", r, errno);
    exit(1);
  }

  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1) {
    perror("fcntl(F_GETFL) after clear");
    exit(1);
  }

  /* Verify that O_NONBLOCK is cleared. */
  assert((flags & O_NONBLOCK) == 0);

  close(fds[0]);
  close(fds[1]);
}

__attribute__((noinline))
static void test_uv_nonblock_ioctl_bad_fd(void) {
  int r = uv__nonblock_ioctl(-1, 1);

  /* Should fail and return a negative errno. */
  assert(r < 0);

  /* Check that it really is -EBADF (what ioctl(-1,...) should return). */
  assert(r == -EBADF);
}

__attribute__((noinline))
int main(void) {
  test_uv_nonblock_ioctl_success();
  test_uv_nonblock_ioctl_bad_fd();
  puts("uv__nonblock_ioctl tests passed");
  return 0;
}
