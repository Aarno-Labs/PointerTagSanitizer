//===-- PTSanIDStrip.h - Strip out PTsan Ids without doing anything else-*- C++ -*-===//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

class PTSanIDStripPass : public PassInfoMixin<PTSanIDStripPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm
