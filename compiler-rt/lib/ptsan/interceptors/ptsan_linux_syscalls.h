//===-- ptsan_linux_syscalls.h -------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "sanitizer_common/sanitizer_internal_defs.h"

namespace __ptsan {

using __sanitizer::sptr;

struct SyscallExecutor {
  // Arguments are kernel-ready; errors use the raw negative-errno convention.
  virtual sptr operator()(sptr number, sptr a, sptr b, sptr c, sptr d, sptr e,
                          sptr f) const = 0;
};

sptr dispatchSyscall(const SyscallExecutor &executor, sptr number, sptr a,
                     sptr b, sptr c, sptr d, sptr e, sptr f);

} // namespace __ptsan
