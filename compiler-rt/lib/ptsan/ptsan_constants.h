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

#ifdef PTSAN_LAM
#ifdef PTSAN_ID_BITS
#if PTSAN_ID_BITS != 15
#error "PTSAN_LAM requires PTSAN_ID_BITS to be 15"
#endif
#else
#define PTSAN_ID_BITS 15
#endif
#elif !defined(PTSAN_ID_BITS)
#define PTSAN_ID_BITS 16
#endif

constexpr u16 DEFAULT_NUM_STACK_ID_BUCKETS = 12;
constexpr u16 DEFAULT_IDS_PER_STACK_BUCKET = 0x200; // 512
constexpr u16 MAX_STACK_ID_BUCKETS = 128;
constexpr u16 MAX_FAST_STACK_ID_LIMIT = 0x7fff;

// Allocatable ID-space size. LAM keeps one top bit free for hardware tagging,
// so its usable ID space is smaller than the metadata table's physical size.
constexpr u32 NumIDs = 1 << PTSAN_ID_BITS;
// ID to default to if we run out
constexpr u32 DefaultIDNo = NumIDs - 1;
// Last valid id
constexpr u32 LastIDNo = NumIDs - 2;

// Metadata tables are indexed from the raw top 16 bits of a pointer. In LAM
// mode only the low half is allocatable, but speculative/hoisted metadata reads
// can still see arbitrary high-half values from non-pointer words.
constexpr u32 MetadataIDBits = 16;
constexpr u32 MetadataNumIDs = 1 << MetadataIDBits;
static_assert(MetadataNumIDs >= NumIDs,
              "Metadata table must cover the allocatable ID space");

constexpr u32 ceil_pow2(u32 n) {
  if (n <= 1) return 1;
  n--;
  n |= n >> 1;
  n |= n >> 2;
  n |= n >> 4;
  n |= n >> 8;
  n |= n >> 16;
  return n + 1;
}

// -1 for 0, -1 for DefaultID
constexpr u32 NumValidIDs = NumIDs - 2;

// We use -2 ids (0 and default), but we want one free
// space in the buffer so we don't have to deal with
// empty/full ambiguity. For 3 id bits, we'd have 8 ids
// (0bx111) + 0, and we would reserve 0 and 7, so we'd have 6
// valid ones and want a buffer of size 7, which is
// NumValidIDs + 1.
//
// We then round to the nearest power of two so we can more
// efficiently modulo it
//   0   1   2   3   4   5   6    7
// [| 1 | 2 | 3 | 4 | 5 | 6 |   |   |]
constexpr u32 IDsBufSz = ceil_pow2(NumValidIDs);
// Make sure this really is a power of 2 or the masking logic later
// will explode
static_assert((IDsBufSz & (IDsBufSz - 1)) == 0,
              "IDsBufSz must be a power of two");

constexpr u32 IDsBufMask = IDsBufSz - 1;
