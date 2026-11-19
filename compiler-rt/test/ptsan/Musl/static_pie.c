//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -static-pie -o %t %s
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// CHECK: Found bad POINT access

#define _GNU_SOURCE
#include "../Inputs/ptsan_test_interface.h"
#include <elf.h>
#include <link.h>
#include <stdlib.h>
#include <sys/auxv.h>

static volatile int global[] = {17, 29};

static int checkMainExecutable(struct dl_phdr_info *info, size_t size,
                               void *data) {
  (void)size;
  (void)data;
  if (info->dlpi_phdr != (const ElfW(Phdr) *)getauxval(AT_PHDR))
    return 0;
  if (!info->dlpi_addr)
    _Exit(1);
  return 1;
}

int main(int argc, char **argv) {
  if (dl_iterate_phdr(checkMainExecutable, 0) != 1)
    return 2;
  unsigned id = get_id(global);
  if (!id || id == ptsan_default_id() || ptsan_get_size(id) != sizeof(global))
    return 3;
  return argc > 1 ? global[argc] : global[0] != 17;
}
