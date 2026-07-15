//===-- PointerTagSanitizer.h - Track pointer bounds by tagging upper bits----*- C++ -*-===//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/IR/PassManager.h"
#include "llvm/Support/CommandLine.h"

namespace llvm {

class PointerTagSanitizerPass : public PassInfoMixin<PointerTagSanitizerPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
