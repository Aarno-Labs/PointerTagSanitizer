//===-- ptsan_interceptors.h ------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file is a part of PointerTagSanitizer.
//
// PTSan libc interceptor declarations.
//
//===----------------------------------------------------------------------===//

#pragma once
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "sanitizer_common/sanitizer_common.h"

extern SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u32 ptsan_in_libc;

namespace __ptsan {
  void InitializeInterceptors();
  void InitializeLibCHeaderInterceptors();
  void InitializeFormatInterceptors();
} // namespace __ptsan
