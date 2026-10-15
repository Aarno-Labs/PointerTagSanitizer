#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# type: ignore
# -*- Python -*-

import os
import shlex


config.name = "PointerTagSanitizerMusl" + config.name_suffix
config.test_source_root = os.path.dirname(__file__)

compile_flags = [
    "-O2",
    "-g",
    "-fno-discard-value-names",
    "-fsanitize=ptsan",
    "-mllvm",
    "-ptsan-runtime-validate",
    "-mllvm",
    "-ptsan-abort-on-error",
]


def build_invocation(toolchain_config, strict, cxx=False):
    flags = ["--config=" + toolchain_config] + compile_flags
    flags += ["-mllvm", "-ptsan-strict-mode=" + ("1" if strict else "0")]
    if cxx:
        flags += ["--driver-mode=g++"]
    return " " + " ".join(
        shlex.quote(arg) for arg in [config.musl_compiler] + flags
    ) + " "


# Each command pairs application instrumentation with matching dependencies.
config.substitutions.extend([
    (
        "%ptsan_musl_strict ",
        build_invocation(config.musl_strict_config, strict=True),
    ),
    (
        "%ptsan_musl_nonstrict ",
        build_invocation(config.musl_nonstrict_config, strict=False),
    ),
    (
        "%ptsanxx_musl_strict ",
        build_invocation(config.musl_strict_cxx_config, strict=True, cxx=True),
    ),
    (
        "%ptsanxx_musl_nonstrict ",
        build_invocation(config.musl_nonstrict_cxx_config, strict=False, cxx=True),
    ),
])
config.substitutions.append(("%exe", "a.out"))

config.suffixes = [".c", ".cpp"]
config.excludes.add("Inputs")
config.available_features.add("ptsan-instrumented-musl")

# Native Linux static configurations.
if config.host_os != "Linux" or config.host_arch not in ["x86_64", "aarch64"]:
    config.unsupported = True
