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
list(APPEND PTSAN_TEST_DEPS opt)

add_lit_testsuite(check-ptsan "Running the Pointer Tag Sanitizer tests"
  ${PTSAN_TESTSUITES}
  DEPENDS ${PTSAN_TEST_DEPS})
