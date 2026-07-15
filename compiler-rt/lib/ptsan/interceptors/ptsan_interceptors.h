#pragma once
#include "sanitizer_common/sanitizer_internal_defs.h"
#include "sanitizer_common/sanitizer_common.h"

extern SANITIZER_INTERFACE_ATTRIBUTE THREADLOCAL u32 ptsan_in_libc;

namespace __ptsan {
  void InitializeInterceptors();
  void InitializeLibCHeaderInterceptors();
  void InitializeFormatInterceptors();
} // namespace __ptsan
