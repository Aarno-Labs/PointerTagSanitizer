//===-- PTSanLateOptimization.cpp - Late PTSan optimization ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"
#include "PTSanUtils.h"
#include "Optimizations/PTSanStripIDHoisting.h"
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

static bool isKnownSafeIntrinsicUse(CallBase &CB) {
  auto *II = dyn_cast<IntrinsicInst>(&CB);
  if (!II) {
    return false;
  }

  switch (II->getIntrinsicID()) {
  case Intrinsic::assume:
  case Intrinsic::sideeffect:
  case Intrinsic::donothing:
  case Intrinsic::prefetch:
  case Intrinsic::is_constant:
  case Intrinsic::objectsize:
  case Intrinsic::expect:
  case Intrinsic::expect_with_probability:
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_value:
  case Intrinsic::dbg_label:
  case Intrinsic::dbg_assign:
  case Intrinsic::pseudoprobe:
  case Intrinsic::experimental_noalias_scope_decl:
  case Intrinsic::var_annotation:
  case Intrinsic::annotation:
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
  case Intrinsic::invariant_start:
  case Intrinsic::invariant_end:
  case Intrinsic::threadlocal_address:
    return true;
  default:
    return false;
  }
}

void PTSanLateOptimizer::insertTaggedUseAnchors() {
  SmallVector<Use *, 32> Uses;
  auto addPointerUse = [&](Use &U) {
    if (U->getType()->isPtrOrPtrVectorTy()) {
      Uses.push_back(&U);
    }
  };

  for (Instruction &I : instructions(F)) {
    if (auto ID = DS.getAsAnchor<GetIDAnchor>(&I)) {
      // A source ID is assigned by lowering, not extracted from pointer bits.
      if (!DS.getAsAnchor<SourceAnchor>(ID->getBase())) {
        Uses.push_back(&ID->Call->getOperandUse(0));
      }
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

    if (isa<ExtractElementInst, InsertElementInst, InsertValueInst>(&I)) {
      for (Use &U : I.operands()) {
        addPointerUse(U);
      }
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
  for (auto &GI : DS.GlobalVariableProxies) {
    MaterializedGlobalConstants |=
        materializeConstantUsersOfGlobal(F, GI.first);
  }
  for (auto &GA : DS.GlobalAliasProxies) {
    MaterializedGlobalConstants |=
        materializeConstantUsersOfGlobal(F, GA.first);
  }

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

static bool validatePolarities(PointerTagSanitizer &DS, Function &F) {
  PTSanPolarityReader Polarities(DS, F);

  auto haveSamePolarity = [&](Value *A, Value *B) {
    return Polarities.getPolarity(A) == Polarities.getPolarity(B);
  };

  auto validateJoin = [&](Value *V, ArrayRef<Value *> Inputs) {
    Polarity P = Polarities.getPolarity(V);

    for (Value *Input : Inputs) {
      if (Polarities.getPolarity(Input) != P) {
        return false;
      }
    }

    return true;
  };

  bool Valid = true;
  for (Instruction &I : instructions(F)) {
    if (I.getType()->isPtrOrPtrVectorTy()) {
      if (auto Required = getRequiredPolarity(DS, &I)) {
        Valid &= Polarities.getPolarity(&I) == *Required;
      }
    }

    if (auto Source = DS.getAsAnchor<SourceAnchor>(&I)) {
      Valid &= Polarities.getPolarity(Source->getSource()) == Polarity::Stripped;
      continue;
    }

    if (!ClClearIdsOnArith) {
      if (auto *Sub = dyn_cast<BinaryOperator>(&I)) {
        if (Sub->getOpcode() == Instruction::Sub) {
          auto *Left = dyn_cast<PtrToIntInst>(Sub->getOperand(0));
          auto *Right = dyn_cast<PtrToIntInst>(Sub->getOperand(1));
          if (Left && Right) {
            Valid &= haveSamePolarity(Left->getPointerOperand(),
                                      Right->getPointerOperand());
          }
        }
      }
    }

    if (auto *Cmp = dyn_cast<ICmpInst>(&I)) {
      if (Cmp->getOperand(0)->getType()->isPtrOrPtrVectorTy()) {
        Valid &= haveSamePolarity(Cmp->getOperand(0), Cmp->getOperand(1));
      }
      continue;
    }

    if (!I.getType()->isPtrOrPtrVectorTy()) {
      continue;
    }

    if (auto Strip = DS.getAsAnchor<StripAnchor>(&I)) {
      Valid &= Polarities.getPolarity(Strip->getValue()) == Polarity::Tagged;
      continue;
    }

    if (auto Tag = DS.getAsAnchor<TagAnchor>(&I)) {
      Valid &= Polarities.getPolarity(Tag->getValue()) == Polarity::Stripped;
      continue;
    }

    if (auto Tagged = DS.getAsAnchor<TaggedAnchor>(&I)) {
      Valid &= Polarities.getPolarity(Tagged->getValue()) == Polarity::Tagged;
      continue;
    }

    if (auto *PHI = dyn_cast<PHINode>(&I)) {
      SmallVector<Value *, 4> IncomingValues(PHI->incoming_values());
      Valid &= validateJoin(PHI, IncomingValues);
      continue;
    }

    if (auto *Select = dyn_cast<SelectInst>(&I)) {
      SmallVector<Value *, 2> Values = {Select->getTrueValue(),
                                        Select->getFalseValue()};
      Valid &= validateJoin(Select, Values);
      continue;
    }

    if (auto *Shuffle = dyn_cast<ShuffleVectorInst>(&I)) {
      auto [UsesLHS, UsesRHS] = getPTSanUsedShuffleOperands(*Shuffle);
      if (UsesLHS) {
        Valid &= haveSamePolarity(Shuffle, Shuffle->getOperand(0));
      }
      if (UsesRHS) {
        Valid &= haveSamePolarity(Shuffle, Shuffle->getOperand(1));
      }
      continue;
    }

    if (Use *U = getPTSanRewriteablePointerUse(I)) {
      Valid &= haveSamePolarity(&I, U->get());
      continue;
    }
  }

  return Valid;
}

void PTSanLateOptimizer::run() {
  // Polarity solving and conversion insertion are required for correctness,
  // including under ptsan-no-opts. Sources are raw until explicitly tagged.
  materializeGlobalConstants();
  insertTaggedUseAnchors();

  PTSanAnchorOptimizer Anchors(PTA);
  Anchors.hoistStripAnchors();
  Anchors.mergeStripAnchors();

  runMinCutStripIDHoisting(PTA);
  Anchors.foldRedundantStripAnchors();

  Anchors.foldStripOfTagAnchors();
  Anchors.foldTagOfStripAnchors();
  Anchors.foldGetIDOfTagAnchors();
  Anchors.hoistGetIDAnchors();
  Anchors.mergeGetIDAnchors();
  Anchors.hoistStripAnchors();
  Anchors.mergeStripAnchors();
  Anchors.eraseUnusedDependencyAnchors();
  assert(validatePolarities(DS, F) &&
         "min-cut polarity metadata must match rewritten IR");
}

} // namespace llvm::ptsan
