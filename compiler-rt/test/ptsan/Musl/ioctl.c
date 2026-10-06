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
//
// CHECK: Found bad

#define _GNU_SOURCE

#include <fcntl.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

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

  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    return 9;
  }
  if (sockatmark(sockets[0]) < 0) {
    return 10;
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
  return 1;
}
