//===-- PTSanAnchorOptimization.cpp - PTSan anchor cleanup -----*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"

namespace llvm::ptsan {

void PTSanAnchorOptimizer::hoistStripAnchors() {
  for (StripAnchor Strip : DS.collectAnchors<StripAnchor>(PTA.F)) {
    Instruction *IP = PTU.getFirstIPAfter(Strip.getValue()).getIP();
    Strip.Call->moveBefore(IP);
  }
}

void PTSanAnchorOptimizer::mergeStripAnchors() {
  DenseMap<Value *, CallInst *> Survivors;

  for (StripAnchor Strip : DS.collectAnchors<StripAnchor>(PTA.F)) {
    auto [It, Inserted] = Survivors.insert({Strip.getValue(), Strip.Call});
    if (Inserted) {
      continue;
    }

    CallInst *Survivor = It->second;
    assert(DT.dominates(Survivor, Strip.Call) &&
           "merged strip anchor must dominate duplicate uses");
    Strip.Call->replaceAllUsesWith(Survivor);
    Strip.erase();
  }
}

void PTSanAnchorOptimizer::hoistGetIDAnchors() {
  for (GetIDAnchor ID : DS.collectAnchors<GetIDAnchor>(PTA.F)) {
    Instruction *IP = PTU.getFirstIPAfter(ID.getBase()).getIP();
    ID.Call->moveBefore(IP);
  }
}

void PTSanAnchorOptimizer::mergeGetIDAnchors() {
  DenseMap<Value *, CallInst *> Survivors;

  for (GetIDAnchor ID : DS.collectAnchors<GetIDAnchor>(PTA.F)) {
    auto [It, Inserted] = Survivors.insert({ID.getBase(), ID.Call});
    if (Inserted) {
      continue;
    }

    CallInst *Survivor = It->second;
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

    ID.Call->replaceAllUsesWith(Tag->getID());
    ID.erase();

    if (Tag->Call->use_empty()) {
      Tag->erase();
    }
  }
}

void PTSanAnchorOptimizer::eraseUnusedDependencyAnchors() {
  for (BoundAnchor Bounds : DS.collectAnchors<BoundAnchor>(PTA.F)) {
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
