//===-- ptsan_constants.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// PTSan compile-time configuration constants.
//
//===----------------------------------------------------------------------===//

#pragma once
#include "ptsan.h"

#ifndef PTSAN_32_BIT_IDS
#define PTSAN_HAS_DEFAULT_ID 1
#endif

#ifdef PTSAN_32_BIT_IDS
#ifdef PTSAN_LAM
#error "PTSAN_32_BIT_IDS and PTSAN_LAM are incompatible"
#endif
#define PTSAN_ID_BITS 32
#elif defined(PTSAN_LAM)
#define PTSAN_ID_BITS 15
#else
#define PTSAN_ID_BITS 16
#endif

// Size of the ID namespace, including reserved IDs.
constexpr u64 NumIDs = 1ULL << PTSAN_ID_BITS;
#ifdef PTSAN_HAS_DEFAULT_ID
// ID to default to if we run out.
constexpr u32 DefaultIDNo = NumIDs - 1;
constexpr u32 LastIDNo = NumIDs - 2;
#else
constexpr u32 LastIDNo = NumIDs - 1;
#endif

// Bucket boundaries cover this aligned namespace; ID zero remains reserved.
#ifdef PTSAN_32_BIT_IDS
#define PTSAN_ID_SPACE_BEGIN                                                    \
  ((!ClStrictMode || ClSupportIndicatorBit) ? 0x80000000U : 0U)
#else
#define PTSAN_ID_SPACE_BEGIN 0U
#endif

inline bool isDefaultID(PTSanID id) {
#ifdef PTSAN_HAS_DEFAULT_ID
  return id == DefaultIDNo;
#else
  return false;
#endif
}

// Metadata tables cover the IDs extracted from pointers. ID16 needs all 16
// index bits even under LAM: speculative/hoisted reads can see high-half IDs
// from non-pointer words. ID32 uses its configured ID width.
constexpr u32 MetadataIDBits = PTSAN_ID_BITS > 16 ? PTSAN_ID_BITS : 16;
constexpr u64 MetadataNumIDs = 1ULL << MetadataIDBits;
static_assert(MetadataNumIDs >= NumIDs,
              "Metadata table must cover the allocatable ID space");
