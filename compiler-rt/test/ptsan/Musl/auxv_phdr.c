//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %ptsan_musl_strict -o %t %s
// RUN: %run %t
// RUN: not %run %t bad 2>&1 | FileCheck %s
// RUN: %ptsan_musl_strict -DCHECK_ITERATION -o %t.iterate %s
// RUN: %run %t.iterate
// CHECK: Found bad

#define _GNU_SOURCE
#include "../Inputs/ptsan_test_interface.h"
#include <elf.h>
#include <link.h>
#include <stdlib.h>
#include <sys/auxv.h>

#ifdef CHECK_ITERATION
struct CallbackData {
  unsigned id;
  int calls;
  int nested;
};

static void checkIteration(int nested);

static int checkHeaders(struct dl_phdr_info *info, size_t size, void *data) {
  struct CallbackData *context = data;
  if (size < sizeof(*info) || !get_id(info) || !get_id(data) ||
      get_id(data) != context->id || context->calls++)
    _Exit(7);
  const Elf64_Phdr *phdr = (void *)getauxval(AT_PHDR);
  if (info->dlpi_phdr != phdr ||
      get_id(info->dlpi_phdr) != get_id(phdr) ||
      info->dlpi_phnum != getauxval(AT_PHNUM))
    _Exit(5);
  // This test uses the configured static, non-PIE executable. Its ELF
  // virtual addresses are already absolute, so the load bias must be zero.
  if (info->dlpi_addr != 0)
    _Exit(6);
#ifdef REQUIRE_PT_PHDR
  int found = 0;
  for (size_t i = 0; i < info->dlpi_phnum; ++i)
    found |= info->dlpi_phdr[i].p_type == PT_PHDR;
  if (!found)
    _Exit(8);
#endif
  if (!context->nested)
    checkIteration(1);
  return 37;
}

static void checkIteration(int nested) {
  struct CallbackData context = {0, 0, nested};
  context.id = get_id(&context);
  if (dl_iterate_phdr(checkHeaders, &context) != 37 || context.calls != 1)
    _Exit(4);
}
#endif

int main(int argc, char **argv) {
  volatile const Elf64_Phdr *phdr = (void *)getauxval(AT_PHDR);
  size_t count = getauxval(AT_PHNUM);
  size_t stride = getauxval(AT_PHENT);
  unsigned id = get_id(phdr);
  if (!id || id == ptsan_default_id() || stride != sizeof(*phdr) ||
      !count || ptsan_get_size(id) != count * stride)
    _Exit(2);
  unsigned loads = 0;
  for (size_t i = 0; i < count; ++i)
    if (phdr[i].p_type == PT_LOAD)
      ++loads;
  if (!loads)
    _Exit(3);
#ifdef CHECK_ITERATION
  checkIteration(0);
#endif
  if (argc > 1)
    (void)phdr[count].p_type;
  _Exit(0);
}
