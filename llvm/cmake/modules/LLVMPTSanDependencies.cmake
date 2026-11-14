# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

include(ExternalProject)
include(GetClangResourceDir)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" OR
   NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|aarch64|arm64|ARM64)$" OR
   CMAKE_CROSSCOMPILING)
  message(FATAL_ERROR
    "PTSan dependency targets require a native Linux x86-64 or AArch64 build")
endif()
if(NOT TARGET clang OR NOT TARGET ptsan)
  message(FATAL_ERROR "PTSan dependency targets require clang and compiler-rt")
endif()

set(PTSAN_DEPENDENCIES_DIR "${LLVM_BINARY_DIR}/ptsan" CACHE PATH
  "Build and installation directory for PTSan dependencies")
set(PTSAN_MUSL_SOURCE_DIR "" CACHE PATH "Path to an existing musl source tree")

set(PTSAN_ID32_STACK_TEMPORAL_SAFETY 1 CACHE STRING
  "Stack temporal safety level for ID32 dependencies and generated configs (0 or 1)")
if(NOT PTSAN_ID32_STACK_TEMPORAL_SAFETY MATCHES "^[01]$")
  message(FATAL_ERROR "PTSAN_ID32_STACK_TEMPORAL_SAFETY must be 0 or 1")
endif()

option(PTSAN_BUILD_BASELINE_DEPENDENCIES
  "Also build uninstrumented musl and C++ libraries for baseline comparisons" OFF)

option(PTSAN_BUILD_LAM_DEPENDENCIES "Also build x86-64 LAM dependency stacks" OFF)
if(PTSAN_BUILD_LAM_DEPENDENCIES AND
   NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  message(FATAL_ERROR "PTSan LAM dependencies require native x86-64")
endif()

get_filename_component(ptsan_source "${LLVM_MAIN_SRC_DIR}/.." ABSOLUTE)
set(ptsan_caches "${ptsan_source}/clang/cmake/caches")
set(ptsan_clang "${LLVM_RUNTIME_OUTPUT_INTDIR}/clang")
set(ptsan_cxx_targets install-unwind install-cxxabi install-cxx)
set(ptsan_tools clang llvm-config llvm-ar llvm-ranlib)

# Clang searches for named configurations beside its executable. Keep paths
# relative to that directory so the compiler and dependencies can move together.
file(RELATIVE_PATH ptsan_config_root
  "${LLVM_RUNTIME_OUTPUT_INTDIR}" "${PTSAN_DEPENDENCIES_DIR}")
set(ptsan_config_root "<CFGDIR>/${ptsan_config_root}")
set(ptsan_install_root "lib${LLVM_LIBDIR_SUFFIX}/ptsan")
set(ptsan_install_config_dir "${CMAKE_CURRENT_BINARY_DIR}/ptsan-install-configs")
get_clang_resource_dir(ptsan_install_resource_dir)
set(ptsan_config_header "\
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
\n")
# Generate configs for both layouts from the same options. The musl toolchain
# templates are completed after Clang is built and can locate GCC's libraries.
function(ptsan_generate_config name content)
  cmake_parse_arguments(ARG "MUSL_TOOLCHAIN" "" "" ${ARGN})
  set(suffix "")
  if(ARG_MUSL_TOOLCHAIN)
    set(suffix ".in")
  endif()
  file(GENERATE OUTPUT "${LLVM_RUNTIME_OUTPUT_INTDIR}/${name}${suffix}"
    CONTENT "${content}")
  set(installed_content "${content}")
  foreach(variant glibc glibc-lam glibc-id32)
    string(REPLACE "${ptsan_config_root}/${variant}-cxx/install"
      "<CFGDIR>/../${ptsan_install_root}/${variant}-cxx" installed_content "${installed_content}")
  endforeach()
  string(REPLACE "${ptsan_config_root}" "<CFGDIR>/../${ptsan_install_root}"
    installed_content "${installed_content}")
  file(GENERATE OUTPUT "${ptsan_install_config_dir}/${name}${suffix}"
    CONTENT "${installed_content}")
  install(FILES "${ptsan_install_config_dir}/${name}"
    DESTINATION bin COMPONENT ptsan-dependencies)
endfunction()

# These remain ordinary compiler-rt and runtimes builds, each with its own
# cache and install tree. Only the requested libraries are built and installed.
function(ptsan_add_cmake_project name source binary cache)
  cmake_parse_arguments(ARG "" "" "DEPENDS;CMAKE_ARGS;TARGETS" ${ARGN})
  ExternalProject_Add(${name}
    SOURCE_DIR "${source}"
    BINARY_DIR "${binary}"
    PREFIX "${binary}/external-project"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
    DEPENDS ${ptsan_tools} ${ARG_DEPENDS}
    CMAKE_ARGS
      -DCMAKE_C_COMPILER=${ptsan_clang}
      -DCMAKE_CXX_COMPILER=${LLVM_RUNTIME_OUTPUT_INTDIR}/clang++
      -DCMAKE_ASM_COMPILER=${ptsan_clang}
      -DCMAKE_AR=${LLVM_RUNTIME_OUTPUT_INTDIR}/llvm-ar
      -DCMAKE_RANLIB=${LLVM_RUNTIME_OUTPUT_INTDIR}/llvm-ranlib
      -DCMAKE_INSTALL_MESSAGE=LAZY
      ${ARG_CMAKE_ARGS}
      # Musl caches require the explicit target before they are loaded.
      -C "${cache}"
    # These install targets populate the build tree. Only the outer install
    # should honor DESTDIR when packaging the finished toolchain.
    BUILD_COMMAND ${CMAKE_COMMAND} -E env --unset=DESTDIR
      ${CMAKE_COMMAND} --build <BINARY_DIR> --target ${ARG_TARGETS}
    INSTALL_COMMAND ""
    BUILD_ALWAYS TRUE
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE)
  ExternalProject_Add_StepDependencies(${name} configure
    "${cache}" "${ptsan_source}/llvm/cmake/modules/LLVMPTSanMuslTarget.cmake"
    "$<TARGET_FILE:clang>")
  # Compiler changes can alter instrumentation without changing source files.
  ExternalProject_Add_Step(${name} clean
    COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target clean
    DEPENDEES configure
    DEPENDERS build
    USES_TERMINAL TRUE)
endfunction()

set(ptsan_glibc_variants glibc glibc-id32)
if(PTSAN_BUILD_LAM_DEPENDENCIES)
  list(APPEND ptsan_glibc_variants glibc-lam)
endif()
add_custom_target(ptsan-dependencies ALL)
foreach(ptsan_glibc_variant IN LISTS ptsan_glibc_variants)
  set(ptsan_glibc_encoding_flags "")
  # Strip-only calls use the runtime's varargs sentinel in every encoding;
  # ID32 also reads its metadata tables. Resolve these from the executable.
  # Configuration probes cannot link standalone. The Release-specific flag
  # follows LLVM's common -z defs flag when linking the shared libraries.
  set(ptsan_glibc_cmake_args
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
    -DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-Wl,-z,undefs)
  if(ptsan_glibc_variant STREQUAL "glibc-lam")
    set(ptsan_glibc_encoding_flags "-fsanitize-ptsan-lam")
  elseif(ptsan_glibc_variant STREQUAL "glibc-id32")
    set(ptsan_glibc_encoding_flags
      "-fsanitize-ptsan-32-bit-ids -mllvm -ptsan-stack-temporal-safety=${PTSAN_ID32_STACK_TEMPORAL_SAFETY}")
  endif()
  set(ptsan_glibc_flags "-mllvm -ptsan-strip-ids -mllvm -ptsan-strip-ids-on-cmp ${ptsan_glibc_encoding_flags}")
  ptsan_generate_config(ptsan-${ptsan_glibc_variant}.cfg
    "${ptsan_config_header}\
-fsanitize=ptsan
${ptsan_glibc_encoding_flags}
-nostdinc++
-I\"${ptsan_config_root}/${ptsan_glibc_variant}-cxx/install/include/c++/v1\"
-stdlib=libc++
-L\"${ptsan_config_root}/${ptsan_glibc_variant}-cxx/install/lib\"
-Wl,-rpath,\"${ptsan_config_root}/${ptsan_glibc_variant}-cxx/install/lib\"
")

  ptsan_add_cmake_project(ptsan-${ptsan_glibc_variant}-cxx
    "${ptsan_source}/runtimes" "${PTSAN_DEPENDENCIES_DIR}/${ptsan_glibc_variant}-cxx/build"
    "${ptsan_caches}/PTSan-glibc-cxx.cmake"
    DEPENDS ptsan
    CMAKE_ARGS "-DCMAKE_C_FLAGS=${ptsan_glibc_flags}" "-DCMAKE_CXX_FLAGS=${ptsan_glibc_flags}"
      ${ptsan_glibc_cmake_args}
      -DCMAKE_INSTALL_PREFIX=${PTSAN_DEPENDENCIES_DIR}/${ptsan_glibc_variant}-cxx/install
    TARGETS ${ptsan_cxx_targets})

  install(DIRECTORY "${PTSAN_DEPENDENCIES_DIR}/${ptsan_glibc_variant}-cxx/install/"
    DESTINATION "${ptsan_install_root}/${ptsan_glibc_variant}-cxx" COMPONENT ptsan-dependencies)
  add_dependencies(ptsan-dependencies ptsan-${ptsan_glibc_variant}-cxx)
endforeach()
if(NOT LLVM_ENABLE_IDE)
  add_llvm_install_targets(install-ptsan-dependencies
    DEPENDS ptsan-dependencies COMPONENT ptsan-dependencies)
endif()

if(NOT PTSAN_MUSL_SOURCE_DIR)
  return()
endif()
if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(ptsan_musl_arch aarch64)
elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
  set(ptsan_musl_arch x86_64)
else()
  message(FATAL_ERROR
    "Unsupported PTSan musl host architecture '${CMAKE_HOST_SYSTEM_PROCESSOR}'")
endif()
set(ptsan_musl_target "${ptsan_musl_arch}-unknown-linux-musl")
if(NOT EXISTS "${PTSAN_MUSL_SOURCE_DIR}/configure")
  message(FATAL_ERROR "PTSAN_MUSL_SOURCE_DIR must name a musl source tree")
endif()

# Linux UAPI headers are needed by the runtime and unwinder. Search these after
# musl's headers so that host libc headers do not replace the sysroot headers.
set(PTSAN_LINUX_INCLUDE_DIRS "/usr/include/${ptsan_musl_arch}-linux-gnu;/usr/include"
  CACHE STRING "Linux UAPI include directories for the musl dependency builds")
set(ptsan_linux_flags "")
foreach(dir IN LISTS PTSAN_LINUX_INCLUDE_DIRS)
  string(APPEND ptsan_linux_flags " -idirafter ${dir}")
endforeach()

find_program(PTSAN_MAKE NAMES gmake make REQUIRED)
include(ProcessorCount)
ProcessorCount(ptsan_musl_default_jobs)
if(ptsan_musl_default_jobs EQUAL 0)
  set(ptsan_musl_default_jobs 1)
endif()
set(PTSAN_MUSL_BUILD_JOBS "${ptsan_musl_default_jobs}" CACHE STRING
  "Parallel jobs per musl libc build (defaults to the available CPU count)")
if(NOT PTSAN_MUSL_BUILD_JOBS MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "PTSAN_MUSL_BUILD_JOBS must be a positive integer")
endif()
set(ptsan_plain_root "${PTSAN_DEPENDENCIES_DIR}/musl-plain")
set(ptsan_musl_runtime_root "${PTSAN_DEPENDENCIES_DIR}/musl-runtime")
set(ptsan_overlay "${ptsan_source}/compiler-rt/lib/ptsan/musl_overlay")

function(ptsan_add_musl name binary install instrumented)
  cmake_parse_arguments(ARG "" "CFLAGS" "" ${ARGN})
  set(compiler "${ptsan_clang}")
  set(flags "${ARG_CFLAGS}")
  if(instrumented)
    string(APPEND compiler " --target=${ptsan_musl_target}")
    set(flags "-O2 -iquote ${ptsan_overlay} ${ptsan_flags}")
  endif()
  # musl's makefiles do not track compiler or command-line changes themselves.
  # Target-specific flags also preserve exceptions on incremental rebuilds.
  set(rules
    "$(ALL_OBJS) $(ALL_OBJS:.o=.lo): config.mak ${binary}/ptsan.mk ${ptsan_clang}\n")
  if(instrumented)
    string(APPEND rules
      "$(ALL_OBJS) $(ALL_OBJS:.o=.lo): $(wildcard ${ptsan_overlay}/*.h)\n")
    # Bootstrap runs before TLS/global proxies exist. Exit reads trusted linker
    # and libc state; the finalizers it calls retain their instrumentation.
    set(startup
      obj/crt/crt1.o
      obj/crt/rcrt1.o
      obj/src/env/__libc_start_main
      obj/src/env/__init_tls
      obj/src/env/__stack_chk_fail
      obj/src/exit/exit
      obj/src/thread/__tls_get_addr)
    if(ptsan_musl_arch STREQUAL "aarch64")
      # AArch64 uses C memmove; wrappers and backend libcalls pass raw pointers.
      # Keep the primitive raw, just like x86's assembly implementation.
      # __set_thread_area establishes the raw hardware thread pointer.
      list(APPEND startup
        obj/src/thread/aarch64/__set_thread_area
        obj/src/string/memmove)
    elseif(ptsan_musl_arch STREQUAL "x86_64")
      # x86-64 implements these primitives in uninstrumented assembly.
    else()
      message(FATAL_ERROR "Unsupported PTSan musl architecture '${ptsan_musl_arch}'")
    endif()
    foreach(obj IN LISTS startup)
      if(obj MATCHES "\\.o$")
        string(APPEND rules "${obj}: CFLAGS = -O2\n")
      else()
        string(APPEND rules "${obj}.o ${obj}.lo: CFLAGS = -O2\n")
      endif()
    endforeach()
    # Keep metadata propagation, but omit checks for allocator metadata outside
    # object bounds, syscall's unused varargs, string scans past terminators,
    # and related libc internals. Wrappers check user ranges.
    foreach(obj
        src/malloc/mallocng/free src/malloc/mallocng/realloc
        src/malloc/mallocng/malloc_usable_size src/misc/syscall
        src/string/stpcpy src/string/stpncpy src/string/strlcpy
        src/string/memccpy src/malloc/mallocng/aligned_alloc)
      string(APPEND rules
        "obj/${obj}.o obj/${obj}.lo: CFLAGS += -mllvm -ptsan-nochecks\n")
    endforeach()
    foreach(obj src/malloc/mallocng/realloc src/malloc/mallocng/aligned_alloc)
      string(APPEND rules
        "obj/${obj}.o obj/${obj}.lo: CFLAGS += -mllvm -ptsan-strip-ptrtoint\n")
    endforeach()
  endif()
  file(GENERATE OUTPUT "${binary}/ptsan.mk" CONTENT "${rules}")
  ExternalProject_Add(${name}-build
    SOURCE_DIR "${PTSAN_MUSL_SOURCE_DIR}"
    BINARY_DIR "${binary}"
    PREFIX "${binary}/external-project"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    EXCLUDE_FROM_ALL TRUE
    DEPENDS clang llvm-ar llvm-ranlib
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --disable-shared
      --prefix=${install} "CC=${compiler}" "CFLAGS=${flags}"
      AR=${LLVM_RUNTIME_OUTPUT_INTDIR}/llvm-ar
      RANLIB=${LLVM_RUNTIME_OUTPUT_INTDIR}/llvm-ranlib
    BUILD_COMMAND ${PTSAN_MAKE} -j${PTSAN_MUSL_BUILD_JOBS}
      -s -f Makefile -f ptsan.mk all
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env --unset=DESTDIR
      ${PTSAN_MAKE} -s -f Makefile -f ptsan.mk install
    BUILD_ALWAYS TRUE
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
    USES_TERMINAL_INSTALL TRUE)
  ExternalProject_Add_StepDependencies(${name}-build configure
    "${PTSAN_MUSL_SOURCE_DIR}/configure" "$<TARGET_FILE:clang>")
  # Dependents need the installed sysroot before configuring, but an unchanged
  # make invocation must not force them to reconfigure through its done stamp.
  add_custom_target(${name} DEPENDS ${name}-build)
endfunction()

ptsan_add_musl(ptsan-musl-plain
  "${ptsan_plain_root}/build" "${ptsan_plain_root}/install" FALSE)

set(ptsan_runtime_flags
  "--sysroot=${ptsan_plain_root}/install${ptsan_linux_flags}")
ptsan_add_cmake_project(ptsan-musl-runtime
  "${ptsan_source}/compiler-rt" "${ptsan_musl_runtime_root}"
  "${ptsan_caches}/PTSan-musl-runtime.cmake"
  DEPENDS ptsan-musl-plain
  CMAKE_ARGS
    -DPTSAN_MUSL_TARGET=${ptsan_musl_target}
    -DLLVM_CMAKE_DIR=${LLVM_BINARY_DIR}/lib/cmake/llvm
    "-DCMAKE_C_FLAGS=${ptsan_runtime_flags}"
    "-DCMAKE_CXX_FLAGS=${ptsan_runtime_flags}"
    -DCOMPILER_RT_PTSAN_MUSL_SOURCE_DIR=${PTSAN_MUSL_SOURCE_DIR}
  TARGETS ptsan builtins)

# Only the runtime payload belongs in the install tree, not its CMake build.
install(DIRECTORY "${ptsan_musl_runtime_root}/lib/linux/"
  DESTINATION "${ptsan_install_root}/musl-runtime/lib/linux"
  COMPONENT ptsan-dependencies
  FILES_MATCHING PATTERN "libclang_rt.ptsan*.a*"
    PATTERN "libclang_rt.builtins*.a" PATTERN "ptsan-init-ro.ld")

# Each encoding/strictness variant has matching libc and C++ libraries.
# All variants share the plain musl headers and uninstrumented PTSan runtime.
set(ptsan_musl_targets)
set(ptsan_musl_cxx_targets)
set(ptsan_musl_modes strict nonstrict id32-strict id32-nonstrict id32-strict-indicator)
if(PTSAN_BUILD_BASELINE_DEPENDENCIES)
  list(APPEND ptsan_musl_modes baseline)
endif()
if(PTSAN_BUILD_LAM_DEPENDENCIES)
  list(APPEND ptsan_musl_modes strict-lam nonstrict-lam)
endif()
foreach(ptsan_mode IN LISTS ptsan_musl_modes)
  set(ptsan_flags "-fsanitize=ptsan")
  set(ptsan_instrument_libc TRUE)
  set(ptsan_cxx_cache "${ptsan_caches}/PTSan-musl-cxx.cmake")
  set(ptsan_cxx_extra_args "")
  if(ptsan_mode STREQUAL "baseline")
    set(ptsan_flags "")
    set(ptsan_instrument_libc FALSE)
    set(ptsan_cxx_cache "${ptsan_caches}/PTSan-musl-baseline-cxx.cmake")
    set(ptsan_cxx_extra_args -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)
  endif()
  if(ptsan_mode MATCHES "^id32-")
    string(APPEND ptsan_flags
      " -fsanitize-ptsan-32-bit-ids -mllvm -ptsan-stack-temporal-safety=${PTSAN_ID32_STACK_TEMPORAL_SAFETY}")
  endif()
  if(ptsan_mode MATCHES "^(id32-)?strict")
    string(APPEND ptsan_flags " -fsanitize-ptsan-strict-mode")
  endif()
  if(ptsan_mode MATCHES "-lam$")
    string(APPEND ptsan_flags " -fsanitize-ptsan-lam")
  endif()
  if(ptsan_mode STREQUAL "id32-strict-indicator")
    string(APPEND ptsan_flags " -mllvm -ptsan-support-indicator-bit")
  endif()
  set(ptsan_musl_root "${PTSAN_DEPENDENCIES_DIR}/musl-${ptsan_mode}")

  ptsan_add_musl(ptsan-musl-${ptsan_mode}-libc
    "${ptsan_musl_root}/libc-build" "${ptsan_musl_root}/libc"
    ${ptsan_instrument_libc} CFLAGS -O2)
  # musl's generated compiler wrappers embed build paths. Applications use our
  # Clang configs; install just the headers, libraries, and startup objects.
  install(DIRECTORY "${ptsan_musl_root}/libc/include" "${ptsan_musl_root}/libc/lib"
    DESTINATION "${ptsan_install_root}/musl-${ptsan_mode}/libc"
    COMPONENT ptsan-dependencies)

  set(ptsan_musl_config_root "${ptsan_config_root}/musl-${ptsan_mode}")
  set(ptsan_musl_c_config
    "${LLVM_RUNTIME_OUTPUT_INTDIR}/ptsan-musl-${ptsan_mode}-c.cfg")
  set(ptsan_musl_toolchain_config
    "${LLVM_RUNTIME_OUTPUT_INTDIR}/ptsan-musl-${ptsan_mode}-toolchain.cfg")
  set(ptsan_musl_install_config
    "${ptsan_install_config_dir}/ptsan-musl-${ptsan_mode}-toolchain.cfg")
  # Applications and tests use the same Clang linking path on both architectures.
  # Keep instrumentation policy out of the toolchain config: application configs
  # supply defaults, while lit RUN lines select the test's strictness.
  ptsan_generate_config(ptsan-musl-${ptsan_mode}-toolchain.cfg
    "${ptsan_config_header}\
--target=${ptsan_musl_target}
--sysroot=\"${ptsan_musl_config_root}/libc\"
-resource-dir=\"${ptsan_config_root}/musl-runtime\"
--gcc-install-dir=\"@PTSAN_GCC_INSTALL_DIR@\"
--rtlib=libgcc
-B\"${ptsan_musl_config_root}/libc/lib\"
-nostdinc
-isystem \"${ptsan_musl_config_root}/libc/include\"
-idirafter \"@PTSAN_CLANG_BUILTIN_HEADERS@\"
-L\"${ptsan_musl_config_root}/libc/lib\"
-static
" MUSL_TOOLCHAIN)
  ptsan_generate_config(ptsan-musl-${ptsan_mode}-c.cfg
    "${ptsan_config_header}\
@ptsan-musl-${ptsan_mode}-toolchain.cfg
${ptsan_flags}
")
  # Query the compiler after it has been built. musl's wrapper uses the same host
  # GCC startup objects and support libraries; a sysroot alone cannot find them.
  set(ptsan_config_script "${CMAKE_CURRENT_LIST_DIR}/LLVMPTSanConfigureMusl.cmake")
  add_custom_command(OUTPUT "${ptsan_musl_toolchain_config}" "${ptsan_musl_install_config}"
    COMMAND ${CMAKE_COMMAND}
      -DPTSAN_CLANG=$<TARGET_FILE:clang>
      -DPTSAN_MUSL_TARGET=${ptsan_musl_target}
      -DPTSAN_CONFIG_INPUT=${ptsan_musl_toolchain_config}.in
      -DPTSAN_CONFIG_OUTPUT=${ptsan_musl_toolchain_config}
      -DPTSAN_INSTALL_CONFIG_OUTPUT=${ptsan_musl_install_config}
      "-DPTSAN_INSTALL_BUILTIN_HEADERS=<CFGDIR>/../${ptsan_install_resource_dir}/include"
      -P "${ptsan_config_script}"
    DEPENDS clang "${ptsan_musl_toolchain_config}.in"
      "${ptsan_musl_install_config}.in" "${ptsan_config_script}"
      "${CMAKE_CURRENT_LIST_DIR}/LLVMPTSanMuslTarget.cmake"
    VERBATIM)
  add_custom_target(ptsan-musl-${ptsan_mode}-config
    DEPENDS "${ptsan_musl_toolchain_config}" "${ptsan_musl_c_config}"
      "${ptsan_musl_install_config}")
  add_custom_target(ptsan-musl-${ptsan_mode}
    DEPENDS ptsan-musl-runtime ptsan-musl-${ptsan_mode}-libc
      ptsan-musl-${ptsan_mode}-config)
  set_target_properties(ptsan-musl-${ptsan_mode} PROPERTIES
    PTSAN_MUSL_COMPILER "${ptsan_clang}"
    PTSAN_MUSL_CONFIG "${ptsan_musl_toolchain_config}")

  set(ptsan_musl_cxx_options "\
-I\"${ptsan_musl_config_root}/cxx/include/c++/v1\"
-stdlib=libc++
-L\"${ptsan_musl_config_root}/cxx/lib\"
# Let the driver add LLVM's unwinder after application objects. Forcing an
# archive from the config would put its PTSan runtime flags before theirs.
--rtlib=compiler-rt
--unwindlib=libunwind
")
  ptsan_generate_config(ptsan-musl-${ptsan_mode}.cfg
    "${ptsan_config_header}@ptsan-musl-${ptsan_mode}-c.cfg\n${ptsan_musl_cxx_options}")
  # Tests select application instrumentation in their RUN lines, independently of
  # the libc/C++ dependency mode. Do not inherit the application configuration.
  set(ptsan_musl_cxx_toolchain_config
    "${LLVM_RUNTIME_OUTPUT_INTDIR}/ptsan-musl-${ptsan_mode}-cxx-toolchain.cfg")
  ptsan_generate_config(ptsan-musl-${ptsan_mode}-cxx-toolchain.cfg
    "${ptsan_config_header}@ptsan-musl-${ptsan_mode}-toolchain.cfg\n${ptsan_musl_cxx_options}")

  set(ptsan_cxx_flags
    "--sysroot=${ptsan_musl_root}/libc${ptsan_linux_flags} ${ptsan_flags}")
  ptsan_add_cmake_project(ptsan-musl-${ptsan_mode}-cxx
    "${ptsan_source}/runtimes" "${ptsan_musl_root}/cxx-build"
    "${ptsan_cxx_cache}"
    DEPENDS ptsan-musl-${ptsan_mode}
    CMAKE_ARGS ${ptsan_cxx_extra_args}
      -DPTSAN_MUSL_TARGET=${ptsan_musl_target}
      -DCMAKE_INSTALL_PREFIX=${ptsan_musl_root}/cxx
      "-DCMAKE_C_FLAGS=${ptsan_cxx_flags}"
      "-DCMAKE_CXX_FLAGS=${ptsan_cxx_flags}"
    TARGETS ${ptsan_cxx_targets})
  set_target_properties(ptsan-musl-${ptsan_mode}-cxx PROPERTIES
    PTSAN_MUSL_CXX_CONFIG "${ptsan_musl_cxx_toolchain_config}")
  install(DIRECTORY "${ptsan_musl_root}/cxx/"
    DESTINATION "${ptsan_install_root}/musl-${ptsan_mode}/cxx"
    COMPONENT ptsan-dependencies)
  list(APPEND ptsan_musl_targets ptsan-musl-${ptsan_mode})
  list(APPEND ptsan_musl_cxx_targets ptsan-musl-${ptsan_mode}-cxx)
endforeach()

add_custom_target(ptsan-musl DEPENDS ${ptsan_musl_targets})
add_custom_target(ptsan-musl-cxx DEPENDS ${ptsan_musl_cxx_targets})
add_dependencies(ptsan-dependencies ptsan-musl-cxx)

if(PTSAN_BUILD_LAM_DEPENDENCIES)
  add_custom_target(ptsan-musl-lam
    DEPENDS ptsan-musl-strict-lam ptsan-musl-nonstrict-lam)
  add_custom_target(ptsan-musl-lam-cxx
    DEPENDS ptsan-musl-strict-lam-cxx ptsan-musl-nonstrict-lam-cxx)
  add_dependencies(ptsan-dependencies ptsan-musl-lam-cxx)
endif()

if(PTSAN_BUILD_BASELINE_DEPENDENCIES)
  add_dependencies(ptsan-dependencies ptsan-musl-baseline-cxx)
endif()
