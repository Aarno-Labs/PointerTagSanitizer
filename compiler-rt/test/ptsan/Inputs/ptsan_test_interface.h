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

#ifdef PTSAN_TEST_32_BIT_IDS
typedef uint32_t PTSanID;
#else
typedef uint16_t PTSanID;
#endif

#define get_id(ptr) ptsan_get_id((const void *)(uintptr_t)(ptr))
#define print_id(ptr)                                                          \
  do {                                                                         \
    printf(#ptr " id: 0x%x\n", get_id(ptr));                                   \
  } while (0)

#ifdef __cplusplus
extern "C" {
#endif

// Runtime TLS layout: checkpoints read the cursor in the first word.
extern __thread struct {
  uintptr_t cursor;
  PTSanID *ids;
} ptsan_stack_id_log;

PTSanID ptsan_get_id(const void *ptr);
uint64_t ptsan_get_stack_cursor(void);
uint64_t ptsan_get_stack_limit(void);
uint64_t ptsan_get_address(const void *ptr);
uint64_t ptsan_get_bound(PTSanID id);
uint64_t ptsan_get_base(PTSanID id);
uint64_t ptsan_get_size(PTSanID id);
#ifdef PTSAN_TEST_32_BIT_IDS
uint32_t ptsan_get_origin(PTSanID id);
#endif
uint32_t ptsan_get_payload(const void *ptr);
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
