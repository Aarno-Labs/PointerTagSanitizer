//===-- PTSanUtils.cpp - Shared PTSan utilities ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/Loads.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Instrumentation/PointerTagSanitizer.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "PTSanUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"

#include <unordered_map>
#include <stdlib.h>
#include <variant>

#define DEBUG_TYPE "ptsan-dbg"

#define DBG(X) DEBUG_WITH_TYPE(DEBUG_TYPE, X)

namespace llvm::ptsan {

using std::optional;
using std::nullopt;
using NW = llvm::SCEV::NoWrapFlags;

cl::opt<bool> ClPTSanLooseLoopOptimizations(
  "ptsan-loose-loop-optimizations",
  cl::desc("Use postdominance-only loop optimization legality checks. This is faster but may be unsound"),
  cl::init(false)
);

bool materializeConstantUsersOfGlobal(Function &F, GlobalValue *G) {
  SmallVector<Constant *, 16> GlobalUsingConstants;

  for (auto UI = G->use_begin(), UE = G->use_end(); UI != UE;) {
    Use &U = *UI++;
    Constant *C = dyn_cast<Constant>(U.getUser());
    if (!C) {
      continue;
    }

    if (!(isa<ConstantExpr>(C) || isa<ConstantAggregate>(C))) {
      errs() << "WARNING: found non-expr, non-aggregate global-using constant: "
             << *C << "\n";
      continue;
    }

    GlobalUsingConstants.push_back(C);
  }

  return convertUsersOfConstantsToInstructions(GlobalUsingConstants, &F, false,
                                               true);
}

StringRef getPTSanPolarityName(Polarity P) {
  switch (P) {
  case Polarity::Tagged:
    return "tagged";
  case Polarity::Stripped:
    return "stripped";
  }
  llvm_unreachable("unknown PTSan polarity");
}

static std::string getPTSanPolarityMDName(Polarity P, StringRef Prefix) {
  std::string MDName = Prefix.str();
  MDName += getPTSanPolarityName(P).str();
  return MDName;
}

void setPTSanPolarityMetadata(Value *V, Polarity P, StringRef Prefix) {
  auto *I = dyn_cast<Instruction>(V);
  if (!I) {
    return;
  }

  Polarity OtherPolarity =
      P == Polarity::Tagged ? Polarity::Stripped : Polarity::Tagged;
  std::string OtherMDName = getPTSanPolarityMDName(OtherPolarity, Prefix);
  assert(!I->getMetadata(OtherMDName) &&
         "contradictory PTSan polarity metadata");

  I->setMetadata(getPTSanPolarityMDName(P, Prefix),
                 MDNode::get(I->getContext(), {}));
}

std::optional<Polarity> getPTSanPolarityMetadata(Value *V, StringRef Prefix) {
  auto *I = dyn_cast<Instruction>(V);
  if (!I) {
    return std::nullopt;
  }

  bool Tagged =
      I->getMetadata(getPTSanPolarityMDName(Polarity::Tagged, Prefix));
  bool Stripped =
      I->getMetadata(getPTSanPolarityMDName(Polarity::Stripped, Prefix));
  assert(!(Tagged && Stripped) && "contradictory PTSan polarity metadata");
  if (Tagged) {
    return Polarity::Tagged;
  }
  if (Stripped) {
    return Polarity::Stripped;
  }
  return std::nullopt;
}

void clearPTSanPolarityMetadata(Function &F, StringRef Prefix) {
  for (Instruction &I : instructions(F)) {
    I.setMetadata(getPTSanPolarityMDName(Polarity::Tagged, Prefix), nullptr);
    I.setMetadata(getPTSanPolarityMDName(Polarity::Stripped, Prefix), nullptr);
  }
}

Use *getPTSanRewriteablePointerUse(Instruction &I) {
  if (!I.getType()->isPointerTy()) {
    return nullptr;
  }

  if (auto *GEP = dyn_cast<GetElementPtrInst>(&I)) {
    return &GEP->getOperandUse(0);
  }

  if (auto *Cast = dyn_cast<CastInst>(&I)) {
    if (Cast->getOperand(0)->getType()->isPointerTy()) {
      return &Cast->getOperandUse(0);
    }
    return nullptr;
  }

  if (auto *Freeze = dyn_cast<FreezeInst>(&I)) {
    return &Freeze->getOperandUse(0);
  }

  return nullptr;
}

Value *getPTSanPointerPeel(Value *V) {
  if (!V->getType()->isPointerTy()) {
    return nullptr;
  }

  if (auto *I = dyn_cast<Instruction>(V)) {
    if (Use *U = getPTSanRewriteablePointerUse(*I)) {
      return U->get();
    }
  }

  if (auto *GEP = dyn_cast<GEPOperator>(V)) {
    return GEP->getPointerOperand();
  }

  if (Operator::getOpcode(V) == Instruction::BitCast ||
      Operator::getOpcode(V) == Instruction::AddrSpaceCast) {
    Value *Operand = cast<Operator>(V)->getOperand(0);
    if (Operand->getType()->isPointerTy()) {
      return Operand;
    }
    return nullptr;
  }

  if (auto *GA = dyn_cast<GlobalAlias>(V)) {
    if (!GA->isInterposable()) {
      return GA->getAliasee();
    }
    return nullptr;
  }

  if (auto *Call = dyn_cast<CallBase>(V)) {
    return getArgumentAliasingToReturnedPointer(Call, false);
  }

  return nullptr;
}


/////////////////////////////////
// PTSanUtils
/////////////////////////////////
// Peel through PTSan's pointer provenance rules while recording each value as a
// base candidate.
static Value *getUnderlyingObjects(Value *V,
                                   SmallPtrSetImpl<Value *> &BasePointers,
                                   unsigned MaxLookup) {
  for (unsigned Count = 0; Count < MaxLookup; ++Count) {
    BasePointers.insert(V);
    Value *Peeled = getPTSanPointerPeel(V);
    if (!Peeled) {
      return V;
    }
    V = Peeled;
  }

  BasePointers.insert(V);
  return V;
}

using BaseSet = SmallPtrSet<Value *, 8>;

SmallVector<Value *> PTSanUtils::findBasePointers(Value *Start) {
  DBG(errs() << "GetBase on " << *Start << "\n");

  SmallPtrSet<const Value *, 32> Visiting;

  // These are *not* the same as knownBases, because these
  // are relative to the current start node -- things here
  // may be null or truncated where they shouldn't be in the 'real'
  // version
  ValueMap<Value *, BaseSet> VisitedObjects;

  // Visiting is the active recursion stack. VisitedObjects contains fully
  // computed candidate base sets for peeled objects, without path-specific
  // peeled values.

  std::function<optional<BaseSet>(Value *)> handle_special;
  std::function<optional<BaseSet>(Value *)> visit;

  visit = [&](Value *V) -> optional<BaseSet> {
    DBG(errs() << "\tGet underlying object for " << *V << ":\n");
    BaseSet UnderlyingBases;
    Value *Obj = getUnderlyingObjects(V, UnderlyingBases, 100);
    DBG(errs() << "\t\t" << *Obj << ":\n");

    if (VisitedObjects.count(Obj)) {
      DBG(errs() << "\tAlready visited\n");
      BaseSet Out = VisitedObjects[Obj];
      set_union(Out, UnderlyingBases);
      return Out;
    }

    // Already visiting -- ignore
    if (!Visiting.insert(Obj).second) {
      DBG(errs() << "\t\tAlready visiting. Returning nullptr\n");
      return nullopt;
    }

    optional<BaseSet> Out = handle_special(Obj);
    Visiting.erase(Obj);

    DBG(errs() << "\tFor " << *Obj << "\n");
    if (Out) {
      VisitedObjects[Obj] = *Out;
      set_union(*Out, UnderlyingBases);
      DBG(errs() << "\t\tGot " << Out->size() << " base candidates\n");
    } else {
      DBG(errs() << "\t\tGot nullptr base\n");
    }

    return Out;
  };

  handle_special = [&] (Value *Obj) -> optional<BaseSet> {
    if (auto *SI = dyn_cast<SelectInst>(Obj)) {
      optional<BaseSet> T = visit(SI->getTrueValue());
      optional<BaseSet> F = visit(SI->getFalseValue());
      if (!T && !F) { return nullopt; }
      if (!T) { return F; }
      if (!F) { return T; }

      T->insert(SI);
      F->insert(SI);
      return set_intersection(*T, *F);
    }

    if (auto *PN = dyn_cast<PHINode>(Obj)) {
      optional<BaseSet> POut = nullopt;

      for (size_t i = 0; i < PN->getNumIncomingValues(); i++) {
        optional<BaseSet> NewObj = visit(PN->getIncomingValue(i));
        // If NewObj is null, no new info, continue.
        if (!NewObj) {
          continue;
        }
        NewObj->insert(PN);
        if (!POut) {
          POut = *NewObj;
        } else {
          POut = set_intersection(*POut, *NewObj);
        }
      }

      return POut;
    }

    BaseSet Out;
    Out.insert(Obj);
    return Out;
  };

  // Special case ConstantPointerNull, since it's
  // normally nullptr in the visitor.
  if (isa<ConstantPointerNull>(Start)) {
    return {Start};
  }

  if (KnownBases.count(Start)) {
    DBG(errs() << "Already known: " << *KnownBases[Start].front() << "\n");
    return KnownBases[Start];
  }

  optional<BaseSet> Out = visit(Start);
  assert(Out && !Out->empty());

  SmallVector<Value *> Bases(Out->begin(), Out->end());

  llvm::sort(Bases, [&](Value *A, Value *B) {
    Instruction *AI = dyn_cast<Instruction>(A);
    Instruction *BI = dyn_cast<Instruction>(B);
    if (!AI || !BI) {
      return !AI && BI;
    }
    if (DT.dominates(AI, BI)) {
      return true;
    }
    if (DT.dominates(BI, AI)) {
      return false;
    }
    return false;
  });
  Instruction *BaseInst = dyn_cast<Instruction>(Bases.front());
  Instruction *StartInst = dyn_cast<Instruction>(Start);
  assert(!BaseInst || !StartInst ||
         BaseInst == StartInst ||
         DT.dominates(BaseInst, StartInst));

  KnownBases[Start] = Bases;
  return Bases;
}

Value *PTSanUtils::findBasePointer(Value *Start) {
  return findBasePointers(Start).front();
}


// Compare A to B. So: A == B -> EQ A <= B -> LTE, A >= B -> GTE
CmpResult PTSanUtils::compareSCEVs(const SCEV *A, const SCEV *B) {
  auto calculate = [](const SCEV *A, const SCEV *B, ScalarEvolution &SE) {
    using P = ICmpInst::Predicate;

    if (A->getType() != B->getType()) {
      DBG(errs() << "\t\t\tDifferent scev types: " << *A->getType() << " and " << *B->getType() << "\n");
      return CmpResult::Unknown;
    }

    const SCEV *PtrA = SE.getPointerBase(A);
    const SCEV *PtrB = SE.getPointerBase(B);
    if (PtrA != PtrB) {
      DBG(errs() << "\t\t\tDifferent pointer bases: " << *PtrA << " and " << *PtrB << "\n");
      return CmpResult::Unknown;
    }

    const SCEV *OffsetA, *OffsetB;
    if (A->getType()->isPointerTy()) {
      OffsetA = SE.removePointerBase(A);
      OffsetB = SE.removePointerBase(B);
    } else{
      OffsetA = A;
      OffsetB = B;
    }

    DBG(errs() << "\t\t\tOffsets are:\n");
    DBG(errs() << "\t\t\t\t" << *OffsetA << "\n");
    DBG(errs() << "\t\t\t\t" << *OffsetB << "\n");

    auto CompareOffsets = [&](const SCEV *OffsetA, const SCEV *OffsetB) {
      const SCEV *Dif = SE.getMinusSCEV(OffsetA, OffsetB);
      DBG(errs() << "\t\t\tDiff is " << *Dif << "\n");

      if (SE.isKnownPredicate(P::ICMP_EQ, OffsetA, OffsetB)) {
        DBG(errs() << "\t\t\tEQ!\n");
        return CmpResult::EQ;
      }

      if (SE.isKnownNonPositive(Dif)) {
        DBG(errs() << "\t\t\tLTE!\n");
        return CmpResult::LTE;
      }
      if (SE.isKnownNonNegative(Dif)) {
        DBG(errs() << "\t\t\tGTE!\n");
        return CmpResult::GTE;
      }

      DBG(errs() << "\t\t\tUnknown!\n");
      return CmpResult::Unknown;
    };

    if (!(SE.containsAddRecurrence(OffsetA) && SE.containsAddRecurrence(OffsetB))) {
      CmpResult Res = CompareOffsets(OffsetA, OffsetB);
      if (Res != CmpResult::Unknown) {
        return Res;
      }
    }

    while(isa<SCEVAddRecExpr>(OffsetA) ||
          isa<SCEVAddRecExpr>(OffsetB)) {
      auto *AddRecA = dyn_cast<SCEVAddRecExpr>(OffsetA);
      auto *AddRecB = dyn_cast<SCEVAddRecExpr>(OffsetB);
      if (!(AddRecA && AddRecB)) {
        DBG(errs() << "only one is an addRec!\n");
        return CmpResult::Unknown;
      }

      if (AddRecA->getLoop() != AddRecB->getLoop()) {
        DBG(errs() << "different loops!\n");
        return CmpResult::Unknown;
      }

      if (AddRecA->getStepRecurrence(SE) !=
          AddRecB->getStepRecurrence(SE)) {
        DBG(errs() << "different recurrence amounts\n");
        return CmpResult:: Unknown;
      }

      OffsetA = AddRecA->getStart();
      OffsetB = AddRecB->getStart();

      DBG(errs() << "\t\t\tNew offsets (starts) are:\n");
      DBG(errs() << "\t\t\t\t" << *OffsetA << "\n");
      DBG(errs() << "\t\t\t\t" << *OffsetB << "\n");
    }

    if (SE.containsAddRecurrence(OffsetA) || SE.containsAddRecurrence(OffsetB)) {
      DBG(errs() << "Failed to strip Addrecs. Do this better!\n");
      return CmpResult::Unknown;
    }

    // This is after the addRec check because it explodes if you try
    // to subtract different addRecs
    return CompareOffsets(OffsetA, OffsetB);
  };


  DBG(errs() << "\t\tComparing " << *A << " and \n\t\t\t\t\t" << *B << "\n");

  auto Key = std::make_pair(A, B);
  auto it  = ScevCmpCache.find(Key);
  if (it != ScevCmpCache.end()) {
    return it->second;
  }

  auto SwapKey = std::make_pair(B, A);
  it = ScevCmpCache.find(SwapKey);
  if (it != ScevCmpCache.end()) {
    return swapOpsCmpResult(it->second);
  }

  CmpResult Res = calculate(A, B, SE);
  ScevCmpCache[Key] = Res;

  return Res;
}

SmallPtrSet<Instruction *, 8>
PTSanUtils::getSCEVInstrs(const SCEV *Expr) {
  struct Collector : public SCEVRewriteVisitor<Collector> {
    Collector(ScalarEvolution &SE, SmallPtrSetImpl<Instruction *> &Out) :
      SCEVRewriteVisitor(SE), Out(Out) {}
    const SCEV *visitUnknown(const SCEVUnknown *U) {
      if(Instruction *I = dyn_cast<Instruction>(U->getValue())) {
        //DBG(errs() << "\tFound dep for " << *U << ":\n");
        //DBG(errs() << "\t" << *I << "\n");
        Out.insert(I);
      }
      return U;
    }

    SmallPtrSetImpl<Instruction *> &Out;
  };
  //DBG(errs() << "\tGetting deps for " << *Expr << "\n");
  if (!ScevDepsCache.contains(Expr)) {
    //DBG(errs() << "\t\tCalculating new value \n");
    SmallPtrSet<Instruction *, 8> Out;
    Collector(SE, Out).visit(Expr);
    ScevDepsCache[Expr] = Out;
  }

  auto Out = SmallPtrSet<Instruction *, 8>(ScevDepsCache.at(Expr));
  //DBG(errs() << "\tDeps are:\n");
  //for (Instruction *I : Out) {
  //  DBG(errs() << "\t" << *I << "\n");
  //}

  return Out;
}

bool PTSanUtils::depsDominateLoc(const SCEV *S, Instruction *Loc) {
  DBG(errs() << "Checking if deps for " << *S << " dominate\n\t" << *Loc << "\n");
  for (Instruction *J : getSCEVInstrs(S)) {
    if (!DT.dominates(J, Loc)) {
      DBG(errs() << "Scev does not dominate instr\n");
      return false;
    }
  }

  DBG(errs() << "Scev dominates instr\n");
  return true;
}

void PTSanUtils::indexInstructionSCEVs() {
  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      if (SE.isSCEVable(I.getType())) {
        SCEVToInstrs[SE.getSCEV(&I)].push_back(&I);
      }
    }
  }
}

Instruction *PTSanUtils::findLocalSCEVRepresentative(const SCEV *S,
                                                      Instruction *Loc) {
  auto It = SCEVToInstrs.find(S);
  if (It == SCEVToInstrs.end()) {
    return nullptr;
  }

  for (Instruction *I : It->second) {
    if (SE.getSCEV(I) == S && (I == Loc || DT.dominates(I, Loc))) {
      return I;
    }
  }

  return nullptr;
}

std::shared_ptr<AccessRange> PTSanUtils::getLoopAccessExtent(
  Value *V, Value *TypeSize, Loop *L) {
  const SCEV *S = SE.getSCEV(V);
  if (!SE.containsAddRecurrence(S)) {
    return nullptr;
  }
  DBG(errs() << "Getting loop access extent for " << *V << " in terms of " << L->getName() << "\n");
  DBG(errs() << "Scev is " << *S << "\n");

  DBG(errs() << "Calculating start: \n");
  const SCEV *Start = solveAccessExtent(L, S, /*FindStart=*/true);
  DBG(errs() << "Calculating last: \n");
  const SCEV *Last = solveAccessExtent(L, S, /*FindStart=*/false);
  DBG(errs() << "Done with both\n");
  if (isa<SCEVCouldNotCompute>(Start)|| isa<SCEVCouldNotCompute>(Last)) {
    return nullptr;
  }

  assert(Start->getType()->isPointerTy());
  assert(Last->getType()->isPointerTy());

  Type *U64Ty = IntegerType::get(Ctx, 64);
  Type *U48Ty = IntegerType::get(Ctx, 48);

  const SCEV *StartInt = SE.getPtrToIntExpr(Start, U64Ty);
  const SCEV *LastInt  = SE.getPtrToIntExpr(Last,  U64Ty);

  // Mask = & 0x0000ffffffffffff via zext(trunc(..., i48), i64)
  const SCEV *StartMasked =
    SE.getZeroExtendExpr(SE.getTruncateExpr(StartInt, U48Ty), U64Ty);
  const SCEV *LastMasked  =
    SE.getZeroExtendExpr(SE.getTruncateExpr(LastInt, U48Ty), U64Ty);

  const SCEV *Delta =
    SE.getMinusSCEV(LastMasked, StartMasked, SCEV::NoWrapFlags::FlagNSW);

  DBG(errs() << "Delta is " << *Delta << "\n");
  const SCEV *TS =  SE.getNoopOrAnyExtend(SE.getSCEV(TypeSize), IntegerType::get(Ctx, 64));


  BasicBlock *Preheader = L->getLoopPreheader();
  assert(Preheader && "PTSan expects preheader");
  Instruction *LocalRepLoc = Preheader->getTerminator();

  auto maybeUseLocalIRRepresentative = [&](const SCEV *RangeSCEV) {
    if (!SE.containsAddRecurrence(RangeSCEV)) {
      return RangeSCEV;
    }

    Instruction *Rep = findLocalSCEVRepresentative(RangeSCEV, LocalRepLoc);
    if (!Rep) {
      return RangeSCEV;
    }

    DBG(errs() << "Using local IR representative for loop range SCEV "
               << *RangeSCEV << ": " << *Rep << "\n");
    return SE.getUnknown(Rep);
  };

  // Only form a simple min+size range when ScalarEvolution can prove the
  // ordering of the fully solved endpoints. Recurrence direction alone is not
  // enough after nested recurrences are folded into this loop.
  if (SE.isKnownPredicate(ICmpInst::ICMP_ULE, StartMasked, LastMasked)) {
    DBG(errs() << "Loop increases\n");
    const SCEV *Size = SE.getAddExpr(Delta, TS, NW::FlagNUW);
    return std::make_shared<SimpleAccessRange>(
      SE, maybeUseLocalIRRepresentative(Start),
      maybeUseLocalIRRepresentative(Size));
  }

  if (SE.isKnownPredicate(ICmpInst::ICMP_UGE, StartMasked, LastMasked)) {
    DBG(errs() << "Loop decreases\n");
    const SCEV *Size = SE.getAddExpr(SE.getNegativeSCEV(Delta), TS, NW::FlagNUW);

    return std::make_shared<SimpleAccessRange>(
      SE, maybeUseLocalIRRepresentative(Last),
      maybeUseLocalIRRepresentative(Size));
  }

  DBG(errs() << "Could not solve direction. Will handle at runtime\n");

  return std::make_shared<LoopAccessRange>(
    SE, maybeUseLocalIRRepresentative(Start),
    maybeUseLocalIRRepresentative(Last), maybeUseLocalIRRepresentative(Delta),
    maybeUseLocalIRRepresentative(TS), L);
}

const SCEV * PTSanUtils::solveAccessExtent(
    Loop *L, const SCEV *S, bool FindStart /*else end*/) {

  class Rewriter : public SCEVRewriteVisitor<Rewriter> {
    ScalarEvolution &SE;
    Loop *TargetLoop;
    bool FindStart;
  public:
    bool Failed;

    Rewriter(PTSanUtils &PTA, Loop *TargetLoop, bool FindStart) :
      SCEVRewriteVisitor(PTA.SE), SE(PTA.SE), TargetLoop(TargetLoop),
      FindStart(FindStart), Failed(false) {}

    const SCEV *visitAddRecExpr(const SCEVAddRecExpr *AR) {
      const Loop *ARLoop = AR->getLoop();

      if (SE.isLoopInvariant(AR, TargetLoop)) {
        DBG(errs() << "\tIgnoring External AddRec " << *AR << "\n");
        return AR;
      }

      if (ARLoop != TargetLoop && !TargetLoop->contains(ARLoop)) {
        DBG(errs() << "\tRejecting unrelated AddRec " << *AR << "\n");
        Failed = true;
        return AR;
      }

      DBG(errs() << "Visiting addRec " << *AR << "(" << *AR->getType() << ")\n");

      const SCEV *Step = AR->getStepRecurrence(SE);
      const SCEV *Start = AR->getStart();
      const SCEV *BEC = SE.getBackedgeTakenCount(ARLoop);

      if (isa<SCEVCouldNotCompute>(BEC)) {
        DBG(errs() << "Could not compute backedge taken count\n");
        Failed = true;
        return AR;
      }

      DBG(errs() << "\tStep is " << *Step << ": " << SE.isLoopInvariant(Step, ARLoop) << "\n");
      DBG(errs() << "\tBEC is " << *BEC << ": " << SE.isLoopInvariant(BEC, ARLoop) << "\n");
      DBG(errs() << "\tStart is " << *Start << ": " << SE.isLoopInvariant(Start, ARLoop) << "\n");

      if (FindStart) {
        const SCEV *Start = AR->evaluateAtIteration(SE.getZero(AR->getType()), SE);
        assert(Start->getType() == AR->getType());
        DBG(errs() << "Solved start as " << *Start << "\n");
        return visit(Start);
      }

      const SCEV *Last = AR->evaluateAtIteration(BEC, SE);
      assert(Last->getType() == AR->getType());
      DBG(errs() << "Solved last as " << *Last << "\n");
      return visit(Last);

      #if 0
      if (!(AR->isAffine() &&
            SE.isLoopInvariant(Step, L) &&
            SE.isLoopInvariant(Start, L) &&
            SE.isLoopInvariant(BEC, L))) {
        DBG(errs() << "Not handling non-affine, non-LI AddRecs");
        Failed = true;
        return AR;
      }

      if (FindStart) {
        assert(Start->getType() == AR->getType());
        DBG(errs() << "Solved start as " << *Start << "\n");
        return Start;
      }

      // This is the last value actually accessed in the loop. So
      // this should be GTE base and LT bound
      Type *BigTy = IntegerType::get(SE.getContext(), 64);

      if (!Start->getType()->isPointerTy()) {
        Start = SE.getNoopOrAnyExtend(Start, BigTy);
      }

      const SCEV *Delta =
        SE.getMulExpr(SE.getNoopOrAnyExtend(Step, BigTy),
                      SE.getNoopOrAnyExtend(BEC, BigTy));

      const SCEV *Last = SE.getAddExpr(Start, Delta);
      #endif

    }
  };

  Rewriter R(*this, L, FindStart);
  const SCEV *Out = R.visit(S);
  if (R.Failed) {
    Out = SE.getCouldNotCompute();
  }

  DBG(errs() << "Done with top-level conversion. Got " << *Out << "\n");

  return Out;
}

// XXX: cache these results?
SmallPtrSet<const BasicBlock *, 16> PTSanUtils::getEscapeBlocks(Loop *L) {
  // Collect loop latches + exiting blocks into a StopSet.
  SmallPtrSet<const BasicBlock *, 16> TheBlocks;
  SmallVector<BasicBlock *, 4> TmpBlocks;

  L->getLoopLatches(TmpBlocks);
  for (BasicBlock *LatchBB : TmpBlocks) {
    TheBlocks.insert(LatchBB);
  }

  TmpBlocks.clear();
  L->getExitingBlocks(TmpBlocks);
  for (BasicBlock *ExitBB : TmpBlocks) {
    TheBlocks.insert(ExitBB);
  }

  return TheBlocks;
}

// We can't hoist an instruction out of a loop unless it executes every loop cycle.
// Otherwise we can end up with (for example) an instruction which executes on the last
// cycle of a loop but would be out of bounds on the first cycle (and never executes there)
static bool executesEveryIteration(Instruction *I,
                                   Loop *L,
                                   DominatorTree &DT) {
  BasicBlock *B = I->getParent();

  assert(L->contains(B));

  SmallVector<BasicBlock *, 4> Latches;
  L->getLoopLatches(Latches);
  for (BasicBlock *Latch : Latches) {
    Instruction *T = Latch->getTerminator();
    if (I != T && !DT.dominates(I, T)) {
      DBG(errs() << "\t" << *I << "does not dominate latch " << Latch->getName() << "\n");
      return false;
    }
  }

  SmallVector<BasicBlock *, 4> Exiting;
  L->getExitingBlocks(Exiting);
  for (BasicBlock *Exit : Exiting) {
    Instruction *T = Exit->getTerminator();
    if (I != T && !DT.dominates(I, T)) {
      DBG(errs() << "\t" << *I << "does not dominate exit " << Exit->getName() << "\n");
      return false;
    }
  }

  return true;
}

// Can I put a check for Sink somewhere in BB?
bool PTSanUtils::isValidCheckBlock(BasicBlock *BB, Instruction *Sink) {
  DBG(errs() << "Considering " << BB->getName() << "\n");
  // Trivially true
  if (BB == Sink->getParent()) {
    return true;
  }

  // If this doesn't dominate the sink instruction, it's unsafe
  if (!DT.dominates(BB, Sink->getParent())) {
    DBG(errs() << "\tDoesn't dominate sink\n");
    return false;
  }

  // If this doesn't imply we'll hit the sink instruction, it's unsafe
  if (!PDT.dominates(Sink->getParent(), BB)) {
    DBG(errs() << "\tSink doesn't post-dominate\n");
    return false;
  }

  Loop *SinkLoop = LI.getLoopFor(Sink->getParent());
  Loop *BBLoop  = LI.getLoopFor(BB);

  //  Reject if the insertion point would land *inside* a sibling or
  //  nested loop that the sink is NOT in.
  //
  //  Acceptable positions are
  //    - the very same loop as the sink, OR
  //    - any parent loop of that sink, OR
  //    - outside every loop.
  bool same_loop   = (BBLoop == SinkLoop);
  bool parent_loop = BBLoop && SinkLoop && BBLoop->contains(SinkLoop);
  bool no_loop     = (BBLoop == nullptr);

  if (!(same_loop || parent_loop || no_loop)) {
    DBG(errs() << "\tWould hoist into unrelated loop\n");
    return false;
  }

  // If the sink is in a loop, and it's possible to reach one of
  // loop's ends or latches from IP without going through sink, reject IP.
  if (SinkLoop && (parent_loop || no_loop)) {
    // Loose mode intentionally restores the older postdominance-only rule.
    // This can hoist checks for accesses that do not execute every iteration.
    if (!ClPTSanLooseLoopOptimizations &&
        !executesEveryIteration(Sink, SinkLoop, DT)) {
      DBG(errs() << "\tSink does not execute every iteration. Cannot hoist out of loop\n");
      return false;
    }
    #if 0
    // Same as executesEveryIteration, but works from anywhere
    SmallPtrSet<const BasicBlock *, 16> LoopEscapeBlocks;
    SmallVector<BasicBlock *, 4> TmpBlocks;

    SinkLoop->getLoopLatches(TmpBlocks);
    for (BasicBlock *LatchBB : TmpBlocks) {
      LoopEscapeBlocks.insert(LatchBB);
    }

    TmpBlocks.clear();
    SinkLoop->getExitingBlocks(TmpBlocks);
    for (BasicBlock *ExitBB : TmpBlocks) {
      LoopEscapeBlocks.insert(ExitBB);
    }

    // Sources: start from the IP's block.
    SmallVector<BasicBlock *, 1> Sources;
    Sources.push_back(BB);

    // Cut set
    SmallPtrSet<BasicBlock *, 1> CutSet;
    CutSet.insert(Sink->getParent());

    if (isManyPotentiallyReachableFromMany(Sources,
                                          LoopEscapeBlocks,
                                          &CutSet,
                                          &DT,
                                          &LI)) {
      DBG(errs() << "Found path from IP to loop latch/exit without going through sink. "
                << "Rejecting IP\n");
      return false;
    }
    #endif
  }

  return true;
}

bool PTSanUtils::hoistTargetDominates(HoistTarget Target, Instruction *Pos) {
  Instruction *I = Target.Instr;
  BasicBlock *TargetBB = I->getParent();
  BasicBlock *PosBB = Pos->getParent();

  if (TargetBB != PosBB) {
    return DT.dominates(TargetBB, PosBB);
  }

  if (Target.Side == BEFORE) {
    return I == Pos || I->comesBefore(Pos);
  }

  assert(!I->isTerminator());
  Instruction *After = I->getNextNode();
  return After == Pos || After->comesBefore(Pos);
}


static BasicBlock *getFirstCommonDominatee(DominatorTree &DT, BasicBlock *A, BasicBlock *B) {
  if (DT.dominates(A, B)) {
    return A;
  }

  if (DT.dominates(B, A)) {
    return B;
  }

  // Since neither dominates the other, they are
  // not the same block
  SmallVector<BasicBlock*, 8> I1Descendants;
  DT.getDescendants(A, I1Descendants);

  SmallVector<BasicBlock*, 8> I2Descendants;
  DT.getDescendants(B, I2Descendants);

  SmallPtrSet<BasicBlock*, 8> I2DescendantsSet;
  for (BasicBlock *BB: I2Descendants) {
    I2DescendantsSet.insert(BB);
  }

  for (BasicBlock *BB: I1Descendants) {
    if (DT.dominates(A, BB) && DT.dominates(B, BB) && I2DescendantsSet.count(BB))
      return BB;
  }

  errs() << "Failed to find first common dominatee for " << A->getName() << " and " << B->getName() << "\n";
  errs() << *B->getParent() << "\n";
  die();
}

HoistTarget PTSanUtils::getFirstCommonInsertionPoint(
    std::initializer_list<Value *> Points) {
  if (Points.size() < 2) {
    errs() << "Call this with at least 2\n";
  };

  auto rec = [&] (const HoistTarget &H1, const HoistTarget &H2) -> HoistTarget {
      if (H1.Instr == H2.Instr) {
        // If they don't match, one must be AFTER
        if (H1.Side != H2.Side) {
          return {H1.Instr, AFTER};
        }
        return H1;
      }

      if (DT.dominates(H1.Instr, H2.Instr)) {
        return H2;
      }

      if (DT.dominates(H2.Instr, H1.Instr)) {
        return H1;
      }

      BasicBlock *BB = getFirstCommonDominatee(DT, H1.Instr->getParent(), H2.Instr->getParent());
      return {&*BB->getFirstInsertionPt(), BEFORE};
  };

  optional<HoistTarget> Acc = nullopt;

  DBG(errs() << "FCD between:");
  for (Value *Point : Points) {
    Instruction *I = dyn_cast<Instruction>(Point);
    if (!I) {
      continue;
    }

    DBG(errs() << "\t" << *Point << "\n");
    HoistTarget T = getFirstIPAfter(Point);
    if (!Acc) {
      Acc = T;
      continue;
    }

    DBG(errs() << "\t\tT is " << *T.getIP() << "\n");
    Acc = rec(*Acc, T);
  }

  if (!Acc) {
    Acc = {&*F.getEntryBlock().getFirstInsertionPt(), BEFORE};
  }
  DBG(errs() << "is :\n" << *Acc->getIP() << "\n");

  return *Acc;
}

HoistTarget PTSanUtils::getFirstIPAfter(Value *V) {
  if (PHINode *PHI = dyn_cast<PHINode>(V)) {
    return {&*PHI->getParent()->getFirstInsertionPt(), BEFORE};
  } else if (InvokeInst *II = dyn_cast<InvokeInst>(V)) {
    return {&*II->getNormalDest()->getFirstInsertionPt(), BEFORE};
  } else if (Instruction *I = dyn_cast<Instruction>(V)) {
    assert(!I->isTerminator());
    return {I, AFTER};
  } else {
    return {&*F.getEntryBlock().getFirstInsertionPt(), BEFORE};
  }
}

static bool isLLVMInternalAccess(Value *Ptr) {
  if (CallBase *CB = dyn_cast<CallBase>(Ptr)) {
    auto *Callee = CB->getCalledFunction();
    if (!Callee) {
      return false;
    }

    // XXX: there are a bunch more of these...
    if (Callee->getName() == "__cxa_begin_catch") {
      return true;
    }
  }
  return false;
}


bool PTSanUtils::isKnownSpatiallyValid(Use *U) {
  Value *Ptr = U->get();
  Instruction *I = cast<Instruction>(U->getUser());
  if (isLLVMInternalAccess(Ptr)) {
    return true;
  }

  // If not an internal access, we look at the actual access:
  Type        *AccessTy;
  MaybeAlign   Alignment;
  if (const auto *LI = dyn_cast<LoadInst>(I)) {
    AccessTy  = LI->getType();
    Alignment = LI->getAlign();
  } else if (const auto *SI = dyn_cast<StoreInst>(I)) {
    AccessTy  = SI->getValueOperand()->getType();
    Alignment = SI->getAlign();
  } else if (auto *MI = dyn_cast<AnyMemIntrinsic>(I)) {
    if (U->getOperandNo() == 0) {
      Alignment = MI->getDestAlign();
    } else if (auto *MTI = dyn_cast<MemTransferInst>(MI);
               MTI && U->getOperandNo() == 1) {
      Alignment = MTI->getSourceAlign();
    } else {
      errs() << "Invalid memtransfer inst? " << MI << "\n";
      die();
    }

    auto *LenC = dyn_cast<ConstantInt>(MI->getLength());
    if (!LenC || LenC->isZero()) {
      return false;
    }

    uint64_t Len = LenC->getZExtValue();
    AccessTy = ArrayType::get(Type::getInt8Ty(I->getContext()), Len);
  } else {
    return false;
  }

  if (!Alignment) {
    Alignment = DL.getPrefTypeAlign(AccessTy);
  }

  return isDereferenceableAndAlignedPointer(
    Ptr, AccessTy, *Alignment, DL, I, nullptr, &DT, nullptr
  );
}

bool PTSanUtils::isUnfreeableMemory(Value *V) {
  auto onlyContainsUnfreeables = [](Value *V) -> bool {
    if (isa<Constant>(V)) {
      return true;
    }

    if (CallBase *CB = dyn_cast<CallBase>(V)) {
      auto *Callee = CB->getCalledFunction();
      if (!Callee) {
        return false;
      }

      // XXX: there are a bunch more of these...
      if (Callee->getName() == "__cxa_begin_catch") {
        return true;
      }
    }

    return false;
  };


  if (isa<AllocaInst>(V) || isa<Constant>(V)) {
    return true;
  }

  if (onlyContainsUnfreeables(V)) {
    return true;
  }

  // Load of global should be true, but not neceesarily loads of things
  // in globals.

  if (auto *LI = dyn_cast<LoadInst>(V)) {
    Value *Base = findBasePointer(LI->getPointerOperand());
    if (onlyContainsUnfreeables(Base)) {
      return true;
    }
  }

  return false;
}


/////////////////////////////////
// FindMemAccesses
/////////////////////////////////
void FindMemAccesses::visitStoreInst(StoreInst &I) {
  auto sz = DL.getTypeStoreSize(I.getValueOperand()->getType());
  DBG(errs() << "Visiting store " << I << "\n");
  DBG(errs() << "\tsize is " << sz << "\n");
  PtrOps.push_back(PtrOp(&I, 1, POINT, sz));
}


void FindMemAccesses::visitLoadInst(LoadInst &I) {
  auto sz = DL.getTypeStoreSize(I.getType());
  DBG(errs() << "Visiting load " << I << "\n");
  DBG(errs() << "\tsize is " << sz << "\n");
  PtrOps.push_back(PtrOp(&I, 0, POINT, sz));
}

void FindMemAccesses::visitAtomicRMWInst(AtomicRMWInst &I) {
  // The size read is the size of the value argument
  auto sz = DL.getTypeStoreSize(I.getValOperand()->getType());
  PtrOps.push_back(PtrOp(&I, 0, POINT, sz));
}

void FindMemAccesses::visitAtomicCmpXchg(AtomicCmpXchgInst &I) {
  // The size read is the size of the compare argument -- not the
  // newVal argument, which is always a bool
  auto sz = DL.getTypeStoreSize(I.getCompareOperand()->getType());
  PtrOps.push_back(PtrOp(&I, 0, POINT, sz));
}

void FindMemAccesses::visitIntrinsicInst(IntrinsicInst &I) {
  switch(I.getIntrinsicID()) {
    case Intrinsic::masked_load:
      // XXX: this wants a size field
      PtrOps.push_back(PtrOp(&I, 0, EXOTIC, nullptr));
      return;
    case Intrinsic::masked_store:
      PtrOps.push_back(PtrOp(&I, 1, EXOTIC, nullptr));
      return;
    case Intrinsic::memcpy:
    case Intrinsic::memmove:
    {
      Value *Size = I.getOperand(2);
      PtrOps.push_back(PtrOp(&I, 0, RANGE, Size));
      PtrOps.push_back(PtrOp(&I, 1, RANGE, Size));
      return;
    }
    case Intrinsic::memset:
    {
      Value *Size = I.getOperand(2);
      PtrOps.push_back(PtrOp(&I, 0, RANGE, Size));
      return;
    }
    case Intrinsic::x86_sse_ldmxcsr:
    case Intrinsic::x86_sse_stmxcsr:
      PtrOps.push_back(PtrOp(&I, 0, EXOTIC, 4));
      return;
    case Intrinsic::x86_umonitor:
      PtrOps.push_back(PtrOp(&I, 0));
      return;
    default:
      if (isa<MemIntrinsic>(I)) {
        errs() << "Unhandled memintrinsic: " << I << "\n";
        errs() << "ID is " << I.getIntrinsicID() << "\n";
        die();
      }
  }
}

/////////////////////////////////
// MISC
/////////////////////////////////


CmpResult swapOpsCmpResult(CmpResult C) {
  switch (C) {
    case CmpResult::EQ:
      return CmpResult::EQ;
    case CmpResult::GTE:
      return CmpResult::LTE;
    case CmpResult::LTE:
      return CmpResult::GTE;
    case CmpResult::Unknown:
      return CmpResult::Unknown;
  }
}


/////////////////////////////////
// SCEV Serialization
/////////////////////////////////


// Schema:
//   !{ !"ptsan.loop.bounds",
//      i1 <Complete>,
//      {
//         !"base",
//         <ValueAsMetadata for Base0>,
//         !Range00,
//         !Range01,
//         ...
//       },
//      {
//         !"base",
//         <ValueAsMetadata for Base1>,
//         !Range10,
//         !Range11,
//         ...
//       },

MDNode *SerializeBoundsInfo::serializeSolvedRanges(
  bool Complete,
  DenseMap<Value *, SmallVector<AccessRangePtr>> &SolvedRanges) {

  SmallVector<Metadata *, 8> RootOps;

  // Tag/name
  RootOps.push_back(MDString::get(Ctx, "ptsan.loop.bounds"));

  // Placeholder for complete -- we do this at the end
  RootOps.push_back(nullptr);

  for (auto [Base, Ranges] : SolvedRanges) {
    MDNode *BaseNode = serializeBaseRangeEntry(Base, Ranges);
    if (!BaseNode) {
      Complete = false;
    } else {
      RootOps.push_back(BaseNode);
    }
  }

  // Complete might have changed. Recalculate.
  ConstantInt *CompleteC =
      ConstantInt::get(Type::getInt1Ty(Ctx), Complete ? 1 : 0);
  RootOps[1] = ConstantAsMetadata::get(CompleteC);

  return MDNode::get(Ctx, RootOps);
}

std::pair<bool, DenseMap<Value *, SmallVector<AccessRangePtr>>>
DeserializeBoundsInfo::deserializeSolvedRanges(MDNode *Root) {

  // Expect: !{ !"ptsan.loop.bounds", i1 <Complete>, !Base0, !Base1, ... }
  assert(Root && Root->getNumOperands() >= 2);
  auto *Tag = cast<MDString>(Root->getOperand(0));
  assert(Tag->getString() == "ptsan.loop.bounds");
  auto *CompleteCAM = cast<ConstantAsMetadata>(Root->getOperand(1));

  auto *CompleteCI = cast<ConstantInt>(CompleteCAM->getValue());
  assert(CompleteCI->getType()->isIntegerTy(1));
  bool Complete = CompleteCI->isOne();

  DenseMap<Value *, SmallVector<AccessRangePtr>> BaseToRanges;
  unsigned NumOps = Root->getNumOperands();
  for (unsigned I = 2; I < NumOps; ++I) {
    MDNode *Entry = cast<MDNode>(Root->getOperand(I));
    auto elt = deserializeBaseRangeEntry(Entry);
    if (!elt) {
      Complete = false;
      continue;
    }

    auto [Base, Ranges] = *elt;
    BaseToRanges[Base] = Ranges;
  }

  return std::make_pair(Complete, BaseToRanges);
}


void SerializeBoundsInfo::addLoopTag(Loop *L, MDNode *Tag) {
  assert(Tag != nullptr);

  MDNode *OldID = L->getLoopID();
  SmallVector<Metadata *, 8> NewOps;

  if (OldID != nullptr) {
    // Preserve existing loop metadata (unroll/vectorize hints, etc).
    for (unsigned I = 0; I < OldID->getNumOperands(); I++) {
      NewOps.push_back(OldID->getOperand(I));
    }
  } else {
    // First operand is a self-reference placeholder; we patch it below.
    NewOps.push_back(nullptr);
  }

  NewOps.push_back(Tag);

  MDNode *NewID = MDNode::getDistinct(Ctx, NewOps);
  // Fix the self-reference in operand 0.
  NewID->replaceOperandWith(0, NewID);

  L->setLoopID(NewID);
}

// LoopID is:
//   distinct !{ !LoopID, !llvm.loop.unroll.disable, ..., !PtsanLoopBounds }
//
// and !PtsanLoopBounds is:
//   !{ !"ptsan.loop.bounds", i1 <Complete>, !Base0, !Base1, ... }
MDNode *SerializeBoundsInfo::getPTSanBounds(const MDNode *LoopID) {
  if (LoopID == nullptr) {
    return nullptr;
  }

  unsigned NumOps = LoopID->getNumOperands();
  if (NumOps <= 1) {
    return nullptr;
  }

  for (unsigned I = 1; I < NumOps; I++) {
    Metadata *Op = LoopID->getOperand(I);

    MDNode *Node = dyn_cast<MDNode>(Op);
    if (Node == nullptr || Node->getNumOperands() == 0) {
      continue;
    }

    MDString *Tag = dyn_cast<MDString>(Node->getOperand(0));
    if (Tag == nullptr) {
      continue;
    }

    if (Tag->getString() == "ptsan.loop.bounds") {
      return Node;
    }
  }

  return nullptr;
}

Metadata *SerializeBoundsInfo::serializeType(Type *Ty) {
  // Integer: !"int", i32 <bitwidth>
  if (Ty->isIntegerTy()) {
    unsigned BitWidth = ((IntegerType *)Ty)->getBitWidth();
    Constant *BW = ConstantInt::get(Type::getInt32Ty(Ctx), BitWidth);

    Metadata *Ops[2] = {
      MDString::get(Ctx, "int"),
      ConstantAsMetadata::get(BW),
    };
    return MDNode::get(Ctx, Ops);
  }

  // Pointer: !"ptr", i32 <addrspace>
  if (Ty->isPointerTy()) {
    unsigned AS = Ty->getPointerAddressSpace();
    Constant *ASConst = ConstantInt::get(Type::getInt32Ty(Ctx), AS);

    Metadata *Ops[2] = {
      MDString::get(Ctx, "ptr"),
      ConstantAsMetadata::get(ASConst),
    };
    return MDNode::get(Ctx, Ops);
  }

  // Array: !"arr", i64 <num-elements>, <elem-type-md>
  if (Ty->isArrayTy()) {
    ArrayType *AT = (ArrayType *)Ty;
    Type *ElemTy = AT->getElementType();
    uint64_t NumElts = AT->getNumElements();

    Metadata *ElemMD = serializeType(ElemTy);
    if (!ElemMD) {
      return nullptr;
    }

    Constant *NumEltsC =
      ConstantInt::get(Type::getInt64Ty(Ctx), NumElts);

    Metadata *Ops[3] = {
      MDString::get(Ctx, "arr"),
      ConstantAsMetadata::get(NumEltsC),
      ElemMD
    };
    return MDNode::get(Ctx, Ops);
  }

  // Struct: !"struct", i1 <packed>, i64 <num-fields>, field0Ty, field1Ty, ...
  if (Ty->isStructTy()) {
    StructType *ST = cast<StructType>(Ty);

    if (ST->isOpaque()) {
      errs() << "Unsupported opaque struct type in serializeType: " << *Ty << "\n";
      return nullptr;
    }

    SmallVector<Metadata *, 8> Ops;
    Ops.push_back(MDString::get(Ctx, "struct"));

    bool IsPacked = ST->isPacked();
    Constant *PackedC =
      ConstantInt::get(Type::getInt1Ty(Ctx), IsPacked ? 1 : 0);
    Ops.push_back(ConstantAsMetadata::get(PackedC));

    uint64_t NumElts = ST->getNumElements();
    Constant *NumEltsC =
      ConstantInt::get(Type::getInt64Ty(Ctx), NumElts);
    Ops.push_back(ConstantAsMetadata::get(NumEltsC));

    for (uint64_t I = 0; I < NumElts; I++) {
      Type *ElemTy = ST->getElementType((unsigned)I);
      Metadata *FieldMD = serializeType(ElemTy);
      if (!FieldMD) {
        return nullptr;
      }
      Ops.push_back(FieldMD);
    }

    return MDNode::get(Ctx, Ops);
  }

  errs() << "Unsupported type in serializeType: " << *Ty << "\n";
  return nullptr;
}

Type *DeserializeBoundsInfo::deserializeType(Metadata *M) {
  assert(M);
  MDNode *Node = cast<MDNode>(M);
  assert(Node->getNumOperands() >= 1);

  MDString *Tag = cast<MDString>(Node->getOperand(0));
  StringRef Kind = Tag->getString();

  LLVMContext &Ctx = SE.getContext();

  // Integer: !"int", i32 <bitwidth>
  if (Kind == "int") {
    assert(Node->getNumOperands() == 2);
    ConstantAsMetadata *CAM = cast<ConstantAsMetadata>(Node->getOperand(1));
    ConstantInt *CI = cast<ConstantInt>(CAM->getValue());
    unsigned BitWidth = (unsigned)CI->getZExtValue();
    return IntegerType::get(Ctx, BitWidth);
  }

  // Pointer: !"ptr", i32 <addrspace>
  if (Kind == "ptr") {
    assert(Node->getNumOperands() == 2);
    ConstantAsMetadata *CAM = cast<ConstantAsMetadata>(Node->getOperand(1));
    ConstantInt *CI = cast<ConstantInt>(CAM->getValue());
    unsigned AS = (unsigned)CI->getZExtValue();
    return PointerType::get(Ctx, AS);
  }

  // Array: !"arr", i64 <num-elements>, <elem-type-md>
  if (Kind == "arr") {
    assert(Node->getNumOperands() == 3);
    ConstantAsMetadata *CAM = cast<ConstantAsMetadata>(Node->getOperand(1));
    ConstantInt *CI = cast<ConstantInt>(CAM->getValue());
    uint64_t NumElts = CI->getZExtValue();

    Type *ElemTy = deserializeType(Node->getOperand(2));
    return ArrayType::get(ElemTy, (uint64_t)NumElts);
  }

  // Struct: !"struct", i1 <packed>, i64 <num-fields>, field0Ty, ...
  if (Kind == "struct") {
    assert(Node->getNumOperands() >= 3);

    // packed flag
    ConstantAsMetadata *PackedCAM =
      cast<ConstantAsMetadata>(Node->getOperand(1));
    ConstantInt *PackedCI = cast<ConstantInt>(PackedCAM->getValue());
    bool IsPacked = PackedCI->getZExtValue() != 0;

    // field count
    ConstantAsMetadata *CountCAM =
      cast<ConstantAsMetadata>(Node->getOperand(2));
    ConstantInt *CountCI = cast<ConstantInt>(CountCAM->getValue());
    uint64_t NumElts = CountCI->getZExtValue();

    assert(Node->getNumOperands() == 3 + NumElts &&
           "struct type metadata operand count mismatch");

    SmallVector<Type *, 8> FieldTys;
    FieldTys.reserve((size_t)NumElts);
    for (uint64_t I = 0; I < NumElts; I++) {
      Type *FieldTy = deserializeType(Node->getOperand(3 + I));
      FieldTys.push_back(FieldTy);
    }

    return StructType::get(Ctx, FieldTys, IsPacked);
  }

  errs() << "Unsupported type tag in deserializeType: " << Kind << "\n";
  die();
}


MDNode *SerializeBoundsInfo::getOrCreateValueID(Value *V) {
  Instruction *I = dyn_cast<Instruction>(V);
  assert(I && "value IDs are only for instructions");

  if (MDNode *Existing = I->getMetadata("ptsan.value.id")) {
    DBG(errs() << "Reusing existing value id for " << *I << "\n");
    return Existing;
  }

  // Otherwise, create a fresh distinct ID node.
  SmallVector<Metadata *, 1> Ops;
  Ops.push_back(MDString::get(Ctx, "ptsan.value.id"));

  MDNode *ID = MDNode::getDistinct(Ctx, Ops);

  // Attach to the instruction so future SerializeBoundsInfo instances
  // (or later calls in this one) see and reuse the same ID.
  I->setMetadata("ptsan.value.id", ID);

  DBG(errs() << "Set new value id for " << *I << "\n");
  return ID;
}


Metadata *SerializeBoundsInfo::serializeValue(Value *V) {
  if (isa<GlobalValue>(V) || isa<Constant>(V)) {
    return ValueAsMetadata::get(V);
  }

  if (Argument *Arg = dyn_cast<Argument>(V)) {
    Constant *ArgNo =
      ConstantInt::get(Type::getInt32Ty(Ctx), Arg->getArgNo());

    Metadata *Ops[2] = {
      MDString::get(Ctx, "arg"),
      ConstantAsMetadata::get(ArgNo)
    };

    return MDNode::get(Ctx, Ops);
  }

  MDNode *ID = getOrCreateValueID(V);

  if (isa<CallInst>(V) || isa<AllocaInst>(V) || isa<PHINode>(V) ||
      isa<LoadInst>(V)) {
    Metadata *Ops[2] = {
      MDString::get(Ctx, "lookup"),
      ID
    };

    return MDNode::get(Ctx, Ops);
  }

  if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(V)) {
    // Encode: !"gep", <instr> ElemTyMD, BaseMD, Idx0MD, Idx1MD, ...
    Metadata *ElemTyMD = serializeType(GEP->getSourceElementType());
    Metadata *BaseMD   = serializeValue(GEP->getPointerOperand());
    if (!ElemTyMD || !BaseMD) {
      return nullptr;
    }

    SmallVector<Metadata *, 8> Ops;
    Ops.push_back(MDString::get(Ctx, "gep"));
    Ops.push_back(ID);
    Ops.push_back(ElemTyMD);
    Ops.push_back(BaseMD);

    for (Value *Idx : GEP->indices()) {
      Metadata *IdxMD = serializeValue(Idx);
      if (!IdxMD) {
        return nullptr;
      }
      Ops.push_back(IdxMD);
    }

    return MDNode::get(Ctx, Ops);
  }

  if (BinaryOperator *BO = dyn_cast<BinaryOperator>(V)) {
    // Encode: !"binop", <instr>, i32 <opcode>, LHSMD, RHSMD
    Metadata *LHSMD = serializeValue(BO->getOperand(0));
    Metadata *RHSMD = serializeValue(BO->getOperand(1));
    if (!LHSMD || !RHSMD) {
      return nullptr;
    }

    Constant *OpcC =
      ConstantInt::get(Type::getInt32Ty(Ctx), (unsigned)BO->getOpcode());

    Metadata *Ops[5] = {
      MDString::get(Ctx, "binop"),
      ID,
      ConstantAsMetadata::get(OpcC),
      LHSMD,
      RHSMD
    };

    return MDNode::get(Ctx, Ops);
  }

  if (SelectInst *Sel = dyn_cast<SelectInst>(V)) {
    // Encode: !"select", <instr>, CondMD, TrueMD, FalseMD
    Metadata *CondMD  = serializeValue(Sel->getCondition());
    Metadata *TrueMD  = serializeValue(Sel->getTrueValue());
    Metadata *FalseMD = serializeValue(Sel->getFalseValue());
    if (!CondMD || !TrueMD || !FalseMD) {
      return nullptr;
    }

    Metadata *Ops[5] = {
      MDString::get(Ctx, "select"),
      ID,
      CondMD,
      TrueMD,
      FalseMD
    };

    return MDNode::get(Ctx, Ops);
  }

  if (UnaryOperator *UO = dyn_cast<UnaryOperator>(V)) {
    // Encode: !"unop", <instr>, i32 <opcode>, OpMD
    Metadata *OpMD = serializeValue(UO->getOperand(0));
    if (!OpMD) {
      return nullptr;
    }

    Constant *OpcC =
      ConstantInt::get(Type::getInt32Ty(Ctx), (unsigned)UO->getOpcode());

    Metadata *Ops[4] = {
      MDString::get(Ctx, "unop"),
      ID,
      ConstantAsMetadata::get(OpcC),
      OpMD
    };

    return MDNode::get(Ctx, Ops);
  }

  if (CastInst *CI = dyn_cast<CastInst>(V)) {
    // Encode: !"cast", <instr> i32 <opcode>, SrcValMD, DstTypeMD

    Metadata *SrcMD = serializeValue(CI->getOperand(0));
    Metadata *DstTyMD = serializeType(CI->getType());

    if (!SrcMD || !DstTyMD) {
      return nullptr;
    }

    Constant *OpcC =
        ConstantInt::get(Type::getInt32Ty(Ctx), (unsigned)CI->getOpcode());

    Metadata *Ops[5] = {
      MDString::get(Ctx, "cast"),
      ID,
      ConstantAsMetadata::get(OpcC),
      SrcMD,
      DstTyMD
    };

    return MDNode::get(Ctx, Ops);
  }

  // Catch plain constants (e.g. GEP indices like i64 32, or other scalars).
  // These are stable across passes within a module, so we can just wrap them.
  if (Constant *C = dyn_cast<Constant>(V)) {
    return ValueAsMetadata::get(C);
  }


  return nullptr;
}

bool DeserializeBoundsInfo::isValidToInsert(Value *V) {
  if (V == nullptr) {
    return false;
  }

  if(Instruction *I = dyn_cast<Instruction>(V)) {
    return DT.dominates(I, IP);
  }

  return true;
}

void DeserializeBoundsInfo::rememberDeserializedValue(MDNode *Node, Value *V) {
  Instruction *I = dyn_cast<Instruction>(V);
  if (I == nullptr) {
    return;
  }

  MDNode *ID = cast<MDNode>(Node->getOperand(1));
  for (BasicBlock &BB : *I->getFunction()) {
    for (Instruction &Other : BB) {
      if (&Other != I && Other.getMetadata("ptsan.value.id") == ID) {
        Other.setMetadata("ptsan.value.id", nullptr);
      }
    }
  }

  I->setMetadata("ptsan.value.id", ID);
  IDToValue[ID] = I;
  AmbiguousValueIDs.erase(ID);
}

bool allValidToInsert(DeserializeBoundsInfo *DBI, std::initializer_list<Value *> vals) {
  for (Value *v : vals) {
    if (!DBI->isValidToInsert(v)) {
      return false;
    }
  }
  return true;
}

#define CHECK_VALS(...)                                                        \
  if (!allValidToInsert(this, {__VA_ARGS__})) {                                \
    return nullptr;                                                            \
  }

Value *DeserializeBoundsInfo::deserializeValue(Metadata *M) {
  assert(M);

  // All of these should always be valid
  if (auto *VAM = dyn_cast<ValueAsMetadata>(M)) {
    Value *V = VAM->getValue();
    assert(isValidToInsert(V));
    return V;
  }

  // Non-global:
  MDNode *Node = cast<MDNode>(M);
  assert(Node->getNumOperands() >= 2);

  MDString *Tag = cast<MDString>(Node->getOperand(0));
  IRBuilder<> IRB(IP);

  if (Tag->getString() == "arg") {
    assert(Node->getNumOperands() == 2);
    ConstantAsMetadata *CAM = cast<ConstantAsMetadata>(Node->getOperand(1));
    ConstantInt *CI = cast<ConstantInt>(CAM->getValue());

    unsigned ArgNo = (unsigned)CI->getZExtValue();
    Function *F = IP->getFunction();
    assert(ArgNo < F->arg_size());

    return F->getArg(ArgNo);
  }

  // Instruction:
  MDNode *ID = cast<MDNode>(Node->getOperand(1));
  if (ID && !AmbiguousValueIDs.contains(ID)) {
    if (IDToValue.contains(ID)) {
      Value *V = IDToValue.at(ID);

      if (isValidToInsert(V)) {
        return V;
      }

      if (!CanSynthesize) {
        return nullptr;
      }
    }
  }

  if (Tag->getString() == "lookup") {
    // !"gep", <instr>, ElemTyMD, BaseMD, Idx0MD, Idx1MD, ...
    assert(Node->getNumOperands() == 2);
    // If we get here, we already tried to look it up and it didn't work
    return nullptr;
  }


  if (Tag->getString() == "gep") {
    // !"gep", <instr>, ElemTyMD, BaseMD, Idx0MD, Idx1MD, ...
    assert(Node->getNumOperands() >= 4);
    if (!CanSynthesize) {
      return nullptr;
    }

    Type *ElemTy = deserializeType(Node->getOperand(2));
    Value *Base  = deserializeValue(Node->getOperand(3));
    CHECK_VALS(Base);

    SmallVector<Value *, 4> Indices;
    for (unsigned I = 4, E = Node->getNumOperands(); I < E; I++) {
      Value *Idx = deserializeValue(Node->getOperand(I));
      CHECK_VALS(Idx);
      Indices.push_back(Idx);
    }


    Value *GEP = IRB.CreateGEP(ElemTy, Base, Indices, "deserialized.gep");
    rememberDeserializedValue(Node, GEP);
    return cast<Instruction>(GEP);
  }

  if (Tag->getString() == "binop") {
    // !"binop", <instr>, i32 <opcode>, LHSMD, RHSMD
    assert(Node->getNumOperands() == 5);
    if (!CanSynthesize) {
      return nullptr;
    }

    ConstantAsMetadata *CAM = cast<ConstantAsMetadata>(Node->getOperand(2));
    ConstantInt *CI = cast<ConstantInt>(CAM->getValue());
    unsigned OpcVal = (unsigned)CI->getZExtValue();

    Value *LHS = deserializeValue(Node->getOperand(3));
    Value *RHS = deserializeValue(Node->getOperand(4));
    CHECK_VALS(LHS, RHS);

    Instruction::BinaryOps Opcode =
      (Instruction::BinaryOps)OpcVal;

    Value *BinOp = IRB.CreateBinOp(Opcode, LHS, RHS, "deserialized.binop");
    rememberDeserializedValue(Node, BinOp);
    return BinOp;
  }

  if (Tag->getString() == "select") {
    // !"select", <instr>, CondMD, TrueMD, FalseMD
    assert(Node->getNumOperands() == 5);
    if (!CanSynthesize) {
      return nullptr;
    }

    Value *Cond  = deserializeValue(Node->getOperand(2));
    Value *TrueV = deserializeValue(Node->getOperand(3));
    Value *FalseV = deserializeValue(Node->getOperand(4));
    CHECK_VALS(Cond, TrueV, FalseV);

    Value *Select = IRB.CreateSelect(Cond, TrueV, FalseV,
                                     "deserialized.select");
    rememberDeserializedValue(Node, Select);
    return Select;
  }

  if (Tag->getString() == "unop") {
    // !"unop", <instr>, i32 <opcode>, OpMD
    assert(Node->getNumOperands() == 4);
    if (!CanSynthesize) {
      return nullptr;
    }

    ConstantAsMetadata *CAM = cast<ConstantAsMetadata>(Node->getOperand(2));
    ConstantInt *CI = cast<ConstantInt>(CAM->getValue());
    unsigned OpcVal = (unsigned)CI->getZExtValue();

    Value *Op = deserializeValue(Node->getOperand(3));

    CHECK_VALS(Op);

    Instruction::UnaryOps Opcode =
      (Instruction::UnaryOps)OpcVal;

    Value *UnOp = IRB.CreateUnOp(Opcode, Op, "deserialized.unop");
    rememberDeserializedValue(Node, UnOp);
    return UnOp;
  }

  if (Tag->getString() == "cast") {
    // !"cast", <instr> i32 <opcode>, SrcValMD, DstTypeMD
    assert(Node->getNumOperands() == 5);
    if (!CanSynthesize) {
      return nullptr;
    }

    ConstantAsMetadata *CAM =
        cast<ConstantAsMetadata>(Node->getOperand(2));
    ConstantInt *CI = cast<ConstantInt>(CAM->getValue());
    unsigned OpcVal = (unsigned)CI->getZExtValue();

    Instruction::CastOps Opcode =
        (Instruction::CastOps)OpcVal;

    Value *Src = deserializeValue(Node->getOperand(3));
    Type *DstTy = deserializeType(Node->getOperand(4));

    CHECK_VALS(Src);

    Value *Cast = IRB.CreateCast(Opcode, Src, DstTy, "deserialized.cast");
    rememberDeserializedValue(Node, Cast);
    return Cast;
  }

  errs() << "Unhandled tag type " << Tag->getString() << "\n";
  die();
}

#undef CHECK_VALS

// ["base", <ValueAsMetadata for base>, !Range0, !Range1, ... !RangeN]
MDNode *SerializeBoundsInfo::serializeBaseRangeEntry(Value *Base, SmallVector<AccessRangePtr> &Ranges) {
  assert(Ranges.size() > 0);
  SmallVector<Metadata *, 8> BaseOps;
  BaseOps.push_back(MDString::get(Ctx, "base"));
  Metadata *BaseMD = serializeValue(Base);
  if (!BaseMD) {
    return nullptr;
  }
  BaseOps.push_back(BaseMD);

  for (auto &RangePtr : Ranges) {
    // Assume AccessRange has: MDNode *serializeToMD(LLVMContext &Ctx) const;
    MDNode *RangeMD = RangePtr->toMD(Ctx);
    // If any of these fail to serialize, we don't have complete info
    // on this base, so give up on it entirely
    if (!RangeMD) {
      return nullptr;
    }

    BaseOps.push_back(RangeMD);
  }

  return MDNode::get(Ctx, BaseOps);
}

optional<std::pair<Value *, SmallVector<AccessRangePtr>>>
DeserializeBoundsInfo::deserializeBaseRangeEntry(MDNode *Entry) {
  assert(Entry && Entry->getNumOperands() > 2);
  auto Tag = cast<MDString>(Entry->getOperand(0));
  assert(Tag->getString() == "base");

  Value *Base = deserializeValue(Entry->getOperand(1));
  if (!Base && !CanSynthesize) {
    return nullopt;
  }

  SmallVector<AccessRangePtr> Ranges;
  bool Complete = Base != nullptr;
  for (unsigned I = 2; I < Entry->getNumOperands(); I++) {
    MDNode *RangeNode = cast<MDNode>(Entry->getOperand(I));
    auto Range = AccessRange::fromMD(*this, RangeNode);
    if (!Range) {
      if (!CanSynthesize) {
        return nullopt;
      }
      Complete = false;
      continue;
    }
    Ranges.push_back(Range);
  }

  if (!Complete) {
    return nullopt;
  }

  return std::make_pair(Base, Ranges);
}

// ["scev", i16<scev-type>, fields...]
MDNode *SerializeBoundsInfo::serializeSCEV(const SCEV *S) {
  SmallVector<Metadata *, 8> Ops;
  Ops.push_back(MDString::get(Ctx, "scev"));
  Ops.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt16Ty(Ctx), S->getSCEVType())));

  switch (S->getSCEVType()) {
  case scConstant: {
    const SCEVConstant *SC = cast<SCEVConstant>(S);
    Ops.push_back(ConstantAsMetadata::get(SC->getValue()));
    break;
  }

  case scUnknown: {
    const SCEVUnknown *SU = cast<SCEVUnknown>(S);
    Value *V = SU->getValue();
    Metadata *ValMD = serializeValue(V);
    if (!ValMD) {
      DBG(errs() << "Scev relies on unstable value " << *V << "\n");
      return nullptr;
    }
    Ops.push_back(ValMD);
    break;
  }

  case scSMaxExpr:
  case scUMaxExpr:
  case scSMinExpr:
  case scUMinExpr:
  case scSequentialUMinExpr:
  case scMulExpr:
  case scAddExpr: {
    const SCEVNAryExpr *A = cast<SCEVNAryExpr>(S);
    for (const SCEV *Op : A->operands()) {
      Ops.push_back(serializeSCEV(Op));
    }
    break;
  }

  case scUDivExpr: {
    const SCEVUDivExpr *U = cast<SCEVUDivExpr>(S);
    Ops.push_back(serializeSCEV(U->getLHS()));
    Ops.push_back(serializeSCEV(U->getRHS()));
    break;
  }

  case scTruncate:
  case scZeroExtend:
  case scSignExtend:
  case scPtrToInt: {
    const SCEVCastExpr *C = cast<SCEVCastExpr>(S);
    const SCEV *Op = C->getOperand();
    Type *Ty = C->getType();
    unsigned BitWidth = 0;
    if (Ty->isIntegerTy()) {
      BitWidth = cast<IntegerType>(Ty)->getBitWidth();
    } else {
      BitWidth = 64; // We assume 64-bit ptrs
    }

    Ops.push_back(serializeSCEV(Op));
    Constant *BW = ConstantInt::get(Type::getInt32Ty(Ctx), BitWidth);
    Ops.push_back(ConstantAsMetadata::get(BW));
    break;
  }

  case scVScale: {
    errs() << "Handle vscale\n";
    die();
  }

  case scCouldNotCompute:
  case scAddRecExpr: {
    return nullptr;
  }
  }

  // If anything failed to serialize, propagate failure
  for (Metadata *Op : Ops) {
    if(Op == nullptr) {
      return nullptr;
    }
  }

  return MDNode::get(Ctx, Ops);
}

// ["scev", i16<scev-type>, fields...]
const SCEV *DeserializeBoundsInfo::deserializeSCEV(Metadata *M) {
  assert(M && "Expected non-null metadata for SCEV");
  MDNode *Node = cast<MDNode>(M);
  assert(Node->getNumOperands() >= 2 && "Malformed SCEV MDNode");

  MDString *Tag = cast<MDString>(Node->getOperand(0));
  assert(Tag->getString() == "scev" && "Expected 'scev' tag");

  auto *KindMD = cast<ConstantAsMetadata>(Node->getOperand(1));
  auto *KindCI = cast<ConstantInt>(KindMD->getValue());
  unsigned KindVal = (unsigned)KindCI->getZExtValue();
  SCEVTypes Kind = (SCEVTypes)KindVal;

  switch (Kind) {
  case scConstant: {
    // ["scev", kind, Const]
    assert(Node->getNumOperands() == 3 && "Constant SCEV must have 1 payload");
    auto *ValMD = cast<ConstantAsMetadata>(Node->getOperand(2));
    auto *CInt = cast<ConstantInt>(ValMD->getValue());
    return SE.getConstant(CInt);
  }

  case scUnknown: {
    // ["scev", kind, ValueMD]
    assert(Node->getNumOperands() == 3 && "Unknown SCEV must have 1 payload");
    Metadata *ValMD = Node->getOperand(2);
    Value *V = deserializeValue(ValMD);
    if (!V) {return nullptr;}
    return SE.getSCEV(V);
  }

  case scSMaxExpr:
  case scUMaxExpr:
  case scSMinExpr:
  case scUMinExpr:
  case scSequentialUMinExpr:
  case scMulExpr:
  case scAddExpr: {
    // ["scev", kind, Op0, Op1, ...]
    unsigned NumOps = Node->getNumOperands();
    assert(NumOps >= 4 && "NAry SCEV must have at least 2 operands");

    SmallVector<const SCEV *, 4> Ops;
    for (unsigned I = 2; I < NumOps; I++) {
      const SCEV *Op = deserializeSCEV(Node->getOperand(I));
      if (!Op) {return nullptr;}
      assert(Op && "Failed to deserialize SCEV operand");
      Ops.push_back(Op);
    }

    switch (Kind) {
    case scAddExpr:
      return SE.getAddExpr(Ops);
    case scMulExpr:
      return SE.getMulExpr(Ops);
    case scSMaxExpr:
      return SE.getSMaxExpr(Ops);
    case scUMaxExpr:
      return SE.getUMaxExpr(Ops);
    case scSMinExpr:
      return SE.getSMinExpr(Ops);
    case scUMinExpr:
      return SE.getUMinExpr(Ops);
    case scSequentialUMinExpr:
      return SE.getUMinExpr(Ops, /*Sequential=*/true);
    default:
      errs() << "Unhandled NArg Scev???\n";
      die();
    }
  }

  case scUDivExpr: {
    // ["scev", kind, LHS, RHS]
    assert(Node->getNumOperands() == 4 && "UDiv SCEV must have 2 operands");

    const SCEV *LHS = deserializeSCEV(Node->getOperand(2));
    const SCEV *RHS = deserializeSCEV(Node->getOperand(3));
    if (!LHS || !RHS) {return nullptr;}

    assert(LHS && RHS && "Failed to deserialize operands of SCEVUDivExpr");
    return SE.getUDivExpr(LHS, RHS);
  }

  case scTruncate:
  case scZeroExtend:
  case scSignExtend:
  case scPtrToInt: {
    // ["scev", kind, OpSCEV, BitWidth]
    assert(Node->getNumOperands() == 4 && "Cast SCEV must have Op + BitWidth");

    MDNode *OpNode = cast<MDNode>(Node->getOperand(2));
    auto *BWMD = cast<ConstantAsMetadata>(Node->getOperand(3));
    auto *BWCI = cast<ConstantInt>(BWMD->getValue());
    unsigned BitWidth = (unsigned)BWCI->getZExtValue();

    const SCEV *Op = deserializeSCEV(OpNode);
    if (!Op) {return nullptr;}
    assert(Op && "Failed to deserialize operand of SCEVCastExpr");

    Type *Ty = IntegerType::get(SE.getContext(), BitWidth);

    switch (Kind) {
    case scTruncate:
      return SE.getTruncateExpr(Op, Ty);
    case scZeroExtend:
      return SE.getZeroExtendExpr(Op, Ty);
    case scSignExtend:
      return SE.getSignExtendExpr(Op, Ty);
    case scPtrToInt:
      return SE.getPtrToIntExpr(Op, Ty);
    default:
      llvm_unreachable("Unexpected cast SCEV kind");
    }
  }

  case scVScale:
    llvm_unreachable("scVScale SCEV should never have been serialized");

  case scCouldNotCompute:
  case scAddRecExpr:
    llvm_unreachable("Unsupported SCEV kind in DeserializeBoundsInfo");
  }

  llvm_unreachable("Unknown SCEV kind in DeserializeBoundsInfo");
}


/////////////////////////////////
// ACCESS RANGE
/////////////////////////////////

SmallPtrSet<Instruction *, 8>
AccessRange::getDeps(PTSanUtils *PTU) const {
  SmallPtrSet<Instruction *, 8> Deps;
  for (const SCEV *E : getAllSCEVs()) {
    auto Sub = PTU->getSCEVInstrs(E);
    Deps.insert(Sub.begin(), Sub.end());
  }
  return Deps;
}


bool AccessRange::isSafeToExpandAt(SCEVExpander &Expander, Instruction *Pos) const {
    for (const SCEV *S : getAllSCEVs()) {
      if (!Expander.isSafeToExpandAt(S, Pos)) {
        return false;
      }
    }
    return true;
  }

RangeCmpResult SimpleAccessRange::cmp(PTSanUtils *PTU, const AccessRange *OtherRaw) const {
  auto *Other = dyn_cast<SimpleAccessRange>(OtherRaw);
  if (!Other) {
    return RCR_UNKNOWN;
  }

  const SCEV *ThisMax = SE.getAddExpr(this->Min, this->Size);
  const SCEV *OtherMax = SE.getAddExpr(Other->Min, Other->Size);
  CmpResult MaxCmp = PTU->compareSCEVs(ThisMax, OtherMax);
  CmpResult MinCmp = PTU->compareSCEVs(this->Min, Other->Min);
  if (MaxCmp == CmpResult::Unknown || MinCmp == CmpResult::Unknown) {
    return RCR_UNKNOWN;
  }

  if (MinCmp == CmpResult::EQ && MaxCmp == CmpResult::EQ) {
    return RCR_EQUAL;
  }

  if ((MinCmp == CmpResult::GTE || MinCmp == CmpResult::EQ) &&
      (MaxCmp == CmpResult::LTE || MaxCmp == CmpResult::EQ)) {
      return RCR_SUBSET;
  }

  if ((MinCmp == CmpResult::LTE || MinCmp == CmpResult::EQ) &&
      (MaxCmp == CmpResult::GTE || MaxCmp == CmpResult::EQ)) {
      return RCR_SUPERSET;
  }

  if ((MinCmp == CmpResult::LTE || MinCmp == CmpResult::EQ) &&
      (MaxCmp == CmpResult::LTE || MaxCmp == CmpResult::EQ)) {
      return RCR_LTE;
  }

  if ((MinCmp == CmpResult::GTE || MinCmp == CmpResult::EQ) &&
      (MaxCmp == CmpResult::GTE || MaxCmp == CmpResult::EQ)) {
      return RCR_GTE;
  }

  die();
}

RangeCmpResult LoopAccessRange::cmp(PTSanUtils *PTU, const AccessRange *OtherRaw) const {
  auto *Other = dyn_cast<LoopAccessRange>(OtherRaw);
  if (!Other) {
    return RCR_UNKNOWN;
  }

  if(this->L != Other->L) {
    DBG(errs() << "Skipping cmp against non-same-loop access range\n");
    return RCR_UNKNOWN;
  }

  auto CouldNotCompute = [](const SCEV *S) {
    return isa<SCEVCouldNotCompute>(S);
  };

  if (CouldNotCompute(this->Delta) || CouldNotCompute(Other->Delta)) {
    return RCR_UNKNOWN;
  }

  if (PTU->compareSCEVs(this->Delta, Other->Delta) != CmpResult::EQ) {
    const SCEV *FirstDiff = SE.getMinusSCEV(Other->First, this->First);
    const SCEV *LastDiff = SE.getMinusSCEV(Other->Last, this->Last);
    if (CouldNotCompute(FirstDiff) || CouldNotCompute(LastDiff) ||
        PTU->compareSCEVs(FirstDiff, LastDiff) != CmpResult::EQ) {
      DBG(errs() << "Skipping cmp against loop range with different delta\n");
      return RCR_UNKNOWN;
    }
  }

  const SCEV *ThisFirstEnd = SE.getAddExpr(this->First, this->TypeSize);
  const SCEV *OtherFirstEnd = SE.getAddExpr(Other->First, Other->TypeSize);
  const SCEV *ThisLastEnd = SE.getAddExpr(this->Last, this->TypeSize);
  const SCEV *OtherLastEnd = SE.getAddExpr(Other->Last, Other->TypeSize);
  if (CouldNotCompute(ThisFirstEnd) || CouldNotCompute(OtherFirstEnd) ||
      CouldNotCompute(ThisLastEnd) || CouldNotCompute(OtherLastEnd)) {
    return RCR_UNKNOWN;
  }

  CmpResult FirstCmp = PTU->compareSCEVs(this->First, Other->First);
  CmpResult FirstEndCmp = PTU->compareSCEVs(ThisFirstEnd, OtherFirstEnd);
  CmpResult LastCmp = PTU->compareSCEVs(this->Last, Other->Last);
  CmpResult LastEndCmp = PTU->compareSCEVs(ThisLastEnd, OtherLastEnd);
  if (FirstCmp == CmpResult::Unknown || FirstEndCmp == CmpResult::Unknown ||
      LastCmp == CmpResult::Unknown || LastEndCmp == CmpResult::Unknown) {
    return RCR_UNKNOWN;
  }

  if (FirstCmp == CmpResult::EQ && FirstEndCmp == CmpResult::EQ &&
      LastCmp == CmpResult::EQ && LastEndCmp == CmpResult::EQ) {
    return RCR_EQUAL;
  }

  if ((FirstCmp == CmpResult::GTE || FirstCmp == CmpResult::EQ) &&
      (FirstEndCmp == CmpResult::LTE || FirstEndCmp == CmpResult::EQ) &&
      (LastCmp == CmpResult::GTE || LastCmp == CmpResult::EQ) &&
      (LastEndCmp == CmpResult::LTE || LastEndCmp == CmpResult::EQ)) {
    return RCR_SUBSET;
  }

  if ((FirstCmp == CmpResult::LTE || FirstCmp == CmpResult::EQ) &&
      (FirstEndCmp == CmpResult::GTE || FirstEndCmp == CmpResult::EQ) &&
      (LastCmp == CmpResult::LTE || LastCmp == CmpResult::EQ) &&
      (LastEndCmp == CmpResult::GTE || LastEndCmp == CmpResult::EQ)) {
    return RCR_SUPERSET;
  }

  if ((FirstCmp == CmpResult::LTE || FirstCmp == CmpResult::EQ) &&
      (FirstEndCmp == CmpResult::LTE || FirstEndCmp == CmpResult::EQ) &&
      (LastCmp == CmpResult::LTE || LastCmp == CmpResult::EQ) &&
      (LastEndCmp == CmpResult::LTE || LastEndCmp == CmpResult::EQ)) {
    return RCR_LTE;
  }

  if ((FirstCmp == CmpResult::GTE || FirstCmp == CmpResult::EQ) &&
      (FirstEndCmp == CmpResult::GTE || FirstEndCmp == CmpResult::EQ) &&
      (LastCmp == CmpResult::GTE || LastCmp == CmpResult::EQ) &&
      (LastEndCmp == CmpResult::GTE || LastEndCmp == CmpResult::EQ)) {
    return RCR_GTE;
  }

  return RCR_UNKNOWN;
}

bool SimpleAccessRange::mergeIn(PTSanUtils *PTU, const AccessRange *OtherRaw) {
  auto *Other = dyn_cast<SimpleAccessRange>(OtherRaw);
  if (!Other) {
    errs() << "Should not be merging different typed ranges\n";
    die();
  }

  RangeCmpResult Cmp = this->cmp(PTU, Other);


  const SCEV *ThisEnd  = SE.getAddExpr(this->Min,  this->Size);
  const SCEV *OtherEnd = SE.getAddExpr(Other->Min, Other->Size);

  switch (Cmp) {
    case RCR_UNKNOWN:
      errs() << "Should not be merging unknown order ranges\n";
      die();
    case RCR_SUBSET:
      this->Min = Other->Min;
      this->Size = Other->Size;
      return true;
    case RCR_EQUAL:
    case RCR_SUPERSET:
      // Absorb the other range -- we do nothing
      return false;
    case RCR_GTE:
      // Extend downward: newMin = Other.Min; keep old End.
      this->Min = Other->Min;
      this->Size = SE.getMinusSCEV(ThisEnd, this->Min); // Size' = End - Min
      assert(!SE.isKnownNegative(Size));
      return true;
    case RCR_LTE:
      // Extend upward: keep Min; newEnd = OtherEnd.
      this->Size = SE.getMinusSCEV(OtherEnd, this->Min); // Size' = End - Min
      assert(!SE.isKnownNegative(Size));
      return true;
  }
}

bool LoopAccessRange::mergeIn(PTSanUtils *PTU, const AccessRange *OtherRaw) {
  auto *Other = dyn_cast<LoopAccessRange>(OtherRaw);
  if (!Other) {
    errs() << "Should not be merging different typed ranges\n";
    die();
  }

  RangeCmpResult Cmp = this->cmp(PTU, Other);

  switch (Cmp) {
    case RCR_SUBSET:
      this->First = Other->First;
      this->Last = Other->Last;
      this->Delta = Other->Delta;
      this->TypeSize = Other->TypeSize;
      return true;
    case RCR_EQUAL:
    case RCR_SUPERSET:
      return false;
    case RCR_GTE:
      this->TypeSize =
        SE.getMinusSCEV(SE.getAddExpr(this->First, this->TypeSize),
                        Other->First);
      this->First = Other->First;
      this->Last = Other->Last;
      this->Delta = Other->Delta;
      assert(!SE.isKnownNegative(this->TypeSize));
      return true;
    case RCR_LTE:
      this->TypeSize =
        SE.getMinusSCEV(SE.getAddExpr(Other->First, Other->TypeSize),
                        this->First);
      assert(!SE.isKnownNegative(this->TypeSize));
      return true;
    case RCR_UNKNOWN:
      errs() << "Should not be merging unknown order ranges\n";
      die();
  }
}

bool SimpleAccessRange::isSizeZero() const {
  return SE.isKnownPredicate(ICmpInst::ICMP_EQ, Size, SE.getZero(Size->getType()));
}

bool LoopAccessRange::isSizeZero() const {
  assert(!SE.isKnownPredicate(ICmpInst::ICMP_EQ, TypeSize, SE.getZero(TypeSize->getType()))
         && "You can have zero-size types??");
  return SE.isKnownPredicate(ICmpInst::ICMP_EQ, Delta, SE.getZero(Delta->getType()));
}

std::shared_ptr<AccessRange> SimpleAccessRange::clone() const {
  return std::make_shared<SimpleAccessRange>(SE, Min, Size);
}

std::shared_ptr<AccessRange> LoopAccessRange::clone() const {
  return std::make_shared<LoopAccessRange>(SE, First, Last, Delta, TypeSize, L);
}

std::pair<Value *, Value *>
SimpleAccessRange::materializeMinSize(PTSanUtils *PTU, SCEVExpander &Expander,
                                      Instruction *IP) const {
  (void)PTU;
  LLVMContext &Ctx = IP->getContext();
  Type        *U64Ty = Type::getInt64Ty(Ctx);
  PointerType *PtrTy = PointerType::getUnqual(Ctx);

  if (Expander.getSE()->isKnownNegative(Size)) {
    errs() << "Size should not be negative! " << *Size << "\n";
    die();
  }

  Value *MinVal = Expander.expandCodeFor(Min, PtrTy, IP);
  Value *SizeVal = Expander.expandCodeFor(Size, U64Ty, IP);
  return std::make_pair(MinVal, SizeVal);
}

static bool containsPointerTypedUnknown(PTSanUtils *PTU, const SCEV *S) {
  return any_of(PTU->getSCEVInstrs(S), [](Instruction *I) {
    return I->getType()->isPointerTy();
  });
}

std::pair<Value *, Value *>
LoopAccessRange::materializeMinSize(PTSanUtils *PTU, SCEVExpander &Expander,
                                    Instruction *IP) const {
  LLVMContext &Ctx = IP->getContext();
  Type        *U64Ty = Type::getInt64Ty(Ctx);
  Type        *I48Ty = Type::getIntNTy(Ctx, 48);
  PointerType *PtrTy = PointerType::getUnqual(Ctx);
  ScalarEvolution *SE = Expander.getSE();

  Value *FirstVal = Expander.expandCodeFor(First, PtrTy, IP);
  Value *LastVal = Expander.expandCodeFor(Last, PtrTy, IP);

  const SCEV *FirstInt = SE->getPtrToIntExpr(First, U64Ty);
  const SCEV *LastInt = SE->getPtrToIntExpr(Last, U64Ty);
  const SCEV *ByteDelta = SE->getMinusSCEV(LastInt, FirstInt);

  Value *SizeVal = nullptr;
  if (!isa<SCEVCouldNotCompute>(ByteDelta) && ByteDelta->getType()->isIntegerTy() &&
      !containsPointerTypedUnknown(PTU, ByteDelta)) {
    // When SCEV can expose a pointer-independent byte delta, materialize it
    // directly so LLVM can CSE identical loop-size expressions across bases.
    // If pointer-typed values remain in the delta, keep the low-address-bit
    // materialization below: full-width ptrtoint arithmetic can include tag or
    // high address bits and no longer describes the PTSan checked span.
    const SCEV *AbsDelta = SE->getAbsExpr(ByteDelta, true);
    const SCEV *Size = SE->getAddExpr(AbsDelta, TypeSize);
    SizeVal = Expander.expandCodeFor(Size, U64Ty, IP);
  } else {
    IRBuilder<> SizeIRB(IP);
    Value *FirstIntVal = SizeIRB.CreatePtrToInt(FirstVal, U64Ty, "ptsan.first.int");
    Value *LastIntVal = SizeIRB.CreatePtrToInt(LastVal, U64Ty, "ptsan.last.int");
    Value *FirstLow = SizeIRB.CreateZExt(SizeIRB.CreateTrunc(FirstIntVal, I48Ty), U64Ty,
                                         "ptsan.first.low");
    Value *LastLow = SizeIRB.CreateZExt(SizeIRB.CreateTrunc(LastIntVal, I48Ty), U64Ty,
                                        "ptsan.last.low");
    Value *Forward = SizeIRB.CreateSub(LastLow, FirstLow, "ptsan.forward.delta");
    Value *Backward = SizeIRB.CreateSub(FirstLow, LastLow, "ptsan.backward.delta");
    Value *AbsDelta =
        SizeIRB.CreateBinaryIntrinsic(Intrinsic::smax, Forward, Backward,
                                      nullptr, "ptsan.abs.delta");
    Value *TypeSizeVal = Expander.expandCodeFor(TypeSize, U64Ty, IP);
    SizeVal = SizeIRB.CreateAdd(AbsDelta, TypeSizeVal, "ptsan.loop.size");
  }

  IRBuilder<> IRB(IP);
  Value *MinVal =
      IRB.CreateSelect(IRB.CreateICmpULT(FirstVal, LastVal),
                       FirstVal, LastVal, "ptsan.loop.min");

  return std::make_pair(MinVal, SizeVal);
}

void SimpleAccessRange::dump(int spaces) const {
  errs().indent(spaces) << "Min: " << *Min << "\n";
  errs().indent(spaces) << "Size: " << *Size << "\n";
}

void LoopAccessRange::dump(int spaces) const {
  errs().indent(spaces) << "First: " << *First << "\n";
  errs().indent(spaces) << "Last: " << *Last << "\n";
  errs().indent(spaces) << "Delta: " << *Delta << "\n";
  errs().indent(spaces) << "Type Size: " << *TypeSize << "\n";
}

static Loop *getLoopFromPreheaderInst(Instruction *IP, LoopInfo &LI) {
  BasicBlock *Preheader = IP->getParent();

  // In LoopSimplify, a preheader has a single successor: the header.
  auto *Br = cast<BranchInst>(Preheader->getTerminator());
  assert(Br->getNumSuccessors() == 1 &&
         "Expected canonical preheader with one successor");
  BasicBlock *Header = Br->getSuccessor(0);

  Loop *L = LI.getLoopFor(Header);
  assert(L && L->getHeader() == Header && "Preheader didn't lead to a loop header?");
  return L;
}

AccessRangePtr AccessRange::fromMD(DeserializeBoundsInfo &DBI, MDNode *MD) {
  assert(MD != nullptr && MD->getNumOperands() >= 1);

  MDString *Tag = cast<MDString>(MD->getOperand(0));
  StringRef Kind = Tag->getString();

  if (Kind == "SimpleAccessRange") {
    // !{ !"SimpleAccessRange", !MinMD, !SizeMD }
    assert(MD->getNumOperands() == 3);

    const SCEV *Min = DBI.deserializeSCEV(MD->getOperand(1));
    const SCEV *Size = DBI.deserializeSCEV(MD->getOperand(2));
    if (!Min || !Size) {
      return nullptr;
    }

    return std::make_shared<SimpleAccessRange>(DBI.SE, Min, Size);
  }

  if (Kind == "LoopAccessRange") {
    // !{ !"LoopAccessRange", !FirstMD, !LastMD, !DeltaMD, !TypeSizeMD }
    assert(MD->getNumOperands() == 5);

    const SCEV *First    = DBI.deserializeSCEV(MD->getOperand(1));
    const SCEV *Last     = DBI.deserializeSCEV(MD->getOperand(2));
    const SCEV *Delta    = DBI.deserializeSCEV(MD->getOperand(3));
    const SCEV *TypeSize = DBI.deserializeSCEV(MD->getOperand(4));
    if (!First || !Last || !Delta || !TypeSize ) {
      return nullptr;
    }

    // We should only ever be deserializing loop access ranges in the loop preaheader
    Loop *L = getLoopFromPreheaderInst(DBI.IP, DBI.LI);

    return std::make_shared<LoopAccessRange>(DBI.SE, First, Last, Delta, TypeSize, L);
  }

  // If we ever get here, the metadata is malformed or from a future kind.
  errs() << "Unknown Access Range type " << Kind << "\n";
  die();
}

MDNode *SimpleAccessRange::toMD(LLVMContext &Ctx) const {
  SerializeBoundsInfo SBI(Ctx);
  MDNode *MinMD = SBI.serializeSCEV(Min);
  MDNode *SizeMD = SBI.serializeSCEV(Size);
  if (!MinMD || !SizeMD) {
    DBG(errs() << "Unserializable scev. Returning nullptr\n");
    return nullptr;
  }
  Metadata *BaseOps[3] = {
    MDString::get(Ctx, "SimpleAccessRange"),
    MinMD,
    SizeMD
  };
  return MDNode::get(Ctx, BaseOps);
}

MDNode *LoopAccessRange::toMD(LLVMContext &Ctx) const {
  SerializeBoundsInfo SBI(Ctx);
  MDNode *MinMD = SBI.serializeSCEV(First);
  MDNode *LastMD = SBI.serializeSCEV(Last);
  MDNode *DeltaMD = SBI.serializeSCEV(Delta);
  MDNode *TypeSizeMD = SBI.serializeSCEV(TypeSize);
  if (!MinMD || !LastMD || !DeltaMD || !TypeSizeMD) {
    DBG(errs() << "Unserializable scev. Returning nullptr\n");
    return nullptr;
  }
  Metadata *BaseOps[5] = {
    MDString::get(Ctx, "LoopAccessRange"),
    MinMD,
    LastMD,
    DeltaMD,
    TypeSizeMD,
  };
  return MDNode::get(Ctx, BaseOps);
}

// This must be out-of-line to act as the 'key function' for the virtual class
// AccessRange. I guess??
AccessRange::~AccessRange() = default;

} // namespace llvm::ptsan
