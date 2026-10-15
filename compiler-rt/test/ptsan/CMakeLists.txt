set(PTSAN_LIT_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR})

set(PTSAN_TESTSUITES)

set(PTSAN_TEST_ARCH ${PTSAN_SUPPORTED_ARCH})

foreach(arch ${PTSAN_TEST_ARCH})
  set(PTSAN_TEST_TARGET_ARCH ${arch})
  string(TOLOWER "-${arch}" PTSAN_TEST_CONFIG_SUFFIX)
  get_test_cc_for_arch(${arch} PTSAN_TEST_TARGET_CC PTSAN_TEST_TARGET_CFLAGS)
  string(TOUPPER ${arch} ARCH_UPPER_CASE)
  set(CONFIG_NAME ${ARCH_UPPER_CASE}Config)

  configure_lit_site_cfg(
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
    ${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_NAME}/lit.site.cfg.py)
  list(APPEND PTSAN_TESTSUITES ${CMAKE_CURRENT_BINARY_DIR}/${CONFIG_NAME})
endforeach()

set(PTSAN_TEST_DEPS ${SANITIZER_COMMON_LIT_TEST_DEPS})
list(APPEND PTSAN_TEST_DEPS ptsan)
list(APPEND PTSAN_TEST_DEPS opt llc)

add_lit_testsuite(check-ptsan "Running the Pointer Tag Sanitizer tests"
  ${PTSAN_TESTSUITES}
  DEPENDS ${PTSAN_TEST_DEPS})

# The musl tests use a separately built, PTSan-instrumented libc and its
# matching runtime. Keep them opt-in so the normal check-ptsan target
# neither requires those artifacts nor silently falls back to the host libc.
if(LLVM_ENABLE_PTSAN_DEPENDENCIES AND LLVM_INCLUDE_RUNTIMES AND
   PTSAN_MUSL_SOURCE_DIR AND NOT COMPILER_RT_STANDALONE_BUILD AND
   NOT LLVM_RUNTIMES_BUILD)
  set(PTSAN_MUSL_TEST_TARGET_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  if(PTSAN_MUSL_TEST_TARGET_ARCH MATCHES "^(aarch64|arm64|ARM64)$")
    set(PTSAN_MUSL_TEST_TARGET_ARCH aarch64)
  elseif(PTSAN_MUSL_TEST_TARGET_ARCH MATCHES "^(x86_64|amd64|AMD64)$")
    set(PTSAN_MUSL_TEST_TARGET_ARCH x86_64)
  else()
    message(FATAL_ERROR
      "Unsupported PTSan musl test architecture '${PTSAN_MUSL_TEST_TARGET_ARCH}'")
  endif()
  # These dependency targets are defined after compiler-rt. Resolve their
  # paths at generation time, before any dependency artifacts need to exist.
  set(PTSAN_MUSL_TEST_COMPILER
    "$<TARGET_PROPERTY:ptsan-musl-strict,PTSAN_MUSL_COMPILER>")
  set(PTSAN_MUSL_TEST_STRICT_CONFIG
    "$<TARGET_PROPERTY:ptsan-musl-strict,PTSAN_MUSL_CONFIG>")
  set(PTSAN_MUSL_TEST_NONSTRICT_CONFIG
    "$<TARGET_PROPERTY:ptsan-musl-nonstrict,PTSAN_MUSL_CONFIG>")
  set(PTSAN_MUSL_TEST_STRICT_CXX_CONFIG
    "$<TARGET_PROPERTY:ptsan-musl-strict-cxx,PTSAN_MUSL_CXX_CONFIG>")
  set(PTSAN_MUSL_TEST_NONSTRICT_CXX_CONFIG
    "$<TARGET_PROPERTY:ptsan-musl-nonstrict-cxx,PTSAN_MUSL_CXX_CONFIG>")
  set(PTSAN_MUSL_TEST_CONFIG_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/Musl${PTSAN_MUSL_TEST_TARGET_ARCH}Config)

  configure_lit_site_cfg(
    ${CMAKE_CURRENT_SOURCE_DIR}/Musl/lit.site.cfg.py.in
    ${PTSAN_MUSL_TEST_CONFIG_DIR}/lit.site.cfg.py.in)
  file(GENERATE OUTPUT ${PTSAN_MUSL_TEST_CONFIG_DIR}/lit.site.cfg.py
    INPUT ${PTSAN_MUSL_TEST_CONFIG_DIR}/lit.site.cfg.py.in)

  add_lit_testsuite(check-ptsan-musl
    "Running the Pointer Tag Sanitizer tests with instrumented musl"
    ${PTSAN_MUSL_TEST_CONFIG_DIR}
    EXCLUDE_FROM_CHECK_ALL
    DEPENDS ${SANITIZER_COMMON_LIT_TEST_DEPS} ptsan-musl-cxx)
endif()
