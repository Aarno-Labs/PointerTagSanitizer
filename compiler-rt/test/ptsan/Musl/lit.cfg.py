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
    "--target=x86_64-linux-musl",
    "-resource-dir",
    config.musl_resource_dir,
    "-static",
    "-O2",
    "-g",
    "-fno-discard-value-names",
    "-fsanitize=ptsan",
    "-mllvm",
    "-ptsan-runtime-validate",
    "-mllvm",
    "-ptsan-abort-on-error",
]


def build_invocation(flags):
    return " " + " ".join(
        shlex.quote(arg) for arg in [config.musl_compiler] + flags
    ) + " "


config.substitutions.append(
    ("%clang_ptsan_musl ", build_invocation(compile_flags))
)
config.substitutions.append(("%exe", "a.out"))

config.suffixes = [".c"]
config.available_features.add("ptsan-instrumented-musl")

# The first runtime configuration is deliberately x86-64 and static-only.
if config.host_os != "Linux" or config.host_arch != "x86_64":
    config.unsupported = True
