//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// RUN: %clang_ptsan -O2 -pthread %s -o %t
// RUN: %run %t
// RUN: %clang_raw -fsanitize=ptsan -O2 -pthread %s -o %t.regular
// RUN: %run %t.regular
#include "Inputs/id_cache_exhaustion.c"
