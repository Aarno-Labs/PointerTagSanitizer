// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Exercise flat network ioctl families and SIOCGIFCONF's nested output buffer.
// Invalid descriptors make the flat cases independent of privileges and host
// interface configuration.
//
// RUN: %ptsan_musl_strict -Wall -Wextra -Werror %s -o %t
// RUN: %run %t good
// RUN: not %run %t bad-ifconf 2>&1 | FileCheck %s
// RUN: not %run %t bad-arp 2>&1 | FileCheck %s
// RUN: %ptsan_musl_id32_strict -Wall -Wextra -Werror %s -o %t.id32
// RUN: %run %t.id32 good
// RUN: not %run %t.id32 bad-ifconf-buffer 2>&1 | FileCheck %s
// CHECK: Found bad

#include <errno.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
  TestSIOCGIFNAME = 0x8910,
  TestSIOCSIFLINK = 0x8911,
  TestSIOCGIFCONF = 0x8912,
  TestSIOCGIFFLAGS = 0x8913,
  TestSIOCSIFFLAGS = 0x8914,
  TestSIOCGIFADDR = 0x8915,
  TestSIOCSIFADDR = 0x8916,
  TestSIOCGIFDSTADDR = 0x8917,
  TestSIOCSIFDSTADDR = 0x8918,
  TestSIOCGIFBRDADDR = 0x8919,
  TestSIOCSIFBRDADDR = 0x891a,
  TestSIOCGIFNETMASK = 0x891b,
  TestSIOCSIFNETMASK = 0x891c,
  TestSIOCGIFMETRIC = 0x891d,
  TestSIOCSIFMETRIC = 0x891e,
  TestSIOCGIFMEM = 0x891f,
  TestSIOCSIFMEM = 0x8920,
  TestSIOCGIFMTU = 0x8921,
  TestSIOCSIFMTU = 0x8922,
  TestSIOCSIFNAME = 0x8923,
  TestSIOCSIFHWADDR = 0x8924,
  TestSIOCGIFENCAP = 0x8925,
  TestSIOCSIFENCAP = 0x8926,
  TestSIOCGIFHWADDR = 0x8927,
  TestSIOCGIFSLAVE = 0x8929,
  TestSIOCSIFSLAVE = 0x8930,
  TestSIOCADDMULTI = 0x8931,
  TestSIOCDELMULTI = 0x8932,
  TestSIOCGIFINDEX = 0x8933,
  TestSIOCSIFPFLAGS = 0x8934,
  TestSIOCGIFPFLAGS = 0x8935,
  TestSIOCSIFHWBROADCAST = 0x8937,
  TestSIOCGIFTXQLEN = 0x8942,
  TestSIOCSIFTXQLEN = 0x8943,
  TestSIOCDARP = 0x8953,
  TestSIOCGARP = 0x8954,
  TestSIOCSARP = 0x8955,
  TestSIOCDRARP = 0x8960,
  TestSIOCGRARP = 0x8961,
  TestSIOCSRARP = 0x8962,
  TestSIOCGIFMAP = 0x8970,
  TestSIOCSIFMAP = 0x8971,
};

static int expect_bad_fd(unsigned long request, void *argument) {
  errno = 0;
  return ioctl(-1, request, argument) == -1 && errno == EBADF ? 0 : 1;
}

static int test_good(void) {
  static const unsigned short ifreq_requests[] = {
      TestSIOCGIFNAME,        TestSIOCSIFLINK,    TestSIOCGIFFLAGS,
      TestSIOCSIFFLAGS,       TestSIOCGIFADDR,    TestSIOCSIFADDR,
      TestSIOCGIFDSTADDR,     TestSIOCSIFDSTADDR, TestSIOCGIFBRDADDR,
      TestSIOCSIFBRDADDR,     TestSIOCGIFNETMASK, TestSIOCSIFNETMASK,
      TestSIOCGIFMETRIC,      TestSIOCSIFMETRIC,  TestSIOCGIFMEM,
      TestSIOCSIFMEM,         TestSIOCGIFMTU,     TestSIOCSIFMTU,
      TestSIOCSIFNAME,        TestSIOCSIFHWADDR,  TestSIOCGIFENCAP,
      TestSIOCSIFENCAP,       TestSIOCGIFHWADDR,  TestSIOCGIFSLAVE,
      TestSIOCSIFSLAVE,       TestSIOCADDMULTI,   TestSIOCDELMULTI,
      TestSIOCGIFINDEX,       TestSIOCSIFPFLAGS,  TestSIOCGIFPFLAGS,
      TestSIOCSIFHWBROADCAST, TestSIOCGIFTXQLEN,  TestSIOCSIFTXQLEN,
      TestSIOCGIFMAP,         TestSIOCSIFMAP,
  };
  struct ifreq interface;
  memset(&interface, 0, sizeof(interface));
  memcpy(interface.ifr_name, "lo", 3);
  for (unsigned i = 0; i < sizeof(ifreq_requests) / sizeof(ifreq_requests[0]);
       ++i) {
    if (expect_bad_fd(ifreq_requests[i], &interface)) {
      return 1;
    }
  }

  static const unsigned short arp_requests[] = {
      TestSIOCDARP,  TestSIOCGARP,  TestSIOCSARP,
      TestSIOCDRARP, TestSIOCGRARP, TestSIOCSRARP,
  };
  struct arpreq arp;
  memset(&arp, 0, sizeof(arp));
  for (unsigned i = 0; i < sizeof(arp_requests) / sizeof(arp_requests[0]);
       ++i) {
    if (expect_bad_fd(arp_requests[i], &arp)) {
      return 2;
    }
  }

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return 3;
  }
  char interfaces[4096];
  struct ifconf configuration = {
      .ifc_len = sizeof(interfaces),
      .ifc_buf = interfaces,
  };
  char *saved_buffer = configuration.ifc_buf;
  if (ioctl(fd, TestSIOCGIFCONF, &configuration) ||
      configuration.ifc_buf != saved_buffer || configuration.ifc_len < 0 ||
      configuration.ifc_len > (int)sizeof(interfaces)) {
    return 4;
  }
  return close(fd);
}

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  if (!strcmp(argv[1], "good")) {
    return test_good();
  }
  if (!strcmp(argv[1], "bad-ifconf")) {
    void *short_configuration = malloc(sizeof(struct ifconf) - 1);
    if (!short_configuration) {
      return 2;
    }
    return ioctl(-1, TestSIOCGIFCONF, short_configuration);
  }
  if (!strcmp(argv[1], "bad-ifconf-buffer")) {
    char *short_buffer = malloc(sizeof(struct ifreq) - 1);
    if (!short_buffer) {
      return 3;
    }
    struct ifconf configuration = {
        .ifc_len = sizeof(struct ifreq),
        .ifc_buf = short_buffer,
    };
    return ioctl(-1, TestSIOCGIFCONF, &configuration);
  }
  if (!strcmp(argv[1], "bad-arp")) {
    void *short_entry = malloc(sizeof(struct arpreq) - 1);
    if (!short_entry) {
      return 4;
    }
    return ioctl(-1, TestSIOCGARP, short_entry);
  }
  return 5;
}
