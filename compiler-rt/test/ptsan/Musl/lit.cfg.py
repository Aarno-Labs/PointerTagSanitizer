#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# type: ignore
# -*- Python -*-

import os
import shlex


# PTSan tests use shell loops, assignments, and compound redirections.
# LLVM 23 defaults to lit's internal shell; retain the native shell here.
import lit.formats
config.test_format = lit.formats.ShTest(
    execute_external=True, force_execute_external=True)
config.available_features.add("shell")

config.name = "PointerTagSanitizerMusl" + config.name_suffix
config.test_source_root = os.path.dirname(__file__)

compile_flags = [
    "-O2",
    "-g",
    "-fno-discard-value-names",
    "-fsanitize=ptsan",
    "-mllvm",
    "-ptsan-abort-on-error",
]


def build_invocation(toolchain_config, mode, cxx=False, validate=True):
    flags = ["--config=" + toolchain_config] + compile_flags
    if validate:
        flags += ["-fsanitize-ptsan-runtime-validate"]
    flags += ["-fno-sanitize-ptsan-strict-mode" if "nonstrict" in mode
              else "-fsanitize-ptsan-strict-mode"]
    if mode.startswith("id32-"):
        flags += ["-fsanitize-ptsan-32-bit-ids", "-DPTSAN_TEST_32_BIT_IDS"]
    if mode.endswith("-indicator"):
        flags += ["-mllvm", "-ptsan-support-indicator-bit"]
    if cxx:
        flags += ["--driver-mode=g++"]
    return " " + " ".join(
        shlex.quote(arg) for arg in [config.musl_compiler] + flags
    ) + " "


# Unsuffixed strict/nonstrict commands always use ID16. Each ID32 command
# selects both the application flags and the matching libc/C++ dependencies.
for mode, (c_config, cxx_config) in config.musl_configs.items():
    name = mode.replace("-", "_")
    config.substitutions.extend([
        ("%ptsan_musl_" + name + "_regular ",
         build_invocation(c_config, mode, validate=False)),
        ("%ptsan_musl_" + name + " ", build_invocation(c_config, mode)),
        ("%ptsanxx_musl_" + name + " ",
         build_invocation(cxx_config, mode, cxx=True)),
    ])
config.substitutions.append(("%exe", "a.out"))

config.suffixes = [".c", ".cpp"]
config.excludes.add("Inputs")
config.available_features.add("ptsan-instrumented-musl")

# Native Linux static configurations.
if config.target_os != "Linux" or config.host_arch not in ["x86_64", "aarch64"]:
    config.unsupported = True
