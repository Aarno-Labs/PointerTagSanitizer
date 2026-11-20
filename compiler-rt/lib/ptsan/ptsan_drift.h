//===-- ptsan_drift.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// Drift-specific PTSan machinery interface.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "ptsan.h"
#include "sanitizer_common/sanitizer_internal_defs.h"

#include <setjmp.h>



void ptsan_handle_error(u32 check_id, u64 function_id, u64 ptr_id,
                  const char * type);

SANITIZER_INTERFACE_ATTRIBUTE
extern int ClPatchAllWith;
