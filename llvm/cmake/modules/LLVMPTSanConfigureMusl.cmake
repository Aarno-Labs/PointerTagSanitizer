# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Find the support libraries using the same compiler and target as musl's
# wrapper, before the application sysroot changes GCC installation discovery.
include("${CMAKE_CURRENT_LIST_DIR}/LLVMPTSanMuslTarget.cmake")
execute_process(
  COMMAND "${PTSAN_CLANG}" --target=${PTSAN_MUSL_TARGET} --rtlib=libgcc
    -print-libgcc-file-name
  RESULT_VARIABLE result
  OUTPUT_VARIABLE libgcc
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0 OR NOT IS_ABSOLUTE "${libgcc}" OR NOT EXISTS "${libgcc}")
  message(FATAL_ERROR "Could not locate GCC support libraries for PTSan's musl configuration")
endif()
get_filename_component(PTSAN_GCC_INSTALL_DIR "${libgcc}" DIRECTORY)

# The musl resource directory contains the runtime, not Clang's builtin headers.
# Keep musl headers first, then use the compiler's own headers for intrinsics.
execute_process(
  COMMAND "${PTSAN_CLANG}" --no-default-config -print-resource-dir
  RESULT_VARIABLE result
  OUTPUT_VARIABLE resource_dir
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT result EQUAL 0 OR NOT IS_ABSOLUTE "${resource_dir}" OR
   NOT IS_DIRECTORY "${resource_dir}/include")
  message(FATAL_ERROR "Could not locate Clang builtin headers for PTSan's musl configuration")
endif()
get_filename_component(config_dir "${PTSAN_CONFIG_OUTPUT}" DIRECTORY)
file(RELATIVE_PATH builtin_headers "${config_dir}" "${resource_dir}/include")
set(PTSAN_CLANG_BUILTIN_HEADERS "<CFGDIR>/${builtin_headers}")
configure_file("${PTSAN_CONFIG_INPUT}" "${PTSAN_CONFIG_OUTPUT}" @ONLY)

if(PTSAN_INSTALL_CONFIG_OUTPUT)
  set(PTSAN_CLANG_BUILTIN_HEADERS "${PTSAN_INSTALL_BUILTIN_HEADERS}")
  configure_file("${PTSAN_INSTALL_CONFIG_OUTPUT}.in"
    "${PTSAN_INSTALL_CONFIG_OUTPUT}" @ONLY)
endif()
