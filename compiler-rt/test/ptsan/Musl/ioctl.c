//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise common flat ioctl requests through public musl APIs.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror -o %t %s
// RUN: %run %t good
// RUN: not %run %t bad-termios 2> %t.termios
// RUN: FileCheck %s < %t.termios
// RUN: not %run %t bad-winsize 2> %t.winsize
// RUN: FileCheck %s < %t.winsize
// RUN: not %run %t bad-int 2> %t.int
// RUN: FileCheck %s < %t.int
// RUN: not %run %t bad-ifreq 2> %t.ifreq
// RUN: FileCheck %s < %t.ifreq
// RUN: not %run %t bad-ethtool 2> %t.ethtool
// RUN: FileCheck %s < %t.ethtool
// RUN: not %run %t bad-hwtstamp 2> %t.hwtstamp
// RUN: FileCheck %s < %t.hwtstamp
//
// CHECK: Found bad

#define _GNU_SOURCE

#include <fcntl.h>
#include <net/if.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

// Stable Linux UAPI values and layouts that are not installed with musl's
// libc headers.
#define SIOCETHTOOL 0x8946
#define ETHTOOL_GLINK 0x0000000a
#define ETHTOOL_GTSO 0x0000001e
#define ETHTOOL_GUFO 0x00000021
#define ETHTOOL_GGSO 0x00000023
#define ETHTOOL_GFLAGS 0x00000025
#define ETHTOOL_GGRO 0x0000002b
#define ETHTOOL_GET_TS_INFO 0x00000041
#define SIOCSHWTSTAMP 0x89b0
#define SIOCGHWTSTAMP 0x89b1

struct TestEthtoolValue {
  uint32_t command;
  uint32_t data;
};

struct TestEthtoolTimestampInfo {
  uint32_t command;
  uint32_t so_timestamping;
  int32_t phc_index;
  uint32_t tx_types;
  uint32_t tx_reserved[3];
  uint32_t rx_filters;
  uint32_t rx_reserved[3];
};

struct TestHardwareTimestampConfig {
  int flags;
  int tx_type;
  int rx_filter;
};

static int openPty(int *master, int *slave) {
  *master = posix_openpt(O_RDWR | O_NOCTTY);
  if (*master < 0) {
    return -1;
  }
  if (grantpt(*master) != 0 || unlockpt(*master) != 0) {
    close(*master);
    return -1;
  }

  char *slave_name = ptsname(*master);
  if (!slave_name) {
    close(*master);
    return -1;
  }
  *slave = open(slave_name, O_RDWR | O_NOCTTY);
  if (*slave < 0) {
    close(*master);
    return -1;
  }
  return 0;
}

static int testGood(void) {
  int master;
  int slave;
  if (openPty(&master, &slave) != 0) {
    return 1;
  }

  struct termios settings;
  if (tcgetattr(slave, &settings) != 0 ||
      tcsetattr(slave, TCSANOW, &settings) != 0) {
    return 2;
  }

  struct winsize window;
  if (ioctl(slave, TIOCGWINSZ, &window) != 0 ||
      ioctl(slave, TIOCSWINSZ, &window) != 0) {
    return 3;
  }

  int value = 0;
  if (ioctl(master, FIONREAD, &value) != 0 ||
      ioctl(master, FIONBIO, &value) != 0) {
    return 4;
  }

  unsigned pty_number = 0;
  if (ioctl(master, TIOCGPTN, &pty_number) != 0 ||
      ioctl(master, TIOCSPTLCK, &value) != 0) {
    return 5;
  }

  (void)ioctl(slave, TIOCGPGRP, &value);
  (void)ioctl(slave, TIOCSPGRP, &value);
  if (ioctl(master, FIOCLEX, 0) != 0 || ioctl(master, FIONCLEX, 0) != 0) {
    return 6;
  }

  unsigned interface_index = if_nametoindex("lo");
  if (!interface_index) {
    return 7;
  }
  char interface_name[IF_NAMESIZE];
  if (!if_indextoname(interface_index, interface_name)) {
    return 8;
  }

  struct ifreq interface;
  memset(&interface, 0, sizeof(interface));
  memcpy(interface.ifr_name, "lo", 3);
  (void)ioctl(-1, SIOCGIFADDR, &interface);
  (void)ioctl(-1, SIOCGIFNETMASK, &interface);
  (void)ioctl(-1, SIOCGIFFLAGS, &interface);
  (void)ioctl(-1, SIOCSIFFLAGS, &interface);
  (void)ioctl(-1, SIOCGIFMTU, &interface);
  (void)ioctl(-1, SIOCGIFHWADDR, &interface);

  struct TestEthtoolValue ethtool_value;
  memset(&ethtool_value, 0, sizeof(ethtool_value));
  interface.ifr_data = (char *)&ethtool_value;
  const uint32_t value_commands[] = {
      ETHTOOL_GLINK, ETHTOOL_GTSO,   ETHTOOL_GUFO,
      ETHTOOL_GGSO,  ETHTOOL_GFLAGS, ETHTOOL_GGRO,
  };
  for (size_t index = 0;
       index != sizeof(value_commands) / sizeof(value_commands[0]); ++index) {
    ethtool_value.command = value_commands[index];
    (void)ioctl(-1, SIOCETHTOOL, &interface);
    if (interface.ifr_data != (char *)&ethtool_value) {
      return 9;
    }
  }

  struct TestEthtoolTimestampInfo timestamp_info;
  memset(&timestamp_info, 0, sizeof(timestamp_info));
  timestamp_info.command = ETHTOOL_GET_TS_INFO;
  interface.ifr_data = (char *)&timestamp_info;
  (void)ioctl(-1, SIOCETHTOOL, &interface);
  if (interface.ifr_data != (char *)&timestamp_info) {
    return 10;
  }

  struct TestHardwareTimestampConfig hardware_timestamp = {0, 0, 0};
  interface.ifr_data = (char *)&hardware_timestamp;
  (void)ioctl(-1, SIOCSHWTSTAMP, &interface);
  (void)ioctl(-1, SIOCGHWTSTAMP, &interface);
  if (interface.ifr_data != (char *)&hardware_timestamp) {
    return 11;
  }

  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    return 12;
  }
  if (sockatmark(sockets[0]) < 0) {
    return 13;
  }

  close(sockets[0]);
  close(sockets[1]);
  close(slave);
  close(master);
  return 0;
}

static int testBadTermios(void) {
  int master;
  int slave;
  if (openPty(&master, &slave) != 0) {
    return 1;
  }
  void *storage = malloc(sizeof(struct termios) - 1);
  if (!storage) {
    return 2;
  }
  (void)tcgetattr(slave, (struct termios *)storage);
  return 0;
}

static int testBadWinsize(void) {
  int master;
  int slave;
  if (openPty(&master, &slave) != 0) {
    return 1;
  }
  void *storage = malloc(sizeof(struct winsize) - 1);
  if (!storage) {
    return 2;
  }
  (void)ioctl(slave, TIOCGWINSZ, storage);
  return 0;
}

static int testBadInt(void) {
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    return 1;
  }
  void *storage = malloc(sizeof(int) - 1);
  if (!storage) {
    return 2;
  }
  (void)ioctl(sockets[0], SIOCATMARK, storage);
  return 0;
}

static int testBadIfreq(void) {
  int socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
  if (socket_fd < 0) {
    return 1;
  }
  void *storage = malloc(sizeof(struct ifreq) - 1);
  if (!storage) {
    return 2;
  }
  memset(storage, 0, sizeof(struct ifreq) - 1);
  (void)ioctl(socket_fd, SIOCGIFINDEX, storage);
  return 0;
}

static int testBadEthtool(void) {
  struct ifreq interface;
  memset(&interface, 0, sizeof(interface));
  void *storage = malloc(sizeof(struct TestEthtoolValue) - 1);
  if (!storage) {
    return 1;
  }
  uint32_t command = ETHTOOL_GLINK;
  memcpy(storage, &command, sizeof(command));
  interface.ifr_data = storage;
  (void)ioctl(-1, SIOCETHTOOL, &interface);
  return 0;
}

static int testBadHardwareTimestamp(void) {
  struct ifreq interface;
  memset(&interface, 0, sizeof(interface));
  interface.ifr_data =
      malloc(sizeof(struct TestHardwareTimestampConfig) - 1);
  if (!interface.ifr_data) {
    return 1;
  }
  (void)ioctl(-1, SIOCSHWTSTAMP, &interface);
  return 0;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (strcmp(argv[1], "good") == 0) {
    return testGood();
  }
  if (strcmp(argv[1], "bad-termios") == 0) {
    return testBadTermios();
  }
  if (strcmp(argv[1], "bad-winsize") == 0) {
    return testBadWinsize();
  }
  if (strcmp(argv[1], "bad-int") == 0) {
    return testBadInt();
  }
  if (strcmp(argv[1], "bad-ifreq") == 0) {
    return testBadIfreq();
  }
  if (strcmp(argv[1], "bad-ethtool") == 0) {
    return testBadEthtool();
  }
  if (strcmp(argv[1], "bad-hwtstamp") == 0) {
    return testBadHardwareTimestamp();
  }
  return 1;
}
