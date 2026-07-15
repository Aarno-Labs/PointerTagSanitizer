//===-- ptsan_test_interface.h ----------------------------------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef PTSAN_TEST_INTERFACE_H
#define PTSAN_TEST_INTERFACE_H

#include <stdint.h>
#include <stdio.h>

#define get_id(ptr) ((uint16_t)((uintptr_t)(ptr) >> 48))
#define print_id(ptr)                                                          \
  do {                                                                         \
    printf(#ptr " id: 0x%x\n", get_id(ptr));                                   \
  } while (0)

#ifdef __cplusplus
extern "C" {
#endif

uint64_t ptsan_get_bound(uint16_t id);
uint64_t ptsan_get_base(uint16_t id);
uint64_t ptsan_get_size(uint16_t id);
uint32_t ptsan_id_bits(void);

#ifdef __cplusplus
} // extern "C"
#endif

static inline uint64_t ptsan_num_ids(void) {
  return (UINT64_C(1) << ptsan_id_bits()) - 1;
}

static inline uint64_t ptsan_max_id(void) { return ptsan_num_ids() - 1; }

static inline uint64_t ptsan_default_id(void) { return ptsan_num_ids(); }

#endif // PTSAN_TEST_INTERFACE_H
