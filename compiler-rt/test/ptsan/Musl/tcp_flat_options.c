//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror %s -o %t
// RUN: %run %t good
// RUN: not %run %t bad-inq-set 2>&1 | FileCheck %s
// RUN: not %run %t bad-inq-get 2>&1 | FileCheck %s
// RUN: not %run %t bad-info 2>&1 | FileCheck %s
// RUN: not %run %t bad-congestion 2>&1 | FileCheck %s
// CHECK: Found bad

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void checkIntegers(int fd) {
  const struct {
    int option;
    int value;
  } options[] = {
      {TCP_INQ, 1}, {TCP_MAXSEG, 1200}, {TCP_CORK, 1}, {TCP_QUICKACK, 1},
      {TCP_KEEPIDLE, 60}, {TCP_KEEPINTVL, 10}, {TCP_KEEPCNT, 3},
      {TCP_SYNCNT, 3}, {TCP_LINGER2, 30}, {TCP_DEFER_ACCEPT, 1},
      {TCP_WINDOW_CLAMP, 4096}, {TCP_NOTSENT_LOWAT, 1024},
      {TCP_FASTOPEN, 5}, {TCP_FASTOPEN_CONNECT, 1},
      {TCP_FASTOPEN_NO_COOKIE, 1}, {TCP_TX_DELAY, 100},
  };
  for (unsigned i = 0; i < sizeof(options) / sizeof(options[0]); ++i) {
    int result = setsockopt(fd, IPPROTO_TCP, options[i].option,
                            &options[i].value, sizeof(int));
    // Client Fast Open can be disabled by the host's TCP configuration.
    if (options[i].option == TCP_FASTOPEN_CONNECT && result == -1 &&
        errno == EOPNOTSUPP) {
      continue;
    }
    assert(result == 0);
    int observed = -1;
    socklen_t size = sizeof(observed);
    assert(getsockopt(fd, IPPROTO_TCP, options[i].option, &observed, &size) == 0);
    assert(size == sizeof(observed));
    // The kernel may clamp or normalize options (for example TCP_MAXSEG).
    // Exercise the option buffers without requiring an exact value round trip.
  }
}

static void checkBuffers(int fd) {
  char algorithm[64] = {0};
  socklen_t size = sizeof(algorithm);
  assert(getsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, algorithm, &size) == 0);
  assert(size > 0 && size <= sizeof(algorithm));
  assert(algorithm[0] != 0 && memchr(algorithm, 0, size) != NULL);
  assert(setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, algorithm,
                    strlen(algorithm)) == 0);

  struct tcp_info info = {0};
  size = sizeof(info);
  assert(getsockopt(fd, IPPROTO_TCP, TCP_INFO, &info, &size) == 0);
  assert(size > 0 && size <= sizeof(info));
  assert(info.tcpi_state == TCP_CLOSE);

  // Short output buffers are legal: the kernel truncates its response.
  unsigned char state = 0;
  size = sizeof(state);
  assert(getsockopt(fd, IPPROTO_TCP, TCP_INFO, &state, &size) == 0);
  assert(size == sizeof(state) && state == TCP_CLOSE);

  unsigned char cc_info[256] = {0};
  size = sizeof(cc_info);
  assert(getsockopt(fd, IPPROTO_TCP, TCP_CC_INFO, cc_info, &size) == 0);
  // The selected congestion-control algorithm may provide no statistics.
  assert(size <= sizeof(cc_info));
}

int main(int argc, char **argv) {
  assert(argc == 2);
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  assert(fd >= 0);
  if (!strcmp(argv[1], "good")) {
    checkIntegers(fd);
    checkBuffers(fd);
  } else {
    char *small = calloc(1, 1);
    assert(small != NULL);
    if (!strcmp(argv[1], "bad-inq-set")) {
      (void)setsockopt(fd, IPPROTO_TCP, TCP_INQ, small, sizeof(int));
    } else if (!strcmp(argv[1], "bad-inq-get")) {
      socklen_t size = sizeof(int);
      (void)getsockopt(fd, IPPROTO_TCP, TCP_INQ, small, &size);
    } else if (!strcmp(argv[1], "bad-info")) {
      socklen_t size = sizeof(struct tcp_info);
      (void)getsockopt(fd, IPPROTO_TCP, TCP_INFO, small, &size);
    } else {
      assert(!strcmp(argv[1], "bad-congestion"));
      (void)setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, small, 2);
    }
    assert(0 && "expected a PTSan complaint for the undersized buffer");
  }
  assert(close(fd) == 0);
  return 0;
}
