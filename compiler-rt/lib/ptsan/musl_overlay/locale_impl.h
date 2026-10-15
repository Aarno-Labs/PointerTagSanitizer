//===-- locale_impl.h - Complete PTSan musl syscall overlaying ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Musl's locale header includes its adjacent pthread_impl.h directly.
// Activate PTSan's syscall overlay before that header defines inline helpers.
//
//===----------------------------------------------------------------------===//

#include "syscall.h"
#include_next "locale_impl.h"
#include "pthread_impl.h"
