//===-- PTSanOptimizationInternal.h - PTSan optimization internals -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "PTSanPhases.h"

namespace llvm::ptsan {

struct MarkerBaseState {
  SmallVector<AccessRangePtr> Ranges;
  // Anchors for preanalysis checks inserted by this marker.
  // Necessary because we might have to roll them back
  SmallVector<CheckAnchor> PreanalysisAnchors;
  bool Used = false;
};

struct MarkerEnvironment {
  LoopPreheaderAnchor Marker;
  DenseMap<Value *, MarkerBaseState> Bases;

  MarkerEnvironment(LoopPreheaderAnchor Marker) : Marker(Marker) {}
};

struct LoopSummaryState {
  SmallVector<MarkerEnvironment, 2> Environments;
  // All bases handled by this loop summary. This is a union of those
  // found in each environment. It's here for easy reference
  DenseSet<Value *> SolvedBases;
  bool Complete = false;
};

class PTSanOptimizer;

class PTSanPreanalysisElider {
  PTSanOptimizer &O;
  PTSanAnalysisState &PTA;
  PointerTagSanitizer &DS;
  DenseMap<uint64_t, LoopSummaryState> SummariesByLoopID;

  std::optional<MarkerBaseState>
  tryMaterializeBase(HoistTarget EnvironmentTarget, Value *Base,
                     SmallVector<AccessRangePtr> Ranges);
  LoopSummaryState materializeSummary(ArrayRef<LoopPreheaderAnchor> Markers);
  SmallDenseSet<uint64_t, 4>
  getAccessLoopIDs(const PlaceablePtrCheck &Check);
  std::optional<unsigned>
  getElisionDepth(const PlaceablePtrCheck &Check,
                  LoopSummaryState &Summary);
  void recordElision(const PlaceablePtrCheck &Check, LoopSummaryState &Summary);

public:
  PTSanPreanalysisElider(PTSanOptimizer &Op);

  void materializeChecks();
  void elideCoveredChecks();
  void eraseUnusedChecks();
};

class PTSanOptimizer {
  friend class PTSanPreanalysisElider;

private:
  PTSanAnalysisState &PTA;
  PointerTagSanitizer &DS;
  DominatorTree &DT;
  PostDominatorTree &PDT;
  LoopInfo &LI;
  ScalarEvolution &SE;
  PTSanUtils &PTU;
  // Optimization inserts/moves anchors, not potentially freeing program calls.
  SmallVector<Instruction *, 16> BlockingCalls;

  bool isBlockingCall(Instruction &I);
  optional<StaticObjectBounds> getStaticBounds(Value *V);
  bool isUnfreeableBase(Value *V);
  HoistTarget hoistBoundsLoad(Instruction *Start, Value *BasePtr);
  SmallPtrSet<Instruction *, 32> getValidCheckIPs(HoistTarget MustDominate,
                                                  Instruction *User,
                                                  AccessRange &Range);
  optional<PlaceablePtrCheck>
  buildLoopHoistedCheck(const PlaceablePtrCheck &Check);
  void applyLoopCheckHoisting();
  void hoistLoadBoundAnchors();
  void mergeLoadBoundAnchors();
  void foldStaticBoundAnchors();
  BoundedPtrCheck analyzePtrAccess(AccessAnchor Access);
  void eraseCheckAnchor(CheckAnchor Anchor);
  bool isRemovableCheck(const BoundedPtrCheck &Op);
  bool shouldEraseRemovableCheck(const BoundedPtrCheck &Op);
  void eraseRemovableChecks();

public:
  PTSanOptimizer(PTSanAnalysisState &PTA);

  void run();
  PlaceablePtrCheck placePtrSink(const BoundedPtrCheck &Check);

  bool mergeOrderableChecks();
  bool mergeSubsumingChecks();
};

} // namespace llvm::ptsan
