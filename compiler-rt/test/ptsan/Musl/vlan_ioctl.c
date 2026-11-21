// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror %s -o %t
// RUN: %run %t good
// RUN: not %run %t bad-buffer 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict -Wall -Wextra -Werror %s -o %t.id32
// RUN: %run %t.id32 good
// RUN: not %run %t.id32 bad-buffer 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_nonstrict %s -o %t.ns
// RUN: %run %t.ns good
// RUN: not %run %t.ns bad-buffer 2>&1 | FileCheck %s
// CHECK: Found bad

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

// Linux UAPI layout: the musl test sysroot does not include kernel headers.
#define TEST_SIOCGIFVLAN 0x8982
enum { GET_VLAN_REALDEV_NAME_CMD = 8, GET_VLAN_VID_CMD = 9 };
struct TestVlanIoctlArgs {
  int cmd;
  char device1[24];
  union {
    char device2[24];
    int VID;
    unsigned int skb_priority, name_type, bind_type, flag;
  } u;
  short vlan_qos;
};

int main(int argc, char **argv) {
  if (argc < 2) {
    return 1;
  }
  if (!strcmp(argv[1], "bad-buffer")) {
    void *short_buffer = malloc(sizeof(struct TestVlanIoctlArgs) - 1);
    if (!short_buffer) {
      return 2;
    }
    // Bounds must be checked even when the kernel would reject the fd.
    return ioctl(-1, TEST_SIOCGIFVLAN, short_buffer);
  }
  struct TestVlanIoctlArgs args = {0};
  args.cmd = GET_VLAN_VID_CMD;
  errno = 0;
  if (ioctl(-1, TEST_SIOCGIFVLAN, &args) != -1 || errno != EBADF) {
    return 3;
  }
  if (!strcmp(argv[1], "good")) {
    return 0;
  }

  // Optional integration test: query <vlan> <vid> <parent> in a disposable
  // namespace. No privileges or interface setup are required by lit tests.
  if (strcmp(argv[1], "query") || argc != 5) {
    return 4;
  }
  if (strlen(argv[2]) >= sizeof(args.device1)) {
    return 5;
  }
  strcpy(args.device1, argv[2]);
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return 6;
  }
  if (ioctl(fd, TEST_SIOCGIFVLAN, &args) || args.u.VID != atoi(argv[3])) {
    return 7;
  }
  args.cmd = GET_VLAN_REALDEV_NAME_CMD;
  if (ioctl(fd, TEST_SIOCGIFVLAN, &args) || strcmp(args.u.device2, argv[4])) {
    return 8;
  }
  return close(fd);
}
