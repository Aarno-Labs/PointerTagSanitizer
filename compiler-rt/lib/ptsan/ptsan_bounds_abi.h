//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ObjectBounds {
  // ID16: tagged object base. ID32: decoding bias B - origin, where B is the
  // raw object base. ID32 checks recover B by adding ptsan_origins[id].
  uint64_t base;
  uint64_t span;
};

#ifdef PTSAN_32_BIT_IDS
extern struct ObjectBounds *ptsan_bounds;
#else
extern struct ObjectBounds ptsan_bounds[];
#endif

#ifdef __cplusplus
}
#endif

#ifdef PTSAN_32_BIT_IDS
// Non-strict ID32 leaves native addresses and the all-ones sentinel unencoded.
static inline int ptsan_is_encoded_id32(uintptr_t bits) {
  return (bits >> 63) != 0 && bits != UINTPTR_MAX;
}
#endif

// Decode an address, without checking bounds or allocation lifetime. ID32
// decoding requires initialized metadata for the encoded ID. Strict ID32
// expects encoded input (including null), not an arbitrary raw address:
// decoding is not idempotent, so do not decode its result again. Non-strict
// ID32 passes unmarked values and UINTPTR_MAX through; ID16 masks the ID bits.
#ifdef PTSAN_32_BIT_IDS
static inline uintptr_t ptsan_raw_address(uintptr_t bits) {
#ifndef PTSAN_STRICT_32_BIT_IDS
  if (!ptsan_is_encoded_id32(bits)) {
    return bits;
  }
#endif
  uint32_t id = bits >> 32;
  return ptsan_bounds[id].base + (uint32_t)bits;
}
#else
static inline uintptr_t ptsan_raw_address(uintptr_t bits) {
  return bits & UINT64_C(0x0000ffffffffffff);
}
#endif
