//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: for operation in accept accept4 recvfrom; do \
// RUN:   %run %t $operation good && %run %t $operation omitted || exit 1; \
// RUN:   not %run %t $operation bad-address 2>&1 | FileCheck %s || exit 1; \
// RUN:   not %run %t $operation bad-length 2>&1 | FileCheck %s || exit 1; \
// RUN: done
// CHECK: Found bad

// Peer-address outputs must be checked and decoded exactly once. Decoding a
// native address again corrupts it in markerless strict mode.
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
  assert(argc == 3);
  int udp = !strcmp(argv[1], "recvfrom");
  int use_accept4 = !strcmp(argv[1], "accept4");
  assert(udp || use_accept4 || !strcmp(argv[1], "accept"));
  int omitted = !strcmp(argv[2], "omitted");
  int bad_address = !strcmp(argv[2], "bad-address");
  int bad_length = !strcmp(argv[2], "bad-length");
  assert(omitted || bad_address || bad_length || !strcmp(argv[2], "good"));

  int listener = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
  int client = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
  assert(listener >= 0 && client >= 0);
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
  socklen_t length = sizeof(address);
  assert(getsockname(listener, (struct sockaddr *)&address, &length) == 0);
  if (!udp)
    assert(listen(listener, 1) == 0);
  assert(connect(client, (struct sockaddr *)&address, length) == 0);

  struct sockaddr_in *peer = NULL;
  socklen_t *peer_length = NULL;
  if (!omitted) {
    peer = malloc(bad_address ? 1 : sizeof(*peer));
    peer_length = malloc(bad_length ? 1 : sizeof(*peer_length));
    assert(peer && peer_length);
    if (!bad_length)
      *peer_length = sizeof(*peer);
  }

  int result;
  if (udp) {
    char byte = 'x';
    assert(send(client, &byte, 1, 0) == 1);
    byte = 0;
    result = recvfrom(listener, &byte, 1, 0,
                      (struct sockaddr *)peer, peer_length);
    assert(result == 1 && byte == 'x');
  } else if (use_accept4) {
    result = accept4(listener, (struct sockaddr *)peer, peer_length,
                     SOCK_CLOEXEC);
    assert(result >= 0);
    assert((fcntl(result, F_GETFD) & FD_CLOEXEC) != 0);
  } else {
    result = accept(listener, (struct sockaddr *)peer, peer_length);
    assert(result >= 0);
  }
  assert(!bad_address && !bad_length && "missing bounds complaint");
  if (!omitted) {
    assert(*peer_length == sizeof(*peer));
    assert(peer->sin_family == AF_INET);
    assert(peer->sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    assert(peer->sin_port != 0);
  }
  if (!udp)
    assert(close(result) == 0);
  assert(close(client) == 0);
  assert(close(listener) == 0);
  free(peer_length);
  free(peer);
  return 0;
}
