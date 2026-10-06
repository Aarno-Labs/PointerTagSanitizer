//===-- PTSanPreanalysisElider.cpp - Elide preanalyzed checks ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanOptimizationInternal.h"

namespace llvm::ptsan {

PTSanPreanalysisElider::PTSanPreanalysisElider(PTSanOptimizer &Op)
    : O(Op), PTA(Op.PTA), DS(Op.DS) {}

std::optional<MarkerBaseState>
PTSanPreanalysisElider::tryMaterializeBase(HoistTarget EnvironmentTarget,
                                           Value *Base,
                                           SmallVector<AccessRangePtr> Ranges) {
  MarkerBaseState State;
  State.Ranges = std::move(Ranges);
  DBG(errs() << "\nAnalyzing preanalyzed uses of " << *Base << "\n");

  Value *False = ConstantInt::getFalse(PTA.F.getContext());
  Instruction *FakeUse = SelectInst::Create(False, Base, Base, "ptsan.fake.use",
                                            EnvironmentTarget.getIP());
  DBG(errs() << "Target instruction is " << *FakeUse << "\n");
  HoistTarget BoundsTarget = PTA.PTU.getFirstCommonInsertionPoint(
      {O.hoistBoundsLoad(FakeUse, Base), EnvironmentTarget});

  assert(Base->getType()->isPointerTy());

  SmallVector<std::pair<AccessRangePtr, SmallPtrSet<Instruction *, 32>>, 0>
      Placements;
  for (AccessRangePtr Range : State.Ranges) {
    auto CheckIPs = O.getValidCheckIPs(BoundsTarget, FakeUse, *Range);
    // XXX: Support partial hoists for a given base
    if (CheckIPs.empty()) {
      return std::nullopt;
    }
    Placements.emplace_back(Range, std::move(CheckIPs));
  }

  for (auto &[Range, CheckIPs] : Placements) {
    Instruction *IP = PlaceablePtrCheck::getCheckIP(CheckIPs, PTA.DT);
    Instruction *BoundsIP = BoundsTarget.getIP();
    GetIDAnchor ID = GetIDAnchor::create(DS, BoundsIP, Base);
    BoundAnchor BaseBound = LoadBoundAnchor::createBase(DS, BoundsIP, ID);
    BoundAnchor SpanBound = LoadBoundAnchor::createSpan(DS, BoundsIP, ID);
    LoopPreanalysisAnchor Anchor =
        LoopPreanalysisAnchor::create(DS, IP, ID, BaseBound, SpanBound);
    PtrOp Op(FakeUse, 1, CHK_TY::PREANALYZED_LOOP, nullptr);
    BoundedPtrCheck BoundedCheck(Anchor, Op, Base, Range);
    // If this check is trivial, remove it
    if (O.shouldEraseRemovableCheck(BoundedCheck)) {
      O.eraseCheckAnchor(Anchor);
      continue;
    }

    PlaceablePtrCheck Check(BoundedCheck, CheckIPs, Range);
    PTA.PlaceablePtrChecks.insert({Anchor.Call, Check});
    State.PreanalysisAnchors.push_back(Anchor);
  }

  return State;
}

LoopSummaryState PTSanPreanalysisElider::materializeSummary(
    ArrayRef<LoopPreheaderAnchor> Markers) {
  LoopSummaryState Summary;
  Summary.Complete = true;
  DenseSet<Value *> FailedBases;

  for (LoopPreheaderAnchor Marker : Markers) {
    MarkerEnvironment Environment(Marker);
    DeserializeBoundsInfo DBI(PTA.SE, Marker.getCaptures());
    auto [Complete, BoundsInfo] =
        DBI.deserializeSolvedRanges(&Marker.getBoundsMetadata());
    Summary.Complete &= Complete;

    // Gather base -> ranges
    for (auto &[RawBase, Ranges] : BoundsInfo) {
      Value *Base = findPTSanBasePointer(RawBase, PTA.PTU);
      MarkerBaseState &State = Environment.Bases[Base];
      State.Ranges.append(Ranges.begin(), Ranges.end());
    }

    // Actually materialize each base's full set of ranges
    for (auto &[Base, State] : Environment.Bases) {
      if (FailedBases.contains(Base)) {
        continue;
      }
      auto Materialized =
          tryMaterializeBase({Marker.Call, AFTER}, Base,
                             std::move(State.Ranges));
      if (!Materialized) {
        FailedBases.insert(Base);
        Summary.Complete = false;
        continue;
      }
      State = std::move(*Materialized);
    }

    Summary.Environments.push_back(std::move(Environment));
  }

  // Roll back any failed materlaizations
  for (Value *Base : FailedBases) {
    for (MarkerEnvironment &Environment : Summary.Environments) {
      if (!Environment.Bases.contains(Base)) {
        continue;
      }
      for (CheckAnchor Anchor : Environment.Bases[Base].PreanalysisAnchors) {
        O.eraseCheckAnchor(Anchor);
      }
      Environment.Bases.erase(Base);
    }
  }

  // Summarize solved bases
  for (MarkerEnvironment &Environment : Summary.Environments) {
    for (Value *Base : llvm::make_first_range(Environment.Bases)) {
      Summary.SolvedBases.insert(Base);
    }
  }

  return Summary;
}

// Entry point for preanalysis check materialization
void PTSanPreanalysisElider::materializeChecks() {
  if (!ClPTSanPreanalyzeLoops || ClNoPTSanOpts) {
    return;
  }

  DenseMap<uint64_t, SmallVector<LoopPreheaderAnchor, 2>> MarkerGroups;
  for (auto Marker : DS.collectAnchors<LoopPreheaderAnchor>(PTA.F)) {
    MarkerGroups[Marker.getLoopID()].push_back(Marker);
  }

  for (auto &[ID, Markers] : MarkerGroups) {
    SummariesByLoopID.insert({ID, materializeSummary(Markers)});
  }
}

SmallDenseSet<uint64_t, 4>
PTSanPreanalysisElider::getAccessLoopIDs(const PlaceablePtrCheck &Check) {
  const Instruction &I = *cast<Instruction>(Check.U->getUser());
  SmallDenseSet<uint64_t, 4> IDs;
  for (uint64_t ID : getPTSanLoopIDs(I)) {
    IDs.insert(ID);
  }
  return IDs;
}

// Get the max depth we would have to insert a preanalysis check at to use this
// summary to elide this check. A bit awkward, but idk anything better
std::optional<unsigned>
PTSanPreanalysisElider::getElisionDepth(const PlaceablePtrCheck &Check,
                                        LoopSummaryState &Summary) {
  bool ExactBase = Summary.SolvedBases.contains(Check.BasePtr);
  if (!ExactBase && !Summary.Complete) {
    return std::nullopt;
  }

  unsigned Depth = 0;
  for (const MarkerEnvironment &Environment : Summary.Environments) {
    if (!ExactBase || Environment.Bases.contains(Check.BasePtr)) {
      Depth = std::max(
          Depth, PTA.LI.getLoopDepth(Environment.Marker.Call->getParent()));
    }
  }
  return Depth;
}

void PTSanPreanalysisElider::recordElision(const PlaceablePtrCheck &Check,
                                           LoopSummaryState &Summary) {
  if (Summary.SolvedBases.contains(Check.BasePtr)) {
    for (MarkerEnvironment &Environment : Summary.Environments) {
      if (Environment.Bases.contains(Check.BasePtr)) {
        Environment.Bases[Check.BasePtr].Used = true;
      }
    }
    return;
  }

  for (MarkerEnvironment &Environment : Summary.Environments) {
    for (MarkerBaseState &State : llvm::make_second_range(Environment.Bases)) {
      State.Used = true;
    }
  }
}

void PTSanPreanalysisElider::elideCoveredChecks() {
  DBG(errs() << "---------------Filtering preanalysis checks-----------\n");

  uint64_t skippedLoopOps = 0;
  uint64_t totalLoopOps = 0;
  for (AccessAnchor Access : DS.collectAnchors<AccessAnchor>(PTA.F)) {
    CallInst *Call = Access.Call;
    const PlaceablePtrCheck &Check = PTA.PlaceablePtrChecks.at(Call);
    SmallDenseSet<uint64_t, 4> IDs = getAccessLoopIDs(Check);
    if (IDs.empty()) {
      continue;
    }
    totalLoopOps++;

    // Iterate all loops this check belongs to. If any of them allow elision,
    // prioritize the outermost so we don't stick unnecessary preanalysis checks in inner
    // loops
    DBG(errs() << "Considering skipping loop use " << *Check.U->getUser() << "\n");
    LoopSummaryState *BestSummary = nullptr;
    unsigned BestDepth = std::numeric_limits<unsigned>::max();
    for (uint64_t ID : IDs) {
      if (!SummariesByLoopID.contains(ID)) {
        continue;
      }
      LoopSummaryState &Summary = SummariesByLoopID[ID];
      std::optional<unsigned> Depth = getElisionDepth(Check, Summary);
      if (Depth && *Depth < BestDepth) {
        BestSummary = &Summary;
        BestDepth = *Depth;
      }
    }

    if (BestSummary) {
      DBG(errs() << "Eliding loop op\n");
      skippedLoopOps++;
      recordElision(Check, *BestSummary);
      O.eraseCheckAnchor(Access);
    }
  }

  DBG(errs() << "Done with preanalysis filtering. Skipped " << skippedLoopOps
             << "/" << totalLoopOps << " loop ops\n");
}

void PTSanPreanalysisElider::eraseUnusedChecks() {
  for (LoopSummaryState &Summary : llvm::make_second_range(SummariesByLoopID)) {
    for (MarkerEnvironment &Environment : Summary.Environments) {
      for (MarkerBaseState &State :
           llvm::make_second_range(Environment.Bases)) {
        if (!State.Used) {
          for (CheckAnchor Anchor : State.PreanalysisAnchors) {
            O.eraseCheckAnchor(Anchor);
          }
        }
      }
    }
  }
}

} // namespace llvm::ptsan
