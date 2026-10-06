#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# Opt-in hook for a PTSan runtimes build, via CMAKE_PROJECT_Runtimes_INCLUDE.
# The unwinder is trusted to read raw ELF metadata and saved machine state.
# Keep metadata propagation, but disable checks in its C/C++ bodies and
# libc++abi's exception handling and RTTI traversal.
function(ptsan_configure_unwinder)
  foreach(target unwind_shared_objects unwind_static_objects)
    if(TARGET ${target})
      # PRIVATE matters: these object targets are also linked into libc++abi.
      target_compile_options(${target} PRIVATE
        "$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-mllvm -ptsan-nochecks>")
    endif()
  endforeach()

  if(TARGET cxxabi_shared_objects)
    # Source options cover both libc++abi object targets without disabling
    # checks in other ABI code. RTTI traversal also implements dynamic_cast;
    # its reads through invalid object pointers are a known unchecked boundary.
    get_target_property(abi_source_dir cxxabi_shared_objects SOURCE_DIR)
    set_property(SOURCE
      "${abi_source_dir}/cxa_personality.cpp"
      "${abi_source_dir}/cxa_exception.cpp"
      "${abi_source_dir}/cxa_exception_storage.cpp"
      "${abi_source_dir}/private_typeinfo.cpp"
      DIRECTORY "${abi_source_dir}"
      APPEND PROPERTY COMPILE_OPTIONS -mllvm -ptsan-nochecks)
  endif()
endfunction()

# project(Runtimes) runs before the library targets have been created.
cmake_language(DEFER CALL ptsan_configure_unwinder)
