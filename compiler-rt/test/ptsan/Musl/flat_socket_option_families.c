// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise the broad Linux socket-option allowlist without depending on
// privileges, optional protocols, or particular network devices. An invalid
// descriptor makes the kernel return EBADF after PTSan validates and strips
// each flat option buffer.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror %s -o %t
// RUN: %run %t
// RUN: %ptsan_musl_id32_strict -Wall -Wextra -Werror %s -o %t.id32
// RUN: %run %t.id32

#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>

enum {
  TestSolRaw = 255,
  TestSolPacket = 263,
  TestSolNetlink = 270,
};

struct OptionFamily {
  int level;
  const unsigned char *options;
  unsigned count;
};

static int check_option(int level, int option) {
  unsigned char storage[256] = {0};
  errno = 0;
  if (setsockopt(-1, level, option, storage, sizeof(storage)) != -1 ||
      errno != EBADF) {
    return 1;
  }

  socklen_t size = sizeof(storage);
  errno = 0;
  if (getsockopt(-1, level, option, storage, &size) != -1 || errno != EBADF) {
    return 2;
  }
  return 0;
}

int main(void) {
  // SO_ATTACH_FILTER (26) and SO_ATTACH_REUSEPORT_CBPF (51) contain nested
  // pointers and have their own regression. Undefined gaps stay fail-closed.
  static const unsigned char socket_options[] = {
      1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
      16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 27, 28, 29, 30, 31,
      32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46,
      47, 48, 49, 50, 52, 53, 55, 56, 57, 59, 60, 61, 62, 63, 64,
      65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77,
  };
  // IP_PKTOPTIONS (9) can expose a pointer-bearing msghdr and is excluded.
  static const unsigned char ip_options[] = {
      1,  2,  3,  4,  5,  6,  7,  8,  10, 11, 12, 13, 14, 15, 16, 17,
      18, 19, 20, 21, 22, 23, 24, 25, 26, 32, 33, 34, 35, 36, 37, 38,
      39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52,
  };
  // IPV6_2292PKTOPTIONS (6) likewise uses a pointer-bearing msghdr.
  static const unsigned char ipv6_options[] = {
      1,  2,  3,  4,  5,  7,  8,  9,  10, 11, 16, 17, 18, 19, 20, 21,
      22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 42,
      43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58,
      59, 60, 61, 62, 66, 67, 70, 72, 73, 74, 75, 76, 77, 78,
  };
  // TCP_ZEROCOPY_RECEIVE (35) changes userspace mappings and remains special.
  static const unsigned char tcp_options[] = {
      1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
      15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28,
      29, 30, 31, 32, 33, 34, 36, 37, 38, 39, 40, 41, 42,
  };
  static const unsigned char udp_options[] = {1, 100, 101, 102, 103, 104};
  static const unsigned char packet_options[] = {
      1,  2,  3,  5,  6,  7,  8,  9,  10, 11, 12, 13,
      14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
  };
  static const unsigned char netlink_options[] = {
      1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
  };
  static const unsigned char filter_option[] = {1};

  const struct OptionFamily families[] = {
      {SOL_SOCKET, socket_options, sizeof(socket_options)},
      {IPPROTO_IP, ip_options, sizeof(ip_options)},
      {IPPROTO_IPV6, ipv6_options, sizeof(ipv6_options)},
      {IPPROTO_TCP, tcp_options, sizeof(tcp_options)},
      {IPPROTO_UDP, udp_options, sizeof(udp_options)},
      {TestSolPacket, packet_options, sizeof(packet_options)},
      {TestSolNetlink, netlink_options, sizeof(netlink_options)},
      {IPPROTO_ICMPV6, filter_option, sizeof(filter_option)},
      {TestSolRaw, filter_option, sizeof(filter_option)},
  };

  for (unsigned family = 0; family < sizeof(families) / sizeof(families[0]);
       ++family) {
    for (unsigned option = 0; option < families[family].count; ++option) {
      int result = check_option(families[family].level,
                                families[family].options[option]);
      if (result) {
        return 10 * (family + 1) + result;
      }
    }
  }
  return 0;
}
