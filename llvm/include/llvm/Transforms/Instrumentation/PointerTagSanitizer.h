//===-- PointerTagSanitizer.h - Track bounds by tagging bits ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/VirtualFileSystem.h"

namespace llvm {

class PointerTagSanitizerPass : public PassInfoMixin<PointerTagSanitizerPass> {
  IntrusiveRefCntPtr<vfs::FileSystem> FS;

public:
  explicit PointerTagSanitizerPass(
      IntrusiveRefCntPtr<vfs::FileSystem> FS = vfs::getRealFileSystem())
      : FS(std::move(FS)) {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
