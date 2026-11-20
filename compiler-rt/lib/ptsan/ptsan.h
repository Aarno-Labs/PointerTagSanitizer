//===-- ptsan.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// PTSan runtime private interface.
//
//===----------------------------------------------------------------------===//

#pragma once
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_libc.h"

//#define PTSAN_LAM
SANITIZER_INTERFACE_ATTRIBUTE
extern int ClLAM;

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClValidate;

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClPrintStats;

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClStrictMode;

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClUseReturnedMallocSize;

SANITIZER_INTERFACE_ATTRIBUTE
extern unsigned ClIDBucketSize;

SANITIZER_INTERFACE_ATTRIBUTE
extern unsigned ClStackIDLogCapacity;

SANITIZER_INTERFACE_ATTRIBUTE
extern unsigned ClStackIDThreadBuckets;

SANITIZER_INTERFACE_ATTRIBUTE
extern unsigned ClIDReuseDistance;

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClRandomizeIDs;

SANITIZER_INTERFACE_ATTRIBUTE
extern unsigned ClRandomSeed;

using namespace __sanitizer;

#ifdef PTSAN_32_BIT_IDS
using PTSanID = u32;
SANITIZER_INTERFACE_ATTRIBUTE
extern int ClSupportIndicatorBit;
SANITIZER_INTERFACE_ATTRIBUTE
extern unsigned ClID32OriginQuantum;
#else
using PTSanID = u16;
#endif

extern "C" {

namespace __ptsan {
  extern bool ptsan_initialized;
  extern bool ptsan_init_is_running;
} // namespace __ptsan

// Initialise ptsan interceptors.
// A call to this method is added to the preinit array on Linux systems.
SANITIZER_INTERFACE_ATTRIBUTE void __ptsan_init();

// Process-global state only; musl calls this before establishing TLS.
SANITIZER_INTERFACE_ATTRIBUTE void __ptsan_init_ids();

SANITIZER_INTERFACE_ATTRIBUTE int ptsan_enable_lam_u48();

void InitializeIDs();
void InitializeSharedConstantRanges();

} //extern "C"
