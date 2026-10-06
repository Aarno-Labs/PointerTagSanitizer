include_directories(. ..)

option(COMPILER_RT_PTSAN_USE_INSTRUMENTED_LIBC
  "Build PTSan for use with an instrumented libc instead of libc interceptors"
  OFF)
set(COMPILER_RT_PTSAN_MUSL_SOURCE_DIR "" CACHE PATH
  "Path to the musl source tree used by the instrumented-libc PTSan runtime")

if(COMPILER_RT_PTSAN_USE_INSTRUMENTED_LIBC AND
   NOT IS_DIRECTORY "${COMPILER_RT_PTSAN_MUSL_SOURCE_DIR}/arch")
  message(FATAL_ERROR
    "COMPILER_RT_PTSAN_MUSL_SOURCE_DIR must name a musl source tree when "
    "COMPILER_RT_PTSAN_USE_INSTRUMENTED_LIBC is enabled")
endif()

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
  interceptors/ptsan_ir_interceptor_helpers.cpp
)

if(COMPILER_RT_PTSAN_USE_INSTRUMENTED_LIBC)
  list(APPEND PTSAN_RTL_SOURCES
    ptsan_musl_internal.c
    interceptors/ptsan_musl_interceptors.cpp
    interceptors/ptsan_musl_syscalls.cpp
  )
else()
  list(APPEND PTSAN_RTL_SOURCES
    interceptors/ptsan_interceptors_with_libc_headers.cpp
  )
endif()

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
  ptsan_musl_interception.h
  ptsan_musl_internal.h
  interceptors/ptsan_common_interceptors.inc
  interceptors/ptsan_libc.inc
  musl_overlay/syscall.h
)

set(PTSAN_COMMON_CFLAGS ${SANITIZER_COMMON_CFLAGS} -g )
append_list_if(COMPILER_RT_PTSAN_USE_INSTRUMENTED_LIBC
  -DPTSAN_INSTRUMENTED_LIBC PTSAN_COMMON_CFLAGS)
# This runtime currently uses musl's static linker-wrapped API boundaries.
# Keep that mechanism separate from instrumented-libc initialization policy.
append_list_if(COMPILER_RT_PTSAN_USE_INSTRUMENTED_LIBC
  -DPTSAN_MUSL_INTERCEPTION PTSAN_COMMON_CFLAGS)
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

# Separate runtime builds receive their compiler through CMake. In an LLVM
# project build, the clang target can be defined after compiler-rt.
if(COMPILER_RT_STANDALONE_BUILD)
  set(PROJECT_CLANG "${CMAKE_C_COMPILER}")
else()
  set(PROJECT_CLANG "$<TARGET_FILE:clang>")
endif()

if(COMPILER_RT_STANDALONE_BUILD AND
   NOT COMPILER_RT_PTSAN_USE_INSTRUMENTED_LIBC)
  check_c_compiler_flag("-mllvm -ptsan-strip-ids" COMPILER_RT_HAS_PTSAN_STRIP_IDS)
  if(NOT COMPILER_RT_HAS_PTSAN_STRIP_IDS)
    message(FATAL_ERROR
      "${PROJECT_CLANG} does not support -mllvm -ptsan-strip-ids. "
      "Set CMAKE_C_COMPILER to a Clang built from this PTSan tree.")
  endif()
endif()

# Helper: build one source with PROJECT_CLANG -> per-arch/variant .o, expose as
# IMPORTED OBJECT, and append its $<TARGET_OBJECTS:...> genex into
# PTSAN_OBJ_GENEX_<variant>_<arch> in the caller.
function(ptsan_add_interceptor_obj variant arch rel_src)
  set(options)
  set(oneValueArgs)
  set(multiValueArgs CFLAGS PASS)
  cmake_parse_arguments(PAIO "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  set(src "${CMAKE_CURRENT_SOURCE_DIR}/${rel_src}")
  get_filename_component(name_we "${rel_src}" NAME_WE)
  set(obj "${CMAKE_CURRENT_BINARY_DIR}/${name_we}_${variant}_${arch}.o")

  # Normalize depfile paths for this subdirectory's custom command.
  cmake_policy(PUSH)
  cmake_policy(SET CMP0116 NEW)
  add_custom_command(
    OUTPUT  "${obj}"
    COMMAND "${PROJECT_CLANG}"
            ${PAIO_CFLAGS}
            ${EXTRA_INCLUDE_FLAGS}
            ${PAIO_PASS}
            -MD -MF "${obj}.d"
            -c "${src}"
            -o "${obj}"
    DEPENDS "${src}" "${PROJECT_CLANG}"
    DEPFILE "${obj}.d"
    COMMENT "Building ${rel_src} with ptsan-strip-ids for variant=${variant} arch=${arch}"
    VERBATIM
  )
  cmake_policy(POP)

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
  if(COMPILER_RT_PTSAN_USE_INSTRUMENTED_LIBC)
    if(NOT EXISTS
       "${COMPILER_RT_PTSAN_MUSL_SOURCE_DIR}/arch/${arch}/syscall_arch.h")
      message(FATAL_ERROR
        "musl has no syscall_arch.h for compiler-rt architecture ${arch}")
    endif()
    list(APPEND PTSAN_CFLAGS
      -I${COMPILER_RT_PTSAN_MUSL_SOURCE_DIR}/arch/${arch}
      -iquote${COMPILER_RT_PTSAN_MUSL_SOURCE_DIR})
  endif()
  set(PTSAN_OBJ_GENEX_${runtime_name}_${arch} "")

  if(NOT COMPILER_RT_PTSAN_USE_INSTRUMENTED_LIBC)
    ptsan_add_interceptor_obj(
      ${runtime_name}
      ${arch}
      interceptors/ptsan_interceptors.cpp
      CFLAGS ${PTSAN_CFLAGS}
      PASS -mllvm -ptsan-strip-ids
    )

    # va_arg loads through backing pointers that may carry IDs. Let the
    # strip-only pass handle those loads without depending on va_list's ABI.
    ptsan_add_interceptor_obj(
      ${runtime_name}
      ${arch}
      interceptors/ptsan_interceptors_format.cpp
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
  endif()

  set(PTSAN_COMMON_OBJECTS
    $<TARGET_OBJECTS:RTSanitizerCommon.${arch}>
    $<TARGET_OBJECTS:RTSanitizerCommonLibc.${arch}>
  )
  if(NOT COMPILER_RT_PTSAN_USE_INSTRUMENTED_LIBC)
    list(APPEND PTSAN_COMMON_OBJECTS
      $<TARGET_OBJECTS:RTInterception.${arch}>
      $<TARGET_OBJECTS:RTSanitizerCommonSymbolizer.${arch}>
      $<TARGET_OBJECTS:RTSanitizerCommonSymbolizerInternal.${arch}>
    )
  endif()

  add_compiler_rt_runtime(clang_rt.${runtime_name}
    STATIC
    ARCHS ${arch}
    SOURCES
      ${PTSAN_RTL_SOURCES}
      ${PTSAN_OBJ_GENEX_${runtime_name}_${arch}}
      ${PTSAN_COMMON_OBJECTS}
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
  get_compiler_rt_output_dir(${arch} ptsan_output_dir)
  get_compiler_rt_install_dir(${arch} ptsan_install_dir)
  configure_file(ptsan-init-ro.ld ${ptsan_output_dir}/ptsan-init-ro.ld COPYONLY)
  install(FILES ptsan-init-ro.ld DESTINATION ${ptsan_install_dir}
    COMPONENT ptsan)

  ptsan_add_runtime_variant(ptsan ${arch})
  ptsan_add_runtime_variant(ptsan_validate ${arch} -DPTSAN_VALIDATE)
  # AArch64 uses software masking; LAM is an x86-64 hardware feature.
  if(arch STREQUAL "x86_64")
    ptsan_add_runtime_variant(ptsan_lam ${arch} -DPTSAN_LAM)
    ptsan_add_runtime_variant(ptsan_lam_validate ${arch}
      -DPTSAN_LAM -DPTSAN_VALIDATE)
  endif()
endforeach()
