//===-- PTSanLocalStripIDHoisting.cpp - Local strip-ID hoisting -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanStripIDHoisting.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/InstIterator.h"
#include <functional>

namespace llvm::ptsan {

struct StripIDHoistPlan {
  SmallPtrSet<CallInst *, 8> StripCalls;
  SmallPtrSet<CallInst *, 8> TaggedCalls;
  SmallPtrSet<Use *, 8> RetagUses;
};

struct StripIDProspect {
  Value *Base = nullptr;
  StripIDHoistPlan Plan;
  SmallVector<StripAnchor, 8> Strips;
};

class PTSanStripIDHoister {
private:
  PTSanAnalysisState &PTA;
  PointerTagSanitizer &DS;
  Function &F;
  LoopInfo &LI;
  PTSanUtils &PTU;
  DenseMap<Value *, Value *> RetagIDBaseForBase;

  int64_t getCostMultiplier(Value *V) const;
  int64_t getSavings(const StripIDProspect &Prospect) const;
  optional<StripIDHoistPlan> getHoistPlan(Value *V) const;
  bool canRetagUses(Value *V, ArrayRef<Use *> Uses) const;
  SmallSetVector<Value *, 32> collectBaseProspects();
  void applyHoist(Value *Base, const StripIDHoistPlan &Plan);

public:
  PTSanStripIDHoister(PTSanAnalysisState &PTA)
      : PTA(PTA), DS(PTA.DS), F(PTA.F), LI(PTA.LI), PTU(PTA.PTU) {}

  void run();
};

static bool isUseInFunction(Use &U, Function &F) {
  auto *I = dyn_cast<Instruction>(U.getUser());
  return I && I->getFunction() == &F;
}

bool isKnownSafeIntrinsicUse(CallBase &CB) {
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
    return true;
  default:
    return false;
  }
}

int64_t PTSanStripIDHoister::getCostMultiplier(Value *V) const {
  auto *I = dyn_cast<Instruction>(V);
  if (!I) {
    return 1;
  }

  // Model each loop level as roughly 4x more expensive. Clamp the depth so the
  // shift stays inside int64_t.
  unsigned LoopDepth = std::min<unsigned>(LI.getLoopDepth(I->getParent()), 30);
  return INT64_C(1) << (2 * LoopDepth);
}

bool PTSanStripIDHoister::canRetagUses(Value *V, ArrayRef<Use *> Uses) const {
  Instruction *IP = PTU.getFirstIPAfter(V).getIP();

  for (Use *U : Uses) {
    auto *UserI = dyn_cast<Instruction>(U->getUser());
    if (UserI && UserI != IP && !PTA.DT.dominates(IP, *U)) {
      return false;
    }
  }

  return true;
}

int64_t PTSanStripIDHoister::getSavings(const StripIDProspect &Prospect) const {
  int64_t TaggedCost = 0;
  for (CallInst *Call : Prospect.Plan.StripCalls) {
    TaggedCost += getCostMultiplier(Call);
  }

  // If Base is already a tag anchor result, the inserted base strip will fold.
  int64_t StrippedCost = DS.getAsAnchor<TagAnchor>(Prospect.Base)
                             ? 0
                             : getCostMultiplier(Prospect.Base);

  for (CallInst *Call : Prospect.Plan.TaggedCalls) {
    TaggedAnchor Tagged = *DS.getAsAnchor<TaggedAnchor>(Call);
    bool RetagWillFold =
        Tagged.getValue() == Prospect.Base &&
        RetagIDBaseForBase.lookup(Prospect.Base) == Prospect.Base;
    if (!RetagWillFold && !willFoldGetIDOfTagAnchor(DS, Tagged)) {
      StrippedCost += 2 * getCostMultiplier(Call);
    }
  }

  SmallPtrSet<Value *, 8> RetaggedValues;
  for (Use *U : Prospect.Plan.RetagUses) {
    Value *V = U->get();
    if (RetaggedValues.insert(V).second && !DS.getAsAnchor<TagAnchor>(V)) {
      StrippedCost += 2 * getCostMultiplier(V);
    }
  }

  return TaggedCost - StrippedCost;
}

optional<StripIDHoistPlan>
PTSanStripIDHoister::getHoistPlan(Value *Base) const {
  SmallPtrSet<Value *, 8> Visiting;
  Instruction *BaseIP = PTU.getFirstIPAfter(Base).getIP();

  std::function<optional<StripIDHoistPlan>(Value *)> rec;
  rec = [&](Value *V) -> optional<StripIDHoistPlan> {
    if (!V->getType()->isPointerTy()) {
      return nullopt;
    }

    if (!Visiting.insert(V).second) {
      return nullopt;
    }

    // Remove V when we return.
    auto VisitingGuard = make_scope_exit([&] { Visiting.erase(V); });

    StripIDHoistPlan Plan;

    if (auto Strip = DS.getAsAnchor<StripAnchor>(V)) {
      Plan.StripCalls.insert(Strip->Call);
      return Plan;
    }

    if (auto Tagged = DS.getAsAnchor<TaggedAnchor>(V)) {
      // This tagged leaf will be rewritten to use tag(V, id). Make sure the
      // hoisted base strip is available at the leaf before accepting the plan.
      if (BaseIP != Tagged->Call &&
          !PTA.DT.dominates(BaseIP, Tagged->Call->getOperandUse(0))) {
        return nullopt;
      }

      Plan.TaggedCalls.insert(Tagged->Call);
      return Plan;
    }

    // A tag anchor can be the initial base; descendant tag anchors are already
    // tagged boundaries and stop the walk.
    if (DS.getAsAnchor<TagAnchor>(V) && V != Base) {
      return Plan;
    }

    // Gather users that remain inside the stripped closure.
    SmallVector<Value *, 8> UsersToVisit;
    for (Use &U : V->uses()) {
      if (!isUseInFunction(U, F)) {
        continue;
      }

      User *User = U.getUser();
      if (DS.getAsAnchor<TagAnchor>(User) ||
          DS.getAsAnchor<SourceAnchor>(User) ||
          DS.getAsAnchor<GetIDAnchor>(User)) {
        continue;
      }

      if (auto *CB = dyn_cast<CallBase>(User)) {
        if (isKnownSafeIntrinsicUse(*CB)) {
          continue;
        }
      }

      if ((isa<PHINode>(User) || isa<SelectInst>(User)) &&
          !is_contained(PTU.findBasePointers(User), Base)) {
        if (!canRetagUses(V, {&U})) {
          return nullopt;
        }

        Plan.RetagUses.insert(&U);
        continue;
      }

      UsersToVisit.push_back(User);
    }

    // Recurse through the stripped closure.
    for (Value *User : UsersToVisit) {
      optional<StripIDHoistPlan> UserPlan = rec(User);
      if (!UserPlan) {
        return nullopt;
      }
      set_union(Plan.StripCalls, UserPlan->StripCalls);
      set_union(Plan.TaggedCalls, UserPlan->TaggedCalls);
      for (Use *U : UserPlan->RetagUses) {
        Plan.RetagUses.insert(U);
      }
    }

    return Plan;
  };

  return rec(Base);
}

static SmallVector<StripAnchor, 8>
getStripIDAnchors(PointerTagSanitizer &DS, const StripIDHoistPlan &Plan) {
  SmallVector<StripAnchor, 8> Strips;
  for (CallInst *Call : Plan.StripCalls) {
    Strips.push_back(*DS.getAsAnchor<StripAnchor>(Call));
  }
  return Strips;
}

static bool hasStripIDOverlap(ArrayRef<StripAnchor> Strips,
                              SmallPtrSetImpl<CallInst *> &SelectedStripCalls) {
  return any_of(Strips, [&](StripAnchor Strip) {
    return SelectedStripCalls.contains(Strip.Call);
  });
}

SmallSetVector<Value *, 32> PTSanStripIDHoister::collectBaseProspects() {
  SmallSetVector<Value *, 32> PendingBases;
  RetagIDBaseForBase.clear();

  for (StripAnchor StripID : DS.collectAnchors<StripAnchor>(F)) {
    if (!StripID.isPointerStrip()) {
      continue;
    }

    SmallVector<Value *> Bases = PTU.findBasePointers(StripID.getValue());
    Value *RetagIDBase = Bases.front();
    for (Value *Base : Bases) {
      auto [It, Inserted] = RetagIDBaseForBase.insert({Base, RetagIDBase});
      assert((Inserted || It->second == RetagIDBase) &&
             "base belongs to multiple top-base kingdoms");
      PendingBases.insert(Base);
    }
  }

  return PendingBases;
}

void PTSanStripIDHoister::applyHoist(Value *Base,
                                     const StripIDHoistPlan &Plan) {
  Instruction *BaseIP = PTU.getFirstIPAfter(Base).getIP();
  StripAnchor BaseStrip = StripAnchor::create(DS, BaseIP, Base);

  Value *RetagIDBase = RetagIDBaseForBase.at(Base);
  TaggedAnchor TaggedRetagIDBase =
      TaggedAnchor::create(DS, BaseIP, RetagIDBase);
  GetIDAnchor RetagID = GetIDAnchor::create(DS, BaseIP, TaggedRetagIDBase.Call);

  SmallMapVector<Value *, SmallVector<Use *, 4>, 8> RetagUsesByValue;
  for (Use *U : Plan.RetagUses) {
    RetagUsesByValue[U->get()].push_back(U);
  }

  for (auto &[Node, Uses] : RetagUsesByValue) {
    Instruction *IP = PTU.getFirstIPAfter(Node).getIP();
    Value *TaggedValue = nullptr;

    if (DS.getAsAnchor<TagAnchor>(Node)) {
      TaggedAnchor Tagged = TaggedAnchor::create(DS, IP, Node);
      TaggedValue = Tagged.Call;
    } else {
      Value *TagInput = Node == Base ? BaseStrip.Call : Node;
      TagAnchor Tag = TagAnchor::create(DS, IP, TagInput, RetagID.Call);
      TaggedAnchor Tagged = TaggedAnchor::create(DS, IP, Tag.Call);
      TaggedValue = Tagged.Call;
    }

    for (Use *U : Uses) {
      U->set(TaggedValue);
    }
  }

  // The hoisted strip replaces uses in the stripped closure. Leave alone uses
  // outside this function, source bookkeeping, the new strip operand, and
  // explicit tagged/get_id consumers of the original tagged value.
  Base->replaceUsesWithIf(BaseStrip.Call, [&](Use &U) {
    if (!isUseInFunction(U, F) || DS.getAsAnchor<SourceAnchor>(U.getUser()) ||
        U.getUser() == BaseStrip.Call) {
      return false;
    }

    if (DS.getAsAnchor<TaggedAnchor>(U.getUser()) ||
        DS.getAsAnchor<GetIDAnchor>(U.getUser())) {
      return false;
    }

    return true;
  });

  for (CallInst *Call : Plan.TaggedCalls) {
    TaggedAnchor Tagged = *DS.getAsAnchor<TaggedAnchor>(Call);
    // Tag anchors require stripped inputs. For direct tagged uses of the hoist
    // base, use the new base strip so tag(strip(X), get_id(X)) can fold back to
    // X without ever creating tag(X, get_id(X)) on an already-tagged pointer.
    Value *TagInput =
        Tagged.getValue() == Base ? BaseStrip.Call : Tagged.getValue();
    TagAnchor Tag = TagAnchor::create(DS, Tagged.Call, TagInput, RetagID.Call);
    Tagged.Call->setArgOperand(0, Tag.Call);
  }

  if (RetagID.Call->use_empty()) {
    RetagID.erase();
  }
  if (TaggedRetagIDBase.Call->use_empty()) {
    TaggedRetagIDBase.erase();
  }
}

void PTSanStripIDHoister::run() {
  PTSanAnchorOptimizer Anchors(PTA);
  SmallSetVector<Value *, 32> PendingBases = collectBaseProspects();

  while (!PendingBases.empty()) {
    SmallVector<StripIDProspect, 32> Prospects;

    // Iterate each base and see if it would be profitable to hoist.
    for (Value *Base : PendingBases) {
      optional<StripIDHoistPlan> Plan = getHoistPlan(Base);
      if (!Plan || Plan->StripCalls.empty()) {
        continue;
      }

      StripIDProspect Prospect{Base, *Plan, getStripIDAnchors(DS, *Plan)};
      if (getSavings(Prospect) <= 0) {
        continue;
      }
      Prospects.push_back(Prospect);
    }

    if (Prospects.empty()) {
      break;
    }

    // Sort prospects by savings.
    sort(Prospects,
         [this](const StripIDProspect &LHS, const StripIDProspect &RHS) {
           return getSavings(LHS) > getSavings(RHS);
         });

    // Select non-overlapping prospects; overlapping bases are reanalyzed after
    // the selected strip IDs are gone.
    SmallVector<StripIDProspect, 32> SelectedProspects;
    SmallPtrSet<CallInst *, 32> SelectedStripCalls;
    PendingBases.clear();
    for (StripIDProspect &Prospect : Prospects) {
      // Overlaps with an already-selected prospect; reanalyze it next round.
      if (hasStripIDOverlap(Prospect.Strips, SelectedStripCalls)) {
        PendingBases.insert(Prospect.Base);
        continue;
      }

      for (StripAnchor Strip : Prospect.Strips) {
        SelectedStripCalls.insert(Strip.Call);
      }
      SelectedProspects.push_back(Prospect);
    }

    for (StripIDProspect &Prospect : SelectedProspects) {
      applyHoist(Prospect.Base, Prospect.Plan);
    }

    for (StripIDProspect &Prospect : SelectedProspects) {
      for (StripAnchor Strip : Prospect.Strips) {
        assert(Strip.isPointerStrip());
        Strip.Call->replaceAllUsesWith(Strip.getValue());
        Strip.erase();
      }
    }

    Anchors.foldStripOfTagAnchors();
    Anchors.foldTagOfStripAnchors();
  }
}

void runLocalStripIDHoisting(PTSanAnalysisState &PTA) {
  PTSanStripIDHoister(PTA).run();
}

} // namespace llvm::ptsan
