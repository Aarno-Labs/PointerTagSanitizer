//===-- PTSanPhases.h - Private PTSan phase declarations -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "PTSanAnchors.h"

namespace llvm::ptsan {


class PTSanAnalysisState {
public:
  Function &F;
  PointerTagSanitizer &DS;

  PTSanAnalysisState(Function &F, PointerTagSanitizer &DS,
                          TargetLibraryInfo &TLI)
      : F(F), DS(DS), DT(F), PDT(F),
        DTU(DT, PDT, DomTreeUpdater::UpdateStrategy::Eager), LI(DT), AC(F),
        TLI(TLI), SE(F, TLI, AC, DT, LI),
        PTU(F, DS.Ctx, DT, PDT, LI, SE) {};

  DominatorTree DT;
  PostDominatorTree PDT;
  DomTreeUpdater DTU;
  LoopInfo LI;
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

public:
  PTSanAnchorOptimizer(PTSanAnalysisState &PTA)
      : PTA(PTA), DS(PTA.DS), DT(PTA.DT), PTU(PTA.PTU) {}

  void hoistStripAnchors();
  void mergeStripAnchors();
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

typedef DenseMap<const Loop *, std::pair<bool, DenseSet<Value *>>>
    KnownLoopBoundsTy;

using PreanalysisElisionKey = std::pair<const Loop *, Value *>;
using PreanalysisElisionSet = DenseSet<PreanalysisElisionKey>;

class PTSanPreparationPass {
public:
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
  bool run(PTSanAnalysisState &PTA);
};

} // namespace llvm::ptsan
