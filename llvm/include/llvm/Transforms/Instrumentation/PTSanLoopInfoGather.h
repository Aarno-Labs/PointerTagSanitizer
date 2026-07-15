
#pragma once

#include "llvm/IR/PassManager.h"

namespace llvm {

struct PTSanLoopInfoGatherPass : PassInfoMixin<PTSanLoopInfoGatherPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm
