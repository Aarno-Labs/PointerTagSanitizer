# Keep the pinned upstream test-suite checkout unmodified. These four legacy
# translation units contain pointer mismatches accepted by the older compiler.
# Limit the diagnostic downgrade to those files, for LLVM 23 and later.
if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  return()
endif()

function(ptsan_multisource_compatibility)
  if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
     CMAKE_C_COMPILER_VERSION VERSION_LESS 23)
    return()
  endif()
  foreach(source IN ITEMS libclamav_cvd.c shared_cdiff.c)
    set_property(SOURCE "${CMAKE_SOURCE_DIR}/MultiSource/Applications/ClamAV/${source}"
      TARGET_DIRECTORY clamscan APPEND PROPERTY COMPILE_OPTIONS
      -Wno-error=incompatible-pointer-types)
  endforeach()
  set_property(SOURCE "${CMAKE_SOURCE_DIR}/MultiSource/Benchmarks/mediabench/gsm/toast/toast.c"
    TARGET_DIRECTORY toast APPEND PROPERTY COMPILE_OPTIONS
    -Wno-error=incompatible-pointer-types)
  set_property(SOURCE "${CMAKE_SOURCE_DIR}/MultiSource/Benchmarks/MiBench/telecomm-gsm/toast.c"
    TARGET_DIRECTORY telecomm-gsm APPEND PROPERTY COMPILE_OPTIONS
    -Wno-error=incompatible-pointer-types)
endfunction()

# CMAKE_PROJECT_INCLUDE runs before targets exist; apply their source properties
# after the root directory has finished adding all benchmark subdirectories.
cmake_language(DEFER CALL ptsan_multisource_compatibility)
