//===-- PTSanStripIDHoisting.h - Strip-ID hoisting helpers -----*- C++ -*-===//
//===----------------------------------------------------------------------===//

#pragma once

#include "../PTSanPhases.h"

namespace llvm::ptsan {

bool isKnownSafeIntrinsicUse(CallBase &CB);
void runLocalStripIDHoisting(PTSanAnalysisState &PTA);
void runMinCutStripIDHoisting(PTSanAnalysisState &PTA);

} // namespace llvm::ptsan
