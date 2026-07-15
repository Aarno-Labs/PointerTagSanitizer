//===-- PTSanLateOptimization.cpp - Late PTSan optimization ---*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"
#include "PTSanUtils.h"
#include "Optimizations/PTSanStripIDHoisting.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/InstIterator.h"

namespace llvm::ptsan {

class PTSanLateOptimizer {
private:
  PTSanAnalysisState &PTA;
  PointerTagSanitizer &DS;
  Function &F;
  PTSanUtils &PTU;

  void insertTaggedUseAnchors();
  void materializeGlobalConstants();

public:
  PTSanLateOptimizer(PTSanAnalysisState &PTA)
      : PTA(PTA), DS(PTA.DS), F(PTA.F), PTU(PTA.PTU) {}

  void run();
};

bool PTSanLateOptimizationPass::run(PTSanAnalysisState &PTA) {
  PTSanLateOptimizer Optimizer(PTA);
  Optimizer.run();
  return true;
}

void PTSanLateOptimizer::insertTaggedUseAnchors() {
  SmallVector<Use *, 32> Uses;
  auto addPointerUse = [&](Use &U) {
    if (U->getType()->isPointerTy()) {
      Uses.push_back(&U);
    }
  };

  for (Instruction &I : instructions(F)) {
    if (auto ID = DS.getAsAnchor<GetIDAnchor>(&I)) {
      Uses.push_back(&ID->Call->getOperandUse(0));
      continue;
    }

    if (auto Check = DS.getAsAnchor<CheckAnchor>(&I)) {
      assert(Check->hasMaterializedRange());
      Uses.push_back(&Check->Call->getOperandUse(CheckAnchor::RangeMinIndex));
      continue;
    }

    if (auto *PtrToInt = dyn_cast<PtrToIntInst>(&I)) {
      addPointerUse(PtrToInt->getOperandUse(0));
      continue;
    }

    if (DS.isInstr(&I) || DS.isAnchor(&I)) {
      continue;
    }

    if (auto *Store = dyn_cast<StoreInst>(&I)) {
      addPointerUse(Store->getOperandUse(0));
      continue;
    }

    if (auto *Ret = dyn_cast<ReturnInst>(&I)) {
      if (Ret->getNumOperands() != 0) {
        addPointerUse(Ret->getOperandUse(0));
      }
      continue;
    }

    if (auto *CB = dyn_cast<CallBase>(&I)) {
      if (isKnownSafeIntrinsicUse(*CB)) {
        continue;
      }

      for (Use &Arg : CB->args()) {
        addPointerUse(Arg);
      }
    }
  }

  for (Use *U : Uses) {
    if (DS.getAsAnchor<StripAnchor>(U->get())) {
      continue;
    }

    TaggedAnchor Tagged =
        TaggedAnchor::create(DS, cast<Instruction>(U->getUser()), U->get());
    U->set(Tagged.Call);
  }
}

void PTSanLateOptimizer::materializeGlobalConstants() {
  PTU.KnownBases.clear();

  bool MaterializedGlobalConstants = false;
  for (StripAnchor StripID : DS.collectAnchors<StripAnchor>(F)) {
    if (!StripID.isPointerStrip()) {
      continue;
    }

    for (Value *Base : PTU.findBasePointers(StripID.getValue())) {
      if (auto *GV = dyn_cast<GlobalValue>(Base)) {
        MaterializedGlobalConstants |= materializeConstantUsersOfGlobal(F, GV);
      }
    }
  }

  if (MaterializedGlobalConstants) {
    PTU.KnownBases.clear();
  }
}

class PTSanPolarityAnnotator {
private:
  PointerTagSanitizer &DS;
  Function &F;
  DenseMap<Value *, Polarity> Polarities;

  void annotate(Value *V, Polarity P) {
    setPTSanPolarityMetadata(V, P);
  }

  bool mark(Value *V, Polarity P) {
    assert(V->getType()->isPointerTy());

    std::optional<Polarity> Expected =
        getPTSanPolarityMetadata(V, "ptsan.expected.");
    if (Expected && *Expected != P) {
      errs() << "PTSan polarity disagrees with min-cut expectation for " << *V
             << ": expected " << getPTSanPolarityName(*Expected) << ", got "
             << getPTSanPolarityName(P) << "\n";
      die();
    }

    auto [It, Inserted] = Polarities.insert({V, P});
    if (Inserted) {
      annotate(V, P);
      return true;
    }

    if (It->second != P) {
      errs() << "Contradictory PTSan polarity for " << *V << ": "
             << getPTSanPolarityName(It->second) << " and "
             << getPTSanPolarityName(P) << "\n";
      die();
    }

    return false;
  }

  bool markFromKnown(Value *V, Value *Source) {
    auto It = Polarities.find(Source);
    if (It == Polarities.end()) {
      return false;
    }

    return mark(V, It->second);
  }

  bool markJoin(Value *V, ArrayRef<Value *> Values) {
    optional<Polarity> Joined;
    for (Value *Input : Values) {
      auto It = Polarities.find(Input);
      if (It == Polarities.end()) {
        return false;
      }

      if (!Joined) {
        Joined = It->second;
        continue;
      }

      if (*Joined != It->second) {
        errs() << "Contradictory PTSan polarity in value join\n";
        die();
      }
    }

    return Joined && mark(V, *Joined);
  }

  void clearMetadata() {
    clearPTSanPolarityMetadata(F);
  }

public:
  PTSanPolarityAnnotator(PointerTagSanitizer &DS, Function &F) : DS(DS), F(F) {}

  void run() {
    clearMetadata();

    bool Changed = true;
    while (Changed) {
      Changed = false;

      for (Instruction &I : instructions(F)) {
        if (!I.getType()->isPointerTy()) {
          continue;
        }

        if (auto Strip = DS.getAsAnchor<StripAnchor>(&I)) {
          Changed |= mark(Strip->Call, Polarity::Stripped);
          continue;
        }

        if (auto Tag = DS.getAsAnchor<TagAnchor>(&I)) {
          Changed |= mark(Tag->getValue(), Polarity::Stripped);
          Changed |= mark(Tag->Call, Polarity::Tagged);
          continue;
        }

        if (auto Tagged = DS.getAsAnchor<TaggedAnchor>(&I)) {
          Changed |= mark(Tagged->getValue(), Polarity::Tagged);
          Changed |= mark(Tagged->Call, Polarity::Tagged);
          continue;
        }

        if (auto *PHI = dyn_cast<PHINode>(&I)) {
          SmallVector<Value *, 4> IncomingValues(PHI->incoming_values());
          Changed |= markJoin(PHI, IncomingValues);
          continue;
        }

        if (auto *Select = dyn_cast<SelectInst>(&I)) {
          SmallVector<Value *, 2> Values = {Select->getTrueValue(),
                                            Select->getFalseValue()};
          Changed |= markJoin(Select, Values);
          continue;
        }

        if (Value *Peeled = getPTSanPointerPeel(&I)) {
          Changed |= markFromKnown(&I, Peeled);
        }
      }
    }
  }
};

void PTSanLateOptimizer::run() {
  insertTaggedUseAnchors();

  if (ClNoPTSanOpts) {
    PTSanPolarityAnnotator(DS, F).run();
    return;
  }

  PTSanAnchorOptimizer Anchors(PTA);
  Anchors.hoistStripAnchors();
  Anchors.mergeStripAnchors();

  if (!ClNoStripIDHoist) {
    materializeGlobalConstants();
    Anchors.hoistStripAnchors();
    Anchors.mergeStripAnchors();

    if (ClUseLocalStripIDHoist) {
      runLocalStripIDHoisting(PTA);
    } else {
      runMinCutStripIDHoisting(PTA);
    }
  }

  Anchors.foldStripOfTagAnchors();
  Anchors.foldTagOfStripAnchors();
  Anchors.foldGetIDOfTagAnchors();
  Anchors.hoistGetIDAnchors();
  Anchors.mergeGetIDAnchors();
  Anchors.hoistStripAnchors();
  Anchors.mergeStripAnchors();
  Anchors.eraseUnusedDependencyAnchors();
  PTSanPolarityAnnotator(DS, F).run();
}

} // namespace llvm::ptsan
