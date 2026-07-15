include_directories(..)

set(PTSAN_RTL_SOURCES
  ptsan_bounds.cpp
  ptsan_stats.cpp
  ptsan_drift.cpp
  ptsan.cpp
  # We have to special-case this. See ptsan_interceptors.cpp for details
  # ptsan_interceptors.cpp
  # ptsan_tsearch.c
  ptsan_preinit.cpp
  ptsan_thread.cpp
  interceptors/ptsan_interceptors_with_libc_headers.cpp
  interceptors/ptsan_interceptors_format.cpp
  interceptors/ptsan_ir_interceptor_helpers.cpp
)

# We keep track of this header list as usual:
set(PTSAN_RTL_HEADERS
  ptsan_constants.h
  ptsan_bounds.h
  ptsan_stats.h
  ptsan_drift.h
  ptsan_interceptors.h
  ptsan.h
  ptsan_thread.h
  ptsan_ptr_utils.h
  interceptors/ptsan_common_interceptors.inc
  interceptors/ptsan_libc.inc
)

set(PTSAN_COMMON_CFLAGS ${SANITIZER_COMMON_CFLAGS} -g )
append_rtti_flag(OFF PTSAN_COMMON_CFLAGS)
# Prevent clang from generating libc calls.
append_list_if(COMPILER_RT_HAS_FFREESTANDING_FLAG -ffreestanding PTSAN_COMMON_CFLAGS)

# Static runtime library.
add_compiler_rt_component(ptsan)

get_directory_property(MY_INCLUDES INCLUDE_DIRECTORIES)

set(EXTRA_INCLUDE_FLAGS "")
foreach(dir IN LISTS MY_INCLUDES)
  list(APPEND EXTRA_INCLUDE_FLAGS "-I${dir}")
endforeach()

set(PROJECT_CLANG ${CMAKE_CURRENT_BINARY_DIR}/../../../../bin/clang)

# Helper: build one source with PROJECT_CLANG -> per-arch/variant .o, expose as
# IMPORTED OBJECT, and append its $<TARGET_OBJECTS:...> genex into
# PTSAN_OBJ_GENEX_<variant>_<arch> in the caller.
function(ptsan_add_interceptor_obj variant arch rel_src)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs CFLAGS EXTRA_DEPS PASS)
  cmake_parse_arguments(PAIO "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  set(src "${CMAKE_CURRENT_SOURCE_DIR}/${rel_src}")
  get_filename_component(name_we "${rel_src}" NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${name_we}_${variant}_${arch}.o")

  set(deps
      "${src}"
      "${CMAKE_CURRENT_SOURCE_DIR}/ptsan_ptr_utils.h"
      "${CMAKE_CURRENT_SOURCE_DIR}/ptsan.h"
      "${PROJECT_CLANG}"
  )
  list(APPEND deps ${PAIO_EXTRA_DEPS})

  add_custom_command(
    OUTPUT  "${obj}"
    COMMAND "${PROJECT_CLANG}"
            ${PAIO_CFLAGS}
            ${EXTRA_INCLUDE_FLAGS}
            ${PAIO_PASS}
            -c "${src}"
            -o "${obj}"
    DEPENDS ${deps}
    COMMENT "Building ${rel_src} with ptsan-strip-ids for variant=${variant} arch=${arch}"
    VERBATIM
  )

  # Import the .o as an OBJECT library
  set(lib "ptsan_${variant}_${name_we}_obj_${arch}")
  add_library(${lib} OBJECT IMPORTED)
  set_target_properties(${lib} PROPERTIES IMPORTED_OBJECTS "${obj}")

  # Append its TARGET_OBJECTS genex to parent var PTSAN_OBJ_GENEX_<variant>_<arch>
  set(genex "$<TARGET_OBJECTS:${lib}>")
  set(parent_list_var "PTSAN_OBJ_GENEX_${variant}_${arch}")
  set(current_value "${${parent_list_var}}")
  if(current_value)
    set(current_value "${current_value};${genex}")
  else()
    set(current_value "${genex}")
  endif()
  set(${parent_list_var} "${current_value}" PARENT_SCOPE)
endfunction()

function(ptsan_add_runtime_variant runtime_name arch)
  set(PTSAN_CFLAGS ${PTSAN_COMMON_CFLAGS} ${ARGN})
  set(PTSAN_OBJ_GENEX_${runtime_name}_${arch} "")

  ptsan_add_interceptor_obj(
    ${runtime_name}
    ${arch}
    interceptors/ptsan_interceptors.cpp
    EXTRA_DEPS ${CMAKE_CURRENT_SOURCE_DIR}/interceptors/ptsan_common_interceptors.inc
    CFLAGS ${PTSAN_CFLAGS}
    PASS -mllvm -ptsan-strip-ids
  )

  set(PTSAN_INTERCEPTOR_SRCS
    interceptors/ptsan_tsearch.c
    interceptors/ptsan_env.c
    interceptors/ptsan_exec.c
    interceptors/ptsan_obstack.cpp
    interceptors/ptsan_qsort.c
  )

  foreach(src IN LISTS PTSAN_INTERCEPTOR_SRCS)
    ptsan_add_interceptor_obj(
      ${runtime_name}
      ${arch}
      ${src}
      CFLAGS ${PTSAN_CFLAGS}
      PASS -mllvm -ptsan-strip-ids
    )
  endforeach()

  add_compiler_rt_runtime(clang_rt.${runtime_name}
    STATIC
    ARCHS ${arch}
    SOURCES
      ${PTSAN_RTL_SOURCES}
      ${PTSAN_OBJ_GENEX_${runtime_name}_${arch}}
      $<TARGET_OBJECTS:RTInterception.${arch}>
      $<TARGET_OBJECTS:RTSanitizerCommon.${arch}>
      $<TARGET_OBJECTS:RTSanitizerCommonLibc.${arch}>
      $<TARGET_OBJECTS:RTSanitizerCommonSymbolizer.${arch}>
      $<TARGET_OBJECTS:RTSanitizerCommonSymbolizerInternal.${arch}>
    ADDITIONAL_HEADERS ${PTSAN_RTL_HEADERS}
    CFLAGS ${PTSAN_CFLAGS}
    PARENT_TARGET ptsan)

  add_sanitizer_rt_symbols(clang_rt.${runtime_name}
    ARCHS ${arch}
    EXTRA ptsan.syms.extra)

  add_dependencies(ptsan clang_rt.${runtime_name}-${arch}-symbols)
endfunction()

# -------------------------------------------------------------------

foreach(arch ${PTSAN_SUPPORTED_ARCH})
  ptsan_add_runtime_variant(ptsan ${arch})
  ptsan_add_runtime_variant(ptsan_validate ${arch} -DPTSAN_VALIDATE)
  # AArch64 uses software masking; LAM is an x86-64 hardware feature.
  if(arch STREQUAL "x86_64")
    ptsan_add_runtime_variant(ptsan_lam ${arch} -DPTSAN_LAM)
    ptsan_add_runtime_variant(ptsan_lam_validate ${arch}
      -DPTSAN_LAM -DPTSAN_VALIDATE)
  endif()
endforeach()
