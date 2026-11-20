//===-- PTSanAnchorOptimization.cpp - PTSan anchor cleanup -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"

namespace llvm::ptsan {

void PTSanAnchorOptimizer::hoistStripAnchors() {
  for (StripAnchor Strip : DS.collectAnchors<StripAnchor>(PTA.F)) {
    HoistTarget Target = PTU.getInsertionPointForUse(Strip.getValue(), Strip.Call);
    for (Value *Operand : drop_begin(Strip.Call->args())) {
      Target = PTU.getFirstCommonInsertionPoint(
          {Target, PTU.getInsertionPointForUse(Operand, Strip.Call)});
    }
    Strip.Call->moveBefore(Target.getIP());
  }
}

void PTSanAnchorOptimizer::sinkAnchor(CallInst *Call) {
  assert(!Call->use_empty() && "unused anchors must be removed before sinking");

  Instruction *IP = nullptr;
  for (Use &U : Call->uses()) {
    auto *UseIP = cast<Instruction>(U.getUser());
    if (auto *PHI = dyn_cast<PHINode>(UseIP)) {
      UseIP = PHI->getIncomingBlock(U)->getTerminator();
    }
    IP = IP ? DT.findNearestCommonDominator(IP, UseIP) : UseIP;
  }

  BasicBlock *SourceBB = Call->getParent();
  while (PTA.LI.getLoopDepth(IP->getParent()) >
         PTA.LI.getLoopDepth(SourceBB)) {
    IP = DT.getNode(IP->getParent())->getIDom()->getBlock()->getTerminator();
  }
  Call->moveBefore(IP);
}

void PTSanAnchorOptimizer::sinkStripAnchors() {
  for (StripAnchor Strip : DS.collectAnchors<StripAnchor>(PTA.F)) {
    sinkAnchor(Strip.Call);
  }
}

void PTSanAnchorOptimizer::sinkLoadBoundAnchors() {
  for (LoadBoundAnchor Bounds :
       DS.collectAnchors<LoadBoundAnchor>(PTA.F)) {
    sinkAnchor(Bounds.Call);
  }
}

void PTSanAnchorOptimizer::mergeStripAnchors() {
  DenseMap<Value *, SmallVector<CallInst *, 1>> Survivors;

  for (StripAnchor Strip : DS.collectAnchors<StripAnchor>(PTA.F)) {
    auto &Candidates = Survivors[Strip.getValue()];
    auto It = llvm::find_if(Candidates, [&](CallInst *Candidate) {
      return Candidate->isIdenticalTo(Strip.Call) &&
             DT.dominates(Candidate, Strip.Call);
    });
    if (It == Candidates.end()) {
      Candidates.push_back(Strip.Call);
      continue;
    }

    CallInst *Survivor = *It;
    assert(DT.dominates(Survivor, Strip.Call) &&
           "merged strip anchor must dominate duplicate uses");
    Strip.Call->replaceAllUsesWith(Survivor);
    Strip.erase();
  }

  // Cached base lists can contain strip anchors erased by merging.
  PTU.KnownBases.clear();
}

void PTSanAnchorOptimizer::foldRedundantStripAnchors() {
  PTSanPolarityReader Polarities(DS, PTA.F);
  for (StripAnchor Strip : DS.collectAnchors<StripAnchor>(PTA.F)) {
    if (!Strip.isPointerStrip()) {
      continue;
    }

    Value *ToStrip = Strip.getValue();
    // Stripping an already-raw pointer leaves it unchanged.
    if (Polarities.getPolarity(ToStrip) == Polarity::Stripped) {
      Strip.Call->replaceAllUsesWith(ToStrip);
      Strip.erase();
    }
  }
}

void PTSanAnchorOptimizer::hoistGetIDAnchors() {
  for (GetIDAnchor ID : DS.collectAnchors<GetIDAnchor>(PTA.F)) {
    Instruction *IP = PTU.getInsertionPointForUse(ID.getBase(), ID.Call).getIP();
    ID.Call->moveBefore(IP);
  }
}

void PTSanAnchorOptimizer::mergeGetIDAnchors() {
  DenseMap<Value *, SmallVector<CallInst *, 1>> Survivors;

  for (GetIDAnchor ID : DS.collectAnchors<GetIDAnchor>(PTA.F)) {
    auto &Candidates = Survivors[ID.getBase()];
    auto It = llvm::find_if(Candidates, [&](CallInst *Candidate) {
      return DT.dominates(Candidate, ID.Call);
    });
    if (It == Candidates.end()) {
      Candidates.push_back(ID.Call);
      continue;
    }

    CallInst *Survivor = *It;
    assert(DT.dominates(Survivor, ID.Call) &&
           "merged get-id anchor must dominate duplicate uses");
    ID.Call->replaceAllUsesWith(Survivor);
    ID.erase();
  }
}

bool willFoldStripOfTagAnchor(PointerTagSanitizer &DS, StripAnchor Strip) {
  return Strip.isPointerStrip() &&
         DS.getAsAnchor<TagAnchor>(Strip.getValue()).has_value();
}

void PTSanAnchorOptimizer::foldStripOfTagAnchors() {
  for (StripAnchor Strip : DS.collectAnchors<StripAnchor>(PTA.F)) {
    if (!willFoldStripOfTagAnchor(DS, Strip)) {
      continue;
    }

    TagAnchor Tag = *DS.getAsAnchor<TagAnchor>(Strip.getValue());
    Strip.Call->replaceAllUsesWith(Tag.getValue());
    Strip.erase();

    if (Tag.Call->use_empty()) {
      Tag.erase();
    }
  }
}

bool willFoldTagOfStripAnchor(PointerTagSanitizer &DS, TagAnchor Tag) {
  optional<StripAnchor> Strip = DS.getAsAnchor<StripAnchor>(Tag.getValue());
  if (!Strip) {
    return false;
  }

  optional<GetIDAnchor> ID = DS.getAsAnchor<GetIDAnchor>(Tag.getID());
  if (!ID) {
    return false;
  }

  Value *IDBase = ID->getBase();
  if (optional<TaggedAnchor> Tagged = DS.getAsAnchor<TaggedAnchor>(IDBase)) {
    IDBase = Tagged->getValue();
  }

  return IDBase == Strip->getValue();
}

void PTSanAnchorOptimizer::foldTagOfStripAnchors() {
  for (TagAnchor Tag : DS.collectAnchors<TagAnchor>(PTA.F)) {
    if (!willFoldTagOfStripAnchor(DS, Tag)) {
      continue;
    }

    StripAnchor Strip = *DS.getAsAnchor<StripAnchor>(Tag.getValue());
    Tag.Call->replaceAllUsesWith(Strip.getValue());
    Tag.erase();

    if (Strip.Call->use_empty()) {
      Strip.erase();
    }
  }
}

bool willFoldGetIDOfTagAnchor(PointerTagSanitizer &DS, TaggedAnchor Tagged) {
  for (Use &U : Tagged.Call->uses()) {
    if (!DS.getAsAnchor<GetIDAnchor>(U.getUser())) {
      return false;
    }
  }

  return true;
}

void PTSanAnchorOptimizer::foldGetIDOfTagAnchors() {
  for (GetIDAnchor ID : DS.collectAnchors<GetIDAnchor>(PTA.F)) {
    optional<TaggedAnchor> Tagged = DS.getAsAnchor<TaggedAnchor>(ID.getBase());
    Value *Base = Tagged ? Tagged->getValue() : ID.getBase();
    if (Tagged) {
      ID.Call->setArgOperand(0, Base);
      if (Tagged->Call->use_empty()) {
        Tagged->erase();
      }
    }

    optional<TagAnchor> Tag = DS.getAsAnchor<TagAnchor>(Base);
    if (!Tag) {
      continue;
    }

    Value *Replacement =
        PTU.splatToMatchType(Tag->getID(), ID.Call->getType(), ID.Call);

    ID.Call->replaceAllUsesWith(Replacement);
    ID.erase();

    if (Tag->Call->use_empty()) {
      Tag->erase();
    }
  }
}

void PTSanAnchorOptimizer::eraseUnusedDependencyAnchors() {
  for (StripAnchor Strip : DS.collectAnchors<StripAnchor>(PTA.F)) {
    if (Strip.Call->use_empty()) {
      Strip.erase();
    }
  }

  // An ID32 static-base anchor computes raw base - origin and uses a separate
  // LoadBoundAnchor for that origin. Reverse order erases the static base first,
  // so its now-unused origin can be erased in the same pass.
  auto BoundsAnchors = DS.collectAnchors<BoundAnchor>(PTA.F);
  for (BoundAnchor Bounds : reverse(BoundsAnchors)) {
    if (Bounds.Call->use_empty()) {
      Bounds.erase();
    }
  }

  for (GetIDAnchor ID : DS.collectAnchors<GetIDAnchor>(PTA.F)) {
    if (ID.Call->use_empty()) {
      ID.erase();
    }
  }

  for (TaggedAnchor Tagged : DS.collectAnchors<TaggedAnchor>(PTA.F)) {
    if (Tagged.Call->use_empty()) {
      Tagged.erase();
    }
  }
}

} // namespace llvm::ptsan
