# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Match the static musl C++ layout without PTSan-specific unwinder options.
# The dependency builder supplies a plain musl sysroot and uninstrumented flags.
include("${CMAKE_CURRENT_LIST_DIR}/PTSan-musl-cxx.cmake")
unset(CMAKE_PROJECT_Runtimes_INCLUDE CACHE)
