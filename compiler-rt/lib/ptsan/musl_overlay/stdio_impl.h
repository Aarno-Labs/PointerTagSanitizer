//===-- stdio_impl.h - Complete PTSan musl syscall overlaying ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Musl's original internal header can reach its adjacent syscall.h before
// -iquote search paths are considered. Include it first, then activate PTSan's
// syscall overlay for the including translation unit.
//
//===----------------------------------------------------------------------===//

#include_next "stdio_impl.h"
#include "syscall.h"
