//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
#pragma once

#include "interception/interception.h"

#ifdef PTSAN_MUSL_INTERCEPTION
// Musl uses static linker wrapping, not dynamically resolved function pointers.
// Define this before shared helpers are parsed so they also call directly.
#undef REAL
#define REAL(name) __real_##name
#undef DECLARE_REAL
#define DECLARE_REAL(ret, name, ...) \
  extern "C" ret REAL(name)(__VA_ARGS__);
#endif
