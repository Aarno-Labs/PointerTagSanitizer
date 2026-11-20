//===-- PTSanPhases.h - Private PTSan phase declarations -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "PTSanAnchors.h"
#include "llvm/IR/CycleInfo.h"

namespace llvm::ptsan {


class PTSanAnalysisState {
public:
  Function &F;
  PointerTagSanitizer &DS;

  // Function-local compatibility policies, shared by discovery and optimization.
  // Lists enable a policy in addition to its module-wide command-line setting.
  bool StripPtrToInt;
  bool ClearIDsOnArith;
  bool ClearIDsOnCmp;

  PTSanAnalysisState(Function &F, PointerTagSanitizer &DS,
                          TargetLibraryInfo &TLI)
      : F(F), DS(DS),
        StripPtrToInt(ClStripPtrToInt ||
                      llvm::is_contained(ClStripPtrToIntFunctions, F.getName())),
        ClearIDsOnArith(ClClearIdsOnArith ||
                        llvm::is_contained(ClClearIdsOnArithFunctions,
                                           F.getName())),
        ClearIDsOnCmp(ClClearIdsOnCmp ||
                      llvm::is_contained(ClClearIdsOnCmpFunctions, F.getName())),
        DT(F), PDT(F),
        DTU(DT, PDT, DomTreeUpdater::UpdateStrategy::Eager), LI(DT), AC(F),
        TLI(TLI), SE(F, TLI, AC, DT, LI),
        PTU(F, DS.Ctx, DT, PDT, LI, SE) {
    CI.compute(F);
  }

  DominatorTree DT;
  PostDominatorTree PDT;
  DomTreeUpdater DTU;
  LoopInfo LI;
  CycleInfo CI;
  AssumptionCache AC;
  TargetLibraryInfo TLI;
  ScalarEvolution SE;
  PTSanUtils PTU;
  std::unordered_map<CallInst *, PlaceablePtrCheck> PlaceablePtrChecks;
};

class PTSanAnchorOptimizer {
private:
  PTSanAnalysisState &PTA;
  PointerTagSanitizer &DS;
  DominatorTree &DT;
  PTSanUtils &PTU;

  void sinkAnchor(CallInst *Call);

public:
  PTSanAnchorOptimizer(PTSanAnalysisState &PTA)
      : PTA(PTA), DS(PTA.DS), DT(PTA.DT), PTU(PTA.PTU) {}

  void hoistStripAnchors();
  void sinkStripAnchors();
  void sinkLoadBoundAnchors();
  void mergeStripAnchors();
  void foldRedundantStripAnchors();
  void hoistGetIDAnchors();
  void mergeGetIDAnchors();
  void foldStripOfTagAnchors();
  void foldTagOfStripAnchors();
  void foldGetIDOfTagAnchors();
  void eraseUnusedDependencyAnchors();
};

bool willFoldStripOfTagAnchor(PointerTagSanitizer &DS, StripAnchor Strip);
bool willFoldTagOfStripAnchor(PointerTagSanitizer &DS, TagAnchor Tag);
bool willFoldGetIDOfTagAnchor(PointerTagSanitizer &DS, TaggedAnchor Tagged);

class PTSanPreparationPass {
public:
  bool run(Module &M);
  bool run(PTSanAnalysisState &PTA);
};

class PTSanDiscoveryPass {
public:
  bool run(PTSanAnalysisState &PTA);
};

class PTSanOptimizationPass {
public:
  bool run(PTSanAnalysisState &PTA);
};

void optimizeLoadBoundAnchors(PTSanAnalysisState &PTA);

class PTSanMetadataLoweringPass {
public:
  bool run(PTSanAnalysisState &PTA);
};

class PTSanLateOptimizationPass {
public:
  bool run(PTSanAnalysisState &PTA);
};

class PTSanLoweringPass {
public:
  bool run(PTSanAnalysisState &PTA, FunctionAnalysisManager &FAM);
};

} // namespace llvm::ptsan
