//===-- PTSanOptimization.cpp - PTSan optimization phase -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanOptimizationInternal.h"

#include "llvm/Analysis/ValueTracking.h"

#include <tuple>

namespace llvm::ptsan {

PTSanOptimizer::PTSanOptimizer(PTSanAnalysisState &PTA)
    : PTA(PTA), DS(PTA.DS), DT(PTA.DT), PDT(PTA.PDT), LI(PTA.LI), SE(PTA.SE),
      PTU(PTA.PTU) {
  for (BasicBlock *BB : depth_first(&PTA.F.getEntryBlock())) {
    for (Instruction &I : *BB) {
      if (isBlockingCall(I)) {
        BlockingCalls.push_back(&I);
      }
    }
  }
}

bool PTSanOptimizationPass::run(PTSanAnalysisState &PTA) {
  PTSanOptimizer Optimizer(PTA);
  Optimizer.run();
  return true;
}

optional<uint64_t> getStaticGlobalSize(const GlobalVariable *GV,
                                              const DataLayout &DL) {
  if (!GV || !GV->getValueType()->isSized()) {
    return nullopt;
  }

  TypeSize Size = DL.getTypeAllocSize(GV->getValueType());
  if (Size.isScalable()) {
    return nullopt;
  }

  uint64_t FixedSize = Size.getFixedValue();
  if (GV->isDeclaration() && FixedSize == 0) {
    return nullopt;
  }

  return FixedSize;
}

optional<StaticObjectBounds>
PTSanOptimizer::getStaticBounds(Value *V) {
  if (auto Source = DS.getAsAnchor<SourceAnchor>(V)) {
    auto *AI = dyn_cast<AllocaInst>(Source->getSource());
    if (!AI) {
      return nullopt;
    }

    auto AS = AI->getAllocationSize(DS.DL);
    if (!AS || AS->isScalable()) {
      return nullopt;
    }

    auto *Size =
        cast<SCEVConstant>(SE.getConstant(DS.U64Ty, AS->getFixedValue()));
    return StaticObjectBounds{Source->Call, Size, true};
  }

  if (auto *LI = dyn_cast<LoadInst>(V)) {
    Value *LoadPtr = LI->getPointerOperand();

    if (auto *Proxy = dyn_cast<GlobalValue>(LoadPtr)) {
      if (!DS.GlobalProxyTargets.contains(Proxy)) {
        return nullopt;
      }

      auto *GV = DS.GlobalProxyTargets.at(Proxy);
      optional<uint64_t> SizeBytes = getStaticGlobalSize(GV, DS.DL);
      if (!SizeBytes) {
        return nullopt;
      }
      auto *Size =
          cast<SCEVConstant>(SE.getConstant(DS.U64Ty, *SizeBytes));
      return StaticObjectBounds{LI, Size, !GV->isConstant()};
    }

    return nullopt;
  }

  auto *GV = dyn_cast<GlobalVariable>(V);
  optional<uint64_t> SizeBytes = getStaticGlobalSize(GV, DS.DL);
  if (SizeBytes && !ClInstrumentGlobals) {
    auto *Base = ConstantExpr::getIntToPtr(
        ConstantInt::get(DS.U64Ty, 0x10000), DS.PtrTy);
    auto *Size = cast<SCEVConstant>(
        SE.getConstant(DS.U64Ty, 0x0000ffffffffffffULL));
    return StaticObjectBounds{Base, Size, true};
  }

  if (SizeBytes && !DS.isInstr(GV)) {
    auto *Size = cast<SCEVConstant>(SE.getConstant(DS.U64Ty, *SizeBytes));
    return StaticObjectBounds{GV, Size, !GV->isConstant()};
  }

  return nullopt;
}

bool PTSanOptimizer::isUnfreeableBase(Value *V) {
  if (DS.getAsAnchor<SourceAnchor>(V)) {
    return true;
  }
  return PTU.isUnfreeableMemory(V);
}

bool PTSanOptimizer::isBlockingCall(Instruction &I) {
  if (DS.isInstr(&I)) {
    return false;
  }

  CallBase *CB = dyn_cast<CallBase>(&I);
  if (!CB) {
    return false;
  }

  // Don't break on intrinsics
  if (isa<IntrinsicInst>(CB)) {
    return false;
  }

  // Don't break on functions which won't free or won't return
  Function *Callee = CB->getCalledFunction();
  if (Callee && (Callee->hasFnAttribute(Attribute::NoFree) ||
                 Callee->hasFnAttribute(Attribute::NoReturn))) {
    return false;
  }

  if (Callee && Callee->getName() == "__assert_fail") {
    DBG(errs() << "Skipping assert which doesn't deallocate\n");
    return false;
  }

  // Otherwise this call could deallocate our memory and we don't
  // want to hoist above it
  DBG(errs() << "\tFound call, cutting off hoist here: " << I << "\n");
  return true;
}

// Hoist a bounds load. All we want to do is hoist the load to as close
// to BasePtr as possible, while making sure the underlying pointer isn't
// invalidated by an intervening call.
// This is surprisingly complicated:
HoistTarget PTSanOptimizer::hoistBoundsLoad(Instruction *Start,
                                                Value *BasePtr) {
  auto findLastBlockingCall = [this](BasicBlock::reverse_iterator Begin,
                                        BasicBlock::reverse_iterator End) -> Instruction * {
    for (auto It = Begin; It != End; ++It) {
      if (isBlockingCall(*It)) {
        return &*It;
      }
    }

    return nullptr;
  };

  auto getIPForValue = [&](Value *V, bool deallocatable) -> HoistTarget {
    if (PHINode *PHI = dyn_cast<PHINode>(V)) {
      Instruction *IP = DS.getFirstRealInsertionPt(*PHI->getParent());
      return {IP, deallocatable && isBlockingCall(*IP) ? AFTER : BEFORE};
    }

    if (InvokeInst *II = dyn_cast<InvokeInst>(V)) {
      // This is invalid, but we need it for dominace
      // info anyway
      return {II, AFTER};
    }

    if (Instruction *I = dyn_cast<Instruction>(V)) {
      assert(!I->isTerminator());
      return {I, AFTER};
    }

    // Value case
    Instruction *IP = DS.getFirstRealInsertionPt(
        Start->getParent()->getParent()->getEntryBlock());
    return {IP, deallocatable && isBlockingCall(*IP) ? AFTER : BEFORE};
  };

  if (ClNoBoundsLoadHoist || ClNoPTSanOpts) {
    return {Start, BEFORE};
  }

  // In an ideal world without memory deallocations, we'd like to host this to here:
  DBG(errs() << "\tBase is " << *BasePtr << "\n");
  // Easiest case: Constants and allocas can't be deallocated, so no
  // worries about intervening functions freeing them, so we can
  // go straight to the top
  if (isUnfreeableBase(BasePtr)) {
    DBG(errs() << "Got non-concerning buffer " << *BasePtr << "\n");
    return getIPForValue(BasePtr, false);
  }

  HoistTarget Target = getIPForValue(BasePtr, true);
  bool ValidTarget = !(Target.Instr->isTerminator() && Target.Side == AFTER);

  DBG(errs() << "Target hoist point is " << *Target.Instr << "\n");

  assert(Target.Instr == Start ||
         DT.properlyDominates(Target.Instr->getParent(), Start->getParent())
         || (Target.Instr->getParent() == Start->getParent() &&
             Target.Instr->comesBefore(Start)));

  BasicBlock *StartBB = Start->getParent();
  BasicBlock *TargetBB = Target.Instr->getParent();


  // Easy case: Start and Target are in the same BB:
  if (Start->getParent() == Target.Instr->getParent()) {
    // Iterate backward through the BB, find the 'first' (from the back) call
    // between them
    // The beginning is inclusive and the end is not. In this case, we don't care about
    // start (since that's where we're using it), but we *do* care about the end. So both
    // iterators here are incremented.
    Instruction *Call = findLastBlockingCall(++BasicBlock::reverse_iterator(Start),
                                     ++BasicBlock::reverse_iterator(Target.Instr));
    // If that call exists, we insert after it
    if (Call) {
      // Call is in the middle of a block so shouldn't be an invoke
      assert(!isa<InvokeInst>(Call));
      DBG(errs() << "Got same-block call\n");
      return {Call, AFTER};
    }

    // Otherwise we can host all the way to the target
    DBG(errs() << "Same-block safe\n");
    assert(ValidTarget &&
      "Invalid target -> target is terminator -> target cannot dominate and be in same block as start");
    return Target;
  }

  // Easy case: There is a call within Start's BB -- we can't hoist above it
  {
    Instruction *Call = findLastBlockingCall(++BasicBlock::reverse_iterator(Start),
                                     StartBB->rend());
    if (Call) {
      // Call is between start of block and start so not a terminator,
      // so not an invoke
      assert(!isa<InvokeInst>(Call));
      DBG(errs() << "Can't hoist above start block\n");
      return {Call, AFTER};
    }
  }

  // Hard case: We need to handle the full function CFG
  // First, find our poison calls -- if these are called we can't guarantee
  // our buffer isn't invalidated
  SmallPtrSet<const BasicBlock *, 16> PoisonBlocks;
  for (Instruction *Call : BlockingCalls) {
    if (isPotentiallyReachable(Call, Start, nullptr, &DT, &LI)) {
      PoisonBlocks.insert(Call->getParent());
      DBG(errs() << "\tGot call block " << Call->getParent()->getName() << "\n");
    }
  }

  // Easy-hard case: If there are no poison blocks, we can hoist the whole way
  if (ValidTarget && PoisonBlocks.empty()) {
    DBG(errs() << "No call blocks\n");
    return Target;
  }

  // Hard-hard case. There is poison, there is a full CFG. First thing,
  // find our list of potential insert points:
  SmallVector<HoistTarget, 32> InsertPoints;

  // First find the highest we can hoist within Target's BB. We don't add
  // startBB yet -- that shows up at the end
  {
    Instruction *Call = findLastBlockingCall(TargetBB->rbegin(),
                                     // The end is noninclusive, and we want to include Target.Instr, so
                                     // increment the end here
                                     ++BasicBlock::reverse_iterator(Target.Instr));
    DBG(errs() << "\tCall for Target Block: " << Call << "\n");
    if (ValidTarget) {
      if (Call) {
        DBG(errs() << "\t\t" << *Call << "\n");
        // If target block ends in a (potentially freeing) invoke, we can't hoist to it
        if (!isa<InvokeInst>(Call)) {
          InsertPoints.push_back({Call, AFTER});
        }
      } else {
        // A nonblocking invoke can be a legal BEFORE insertion point, for
        // example when it immediately follows a pointer PHI.
        InsertPoints.push_back(Target);
      }
    }
  }

  // Candidate blocks dominate StartBB and are dominated by TargetBB: walk
  // that dominator-tree chain instead of searching the entire function.
  for (auto *Node = DT.getNode(StartBB)->getIDom();
       Node && Node->getBlock() != TargetBB; Node = Node->getIDom()) {
    BasicBlock *BB = Node->getBlock();
    Instruction *Call = findLastBlockingCall(BB->rbegin(), BB->rend());
    if (Call) {
      // A potentially freeing invoke prevents insertion in this block.
      if (!isa<InvokeInst>(Call)) {
        InsertPoints.push_back({Call, AFTER});
      }
    } else {
      InsertPoints.push_back({DS.getFirstRealInsertionPt(*BB), BEFORE});
    }
  }

  // Now filter. If a block can reach a call, and that call can reach our
  // use (Start), that block is no good since there potentially exists a
  // path where the bounds are invalidated


  // Now filter. We know that each IP doesn't have a call after it within
  // its basic block, so we're really interested in successor blocks:
  SmallVector<HoistTarget, 8> ValidIPs;
  for (HoistTarget Elt : InsertPoints) {
    BasicBlock *BB = Elt.Instr->getParent();
    SmallVector<BasicBlock *, 4> Succs;
    Succs.append(succ_begin(BB), succ_end(BB));
    // If any of the poison blocks (which can include BB!) are reachable
    // from the *successors* of this block, this block is a bad prospect
    if (Succs.empty() ||
        !isManyPotentiallyReachableFromMany(Succs, PoisonBlocks, nullptr, &DT, &LI)) {
      DBG(errs() << "Found valid insert block " << BB->getName() << "\n");
      ValidIPs.push_back(Elt);
    }
  }

  // (Yes, these two steps (^ and ⌄) could be combined. But I think it's clearer
  // if they're not, and this is complicated enough)

  // Now! We have our list. We want the highest possible insert point. We
  // know that, at worst, we can hoist to the top of StartBB:
  HoistTarget FinalIP = {DS.getFirstRealInsertionPt(*StartBB), BEFORE};

  for (HoistTarget Elt : ValidIPs) {
    if (DT.dominates(Elt.Instr, FinalIP.Instr)) {
      FinalIP = Elt;
    }
  }

  return FinalIP;
}


SmallPtrSet<Instruction *, 32>
PTSanOptimizer::getValidCheckIPs(HoistTarget MustDominate,
                                      Instruction *Sink, AccessRange &Range) {
  DBG(errs() << "\n----Getting checkIP-----\n");
  SCEVExpander Expander(SE, DS.DL, "valid-check-ips");
  SmallPtrSet<Instruction *, 32> CheckIPs;
  SmallPtrSet<Instruction *, 8> Deps = Range.getDeps(&PTU);

  auto getFirstIPAfterMustDominateInBlock = [&](BasicBlock *BB) {
    Instruction *Start = &*BB->getFirstInsertionPt();
    Instruction *IP = MustDominate.getIP();
    if (IP->getParent() == BB && Start->comesBefore(IP)) {
      Start = IP;
    }
    return Start;
  };

  for (BasicBlock *BB : depth_first(&PTA.F.getEntryBlock())) {
    if (!PTU.isValidCheckBlock(BB, Sink)) {
      continue;
    }

    // Bounds have to be live here as well
    if (!DT.dominates(MustDominate.getIP()->getParent(), BB)) {
      DBG(errs() << "\tBounds isn't live\n");
      continue;
    }

    // XXX: This is really inefficient. I should do ranges
    // If we get here, this is a prospect. Now:
    BasicBlock::iterator StartIP;
    if (BB == Sink->getParent()) {
      // We handle this below
      DBG(errs() << "\tSkipping parent block until later\n");
      continue;
    } else {
      StartIP = getFirstIPAfterMustDominateInBlock(BB)->getIterator();
    }

    for (Instruction &NewIP : make_range(StartIP, BB->end())) {
      if (DS.isAnchor(&NewIP)) {
        continue;
      }

      DBG(errs() << "Considering " << NewIP << "\n");
      // Now, is this actually a valid expansion target?
      if (!PTU.depsDominateLoc(Deps, &NewIP)) {
        continue;
      }
      if (!Range.isSafeToExpandAt(Expander, &NewIP)) {
        DBG(errs() << "\tNot provably safe to expand at\n");
        continue;
      }
      DBG(errs() << "\tValid IP\n");
      CheckIPs.insert(&NewIP);
    }
  }

  // Special-case the sink's block -- because we may want to merge
  // multiple sinks with the same bounds but different, well, sinks,
  // this has to be handled per-instruction. We can assume that any weird
  // order or dominance stuff is valid in the check BB (since the check/
  // access is definitely live), so we don't use isSafeToExpandAt,
  // which is too conservative:

  BasicBlock *BB = Sink->getParent();
  auto IPs =  make_range(BB->getFirstInsertionPt(), BB->end());
  for (Instruction &IP : IPs) {
    if (DS.isAnchor(&IP)) {
      continue;
    }

    DBG(errs() << "Considering " << IP << "\n");
    bool ValidIP = PTU.depsDominateLoc(Deps, &IP);

    if (!PTU.hoistTargetDominates(MustDominate, &IP)) {
      DBG(errs() << "\tBoundsIP " << *MustDominate.getIP()
                 << "\n\t\tdoes not dominate-or-is IP^\n");
      ValidIP = false;
    }

    if (ValidIP) {
      DBG(errs() << "\tgot extra IP of " << IP << "\n");
      CheckIPs.insert(&IP);
    }

    if (&IP == Sink) {
      DBG(errs() << "\tgot last IP of " << *Sink << "\n");
      break; // nothing after I is allowed
    }

  }

  DBG(errs() << "------Done with checkIP-----\n\n");

  return CheckIPs;
}

optional<PlaceablePtrCheck>
PTSanOptimizer::buildLoopHoistedCheck(const PlaceablePtrCheck &Check) {
  if (Check.Mask) {
    return nullopt;
  }

  const auto &PointRange = cast<SimpleAccessRange>(*Check.Range);
  Instruction *I = cast<Instruction>(Check.U->getUser());

  Loop *L = LI.getLoopFor(I->getParent());
  if (!L) {
    return nullopt;
  }

  SmallVector<Loop *, 8> Ancestors;
  for (Loop *p = L; p != nullptr; p = p->getParentLoop()) {
    Ancestors.push_back(p);
  }

  // Work from out to in
  for (Loop *L : reverse(Ancestors)) {
    DBG(errs() << "Check for Loop op in terms of\n\t" << *L << "\n");

    // Quick check to make sure it's possible to remove the check from
    // this loop at all
    if (!PTU.isValidCheckBlock(L->getLoopPreheader(), I)) {
      DBG(errs() << "Loop preaheader is not a valid location for instruction. Skipping!\n");
      DBG(errs() << "\tInstruction block is " << I->getParent()->getName() << "\n");
      DBG(errs() << "\tHeader block is " << L->getHeader()->getName() << "\n");
      continue;
    }

    auto AccessExtent = PTU.getLoopAccessExtent(PointRange, L);

    if (!AccessExtent) {
      DBG(errs() << "\tFailed to find access extent\n");
      continue;
    }

    DBG(errs() << "\t\tAccessed range:\n");
    DBG(AccessExtent->dump(4));

    auto CheckIPs =
        getValidCheckIPs(Check.Anchor.getBoundsTarget(PTU), I, *AccessExtent);

    // We don't want to hoist if it's not out of the loop
    CheckIPs.remove_if([&](Instruction *I) {
      return !DT.properlyDominates(I->getParent(), L->getHeader());
    });


    if (CheckIPs.size() == 0) {
      DBG(errs() << "\tNo good hoist point -- returning nullopt\n");
      continue;
    }

    DBG(errs() << "Successfully analyzed loop op!\n");
    auto Out = PlaceablePtrCheck(Check, CheckIPs, AccessExtent);
    Out.CheckTy = CHK_TY::LOOP;
    return Out;
  }

  return nullopt;
}

PlaceablePtrCheck PTSanOptimizer::placePtrSink(const BoundedPtrCheck &Check) {
  Instruction *I = cast<Instruction>(Check.U->getUser());
  Value *Ptr = Check.getUnstrippedPtr(DS);
  DBG(errs() << "\nAnalyzing Use of\n\t"
             << *Ptr << " in\n\t"
             <<  *I  << "\n");

  auto CheckIPs = getValidCheckIPs(Check.Anchor.getBoundsTarget(PTU), I,
                                   *Check.Range);

  PlaceablePtrCheck Out(Check, CheckIPs, Check.Range);

  return Out;
}

void PTSanOptimizer::applyLoopCheckHoisting() {
  if (isLoopCheckHoistDisabled()) {
    return;
  }

  auto AccessAnchors = DS.collectAnchors<AccessAnchor>(PTA.F);

  for (AccessAnchor Access : AccessAnchors) {
    PlaceablePtrCheck &Check = PTA.PlaceablePtrChecks.at(Access.Call);
    assert(Check.CheckTy != CHK_TY::COMPOUND && "Run hoisting before merge");

    if (auto LoopCheck = buildLoopHoistedCheck(Check)) {
      CallInst *Call = Access.Call;
      PTA.PlaceablePtrChecks.at(Call) = *LoopCheck;
      Call->moveBefore(LoopCheck->getCheckIP(DT));
    }
  }
}

bool PTSanOptimizer::isRemovableCheck(const BoundedPtrCheck &Op) {
  assert(Op.U && Op.BasePtr && Op.Range);
  Value *Ptr = Op.getUnstrippedPtr(DS);

  // Keep this separate from the SCEV fixed-object proof below. LLVM's
  // dereferenceability/alignment facts can prove some accesses that our
  // object-range comparison cannot.
  if (PTU.isKnownSpatiallyValid(Op.U) && isUnfreeableBase(Op.BasePtr)) {
    DBG(errs() << "\tSkipping check for inbounds, nonfreeable "
               << *Op.U->getUser() << "\n");
    return true;
  }

  if (Op.Range->isSizeZero()) {
    DBG(errs() << "\tSkipping check for size-0 access " << *Ptr << "\n");
    return true;
  }

  if (optional<StaticObjectBounds> Bounds = getStaticBounds(Op.BasePtr)) {
    const SCEV *Base = SE.getSCEV(Bounds->Base);
    SimpleAccessRange ObjectRange(SE, Base, Bounds->Size);
    RangeCmpResult Cmp = Op.Range->cmp(&PTU, &ObjectRange);
    if (Cmp == RCR_SUBSET || Cmp == RCR_EQUAL) {
      DBG(errs() << "\tSkipping statically in-bounds object access "
                 << *Op.U->getUser() << "\n");
      return true;
    }
  }

  return false;
}

bool PTSanOptimizer::shouldEraseRemovableCheck(const BoundedPtrCheck &Op) {
  if (isRemovableCheckFilteringDisabled()) {
    return false;
  }

  return isRemovableCheck(Op);
}

void PTSanOptimizer::eraseRemovableChecks() {
  DBG(errs() << "----------------------Filtering bounded ptr ops-----------------\n");
  for (AccessAnchor Access : DS.collectAnchors<AccessAnchor>(PTA.F)) {
    const PlaceablePtrCheck &Check = PTA.PlaceablePtrChecks.at(Access.Call);
    if (ClNoChecks || shouldEraseRemovableCheck(Check)) {
      eraseCheckAnchor(Access);
    }
  }
  DBG(errs() << "----------------Done Filtering bounded ptr ops-----------------\n");
}

bool PTSanOptimizer::mergeSubsumingChecks() {
  if (isCheckMergeDisabled()) {
    return false;
  }

  DBG(errs() << "-----------Merging same addr checks--------------\n");
  DBG(
    SE.verify();
    assert(DT.verify());
    assert(PDT.verify());
    LI.verify(DT);
  );

  auto Checks = DS.collectAnchors<CheckAnchor>(PTA.F);

  // Bound anchors -> checks using those bounds.
  DenseMap<std::pair<Value *, Value *>, SmallVector<CheckAnchor, 4>> M;
  bool Changed = false;

  for (CheckAnchor NewAnchor : Checks) {
    PlaceablePtrCheck &NewCheck = PTA.PlaceablePtrChecks.at(NewAnchor.Call);
    bool Merged = false;

    DBG(errs() << "\n\nConsidering check covering:\n");
    DBG(for (Use *U : NewCheck.UsesCovering) {
      errs() << "\t" << *U->getUser() << "\n";
    });

    Value *NewBaseBound = NewCheck.Anchor.getBaseBound().Call;
    Value *NewSpanBound = NewCheck.Anchor.getSpanBound().Call;
    auto Key = std::make_pair(NewBaseBound, NewSpanBound);

    if (M.contains(Key)) {
      auto &EstablishedChecks = M.at(Key);
      for (CheckAnchor EAnchor : EstablishedChecks) {
        PlaceablePtrCheck &ECheck = PTA.PlaceablePtrChecks.at(EAnchor.Call);
        assert(NewCheck.BasePtr == ECheck.BasePtr);
        assert(NewBaseBound == ECheck.Anchor.getBaseBound().Call);
        assert(NewSpanBound == ECheck.Anchor.getSpanBound().Call);


        auto getCoveringIPs = [&](PlaceablePtrCheck &Superset,
                                  PlaceablePtrCheck &Subset) {
          SmallPtrSet<Instruction *, 32> IPs;
          for (Instruction *IP : Superset.CheckIPs) {
            bool CoversAllUses = llvm::all_of(Subset.UsesCovering, [&](Use *U) {
              return DT.dominates(IP, cast<Instruction>(U->getUser()));
            });
            if (CoversAllUses) {
              IPs.insert(IP);
            }
          }
          return IPs;
        };

        PlaceablePtrCheck *UseThisCheck = nullptr;
        SmallPtrSet<Instruction *, 32> CoveringIPs;
        RangeCmpResult Cmp = NewCheck.Range->cmp(&PTU, ECheck.Range.get());
        switch (Cmp) {
          case RCR_SUPERSET:
            if (CoveringIPs = getCoveringIPs(NewCheck, ECheck); CoveringIPs.size()) {
              UseThisCheck = &NewCheck;
            }
            break;
          case RCR_SUBSET:
            if (CoveringIPs = getCoveringIPs(ECheck, NewCheck); CoveringIPs.size()) {
              UseThisCheck = &ECheck;
            }
            break;
          case RCR_EQUAL:
            if (CoveringIPs = getCoveringIPs(ECheck, NewCheck); CoveringIPs.size()) {
              UseThisCheck = &ECheck;
            } else if (CoveringIPs = getCoveringIPs(NewCheck, ECheck); CoveringIPs.size()) {
              UseThisCheck = &NewCheck;
            }
            break;
          default:
            DBG(errs() << "\tNo sub/super relationship\n");
        }

        if (!UseThisCheck) {
          DBG(errs() << "\tSuperset check if exists does not dominate all covered uses\n");
          continue;
        }

        assert(CoveringIPs.size());
        Merged = true;
        if (UseThisCheck == &NewCheck) {
          ECheck.Range = NewCheck.Range;
          ECheck.InstigatingUses = NewCheck.InstigatingUses;
        }
        ECheck.CheckIPs = std::move(CoveringIPs);
        ECheck.CheckTy = COMPOUND;
        ECheck.UsesCovering.insert(NewCheck.UsesCovering.begin(),
                                   NewCheck.UsesCovering.end());
        EAnchor.Call->moveBefore(ECheck.getCheckIP(DT));
        eraseCheckAnchor(NewAnchor);
        Changed = true;
        DBG(errs() << "\tMerged subset into superset check\n");
        break;
      }
    }

    if (!Merged) {
      M[Key].push_back(NewAnchor);
    }
  }

  return Changed;
}

bool PTSanOptimizer::mergeOrderableChecks() {
  if (isCheckMergeDisabled()) {
    return false;
  }

  DBG(errs() << "\n\n-----------Merging orderable addr checks--------------\n");

  DBG(
    SE.verify();
    assert(DT.verify());
    assert(PDT.verify());
    LI.verify(DT);
  );

  auto Checks = DS.collectAnchors<CheckAnchor>(PTA.F);

  // Bound anchors -> checks using those bounds.
  DenseMap<std::pair<Value *, Value *>, SmallVector<CheckAnchor, 4>> M;
  bool Changed = false;

  for (CheckAnchor NewAnchor : Checks) {
    PlaceablePtrCheck &NewCheck = PTA.PlaceablePtrChecks.at(NewAnchor.Call);
    bool Merged = false;

    DBG(errs() << "Orderable addr compound check for\n");
    DBG(errs() << "  Base ptr: " << *NewCheck.BasePtr << "\n");
    DBG(errs() << "  Uses:\n");
    DBG(for (Use *U : NewCheck.UsesCovering) {
      errs() << "    " << *U->getUser() << "\n";
    });

    Value *NewBaseBound = NewCheck.Anchor.getBaseBound().Call;
    Value *NewSpanBound = NewCheck.Anchor.getSpanBound().Call;
    auto Key = std::make_pair(NewBaseBound, NewSpanBound);

    if (M.contains(Key)) {
      auto &EstablishedChecks = M.at(Key);
      for (CheckAnchor EAnchor : EstablishedChecks) {
        PlaceablePtrCheck &ECheck = PTA.PlaceablePtrChecks.at(EAnchor.Call);
        assert(NewBaseBound == ECheck.Anchor.getBaseBound().Call);
        assert(NewSpanBound == ECheck.Anchor.getSpanBound().Call);
        RangeCmpResult Cmp = NewCheck.Range->cmp(&PTU, ECheck.Range.get());

        // Sub/super is handled in a different optimization
        if (!(Cmp == RCR_GTE || Cmp == RCR_LTE)) {
          DBG(errs() << "Could not establish order\n");
          continue;
        }

        DBG(errs() << "Finding IP intersection for compound check for\n");
        DBG(for (Use *U: ECheck.UsesCovering) {
          errs() << "\t" << *U->getUser() << "\n";
        });

        // Because we do set intersection here, we are guaranteed that any check IP will
        // be postdominated by all instigating instructions
        auto CommonIPs = set_intersection(NewCheck.CheckIPs, ECheck.CheckIPs);
        if (CommonIPs.size() == 0) {
          #if 0 //This is only useful for very specific debugging runs
          DBG(errs() << "\tFailed to find common IP between otherwise-compatible checks\n");


          DBG(errs() << "\tNewCheck IPs were: \n");
          SmallPtrSet<BasicBlock *, 8> NewCheckBBs;
          for (Instruction *I : NewCheck.CheckIPs) {
            NewCheckBBs.insert(I->getParent());
            DBG(errs() << "\t\t" << *I << "\n");
          }

          DBG(errs() << "\tECheck IPs were: \n");
          SmallPtrSet<BasicBlock *, 8> ECheckBBs;
          for (Instruction *I: ECheck.CheckIPs) {
            ECheckBBs.insert(I->getParent());
            DBG(errs() << "\t\t" << *I << "\n");
          }

          auto CommonBBs = set_intersection(NewCheckBBs, ECheckBBs);
          if (CommonBBs.size() > 0) {
            DBG(errs() << "\tChecks have common BBs!\n");
            for (BasicBlock *BB : CommonBBs) {
              DBG(errs() << "\t\t" << BB->getName() << "\n");
            }
            // This can happen valid-ly: Pretend we have something like:
            // unsigned char buf[100];
            // a = buf[i]
            // b = buf[i + a];
            // In this case, we know that a <= b, so we have an ordering. But,
            // - We can't check above a, because a hasn't run yet so we don't know
            //   the bounds of b's access
            // - We can't check after a, since we have to check before all accesses
            // So we give up
          }
          #endif
          continue;
        }

        ECheck.CheckIPs = CommonIPs;

        DBG(errs() << "\tMerging!\n");

        Merged = true;
        ECheck.CheckTy = COMPOUND;
        ECheck.InstigatingUses.insert(
          NewCheck.InstigatingUses.begin(),
          NewCheck.InstigatingUses.end());

        ECheck.UsesCovering.insert(
          NewCheck.UsesCovering.begin(),
          NewCheck.UsesCovering.end());

        ECheck.Range->mergeIn(&PTU, NewCheck.Range.get());

        DBG(errs() << "\t\tMerged:\n");
        DBG(ECheck.Range->dump(4));

        EAnchor.Call->moveBefore(ECheck.getCheckIP(DT));
        eraseCheckAnchor(NewAnchor);
        Changed = true;
        break;
      }
    }

    if (!Merged) {
      M[Key].push_back(NewAnchor);
    }
  }

  DBG(errs() << "\n\n----------Check merging done-----------\n\n");
  return Changed;
}

bool isKnownZeroAccessSize(Value *Size) {
  if (auto *SizeConst = dyn_cast_or_null<ConstantInt>(Size)) {
    return SizeConst->isZero();
  }

  return false;
}

BoundedPtrCheck PTSanOptimizer::analyzePtrAccess(AccessAnchor Access) {
  unsigned OperandIndex = Access.getOperandIndex();
  CHK_TY CheckTy = Access.getCheckType();
  Value *Size = Access.getSize();

  assert(CheckTy != NO_CHECK && !isKnownZeroAccessSize(Size)
        && "These should not have been emitted");

  Use *U = Access.getAccessedUse();
  Instruction *I = cast<Instruction>(U->getUser());

  Value *Ptr = BoundedPtrCheck::getUnstrippedPtrFromUse(U, DS);
  Value *BasePtr = findPTSanBasePointer(Ptr, PTU);

  if (auto Range = PTU.getScalarEnvelope(Ptr, Size, Access.getMask(), I)) {
    PtrOp Op(I, OperandIndex, CheckTy, Size, nullptr);
    return BoundedPtrCheck(Access, Op, BasePtr, Range);
  }

  Value *Mask = Access.getMask();
  int64_t Offset = 0;
  Value *RangePointer = GetPointerBaseWithConstantOffset(Ptr, Offset, DS.DL);
  int64_t LaneSize = cast<ConstantInt>(Size)->getSExtValue();
  auto Range = std::make_shared<VectorAccessRange>(SE, RangePointer, Offset,
                                                   Mask, LaneSize, LaneSize);
  PtrOp Op(I, OperandIndex, CheckTy, Size, Mask);
  return BoundedPtrCheck(Access, Op, BasePtr, Range);
}

void PTSanOptimizer::eraseCheckAnchor(CheckAnchor Anchor) {
  CallInst *Call = Anchor.Call;

  Anchor.erase();
  PTA.PlaceablePtrChecks.erase(Call);
}

void PTSanOptimizer::hoistLoadBoundAnchors() {
  // Bounds loads at the same site share a placement query. Only bounds
  // anchors move here; real instructions and ID sources stay put.
  DenseMap<std::pair<Instruction *, Value *>, HoistTarget> Targets;
  for (LoadBoundAnchor Bounds : DS.collectAnchors<LoadBoundAnchor>(PTA.F)) {
    // Load-bound IDs remain GetIDAnchors until late optimization.
    GetIDAnchor ID = *DS.getAsAnchor<GetIDAnchor>(Bounds.getID());
    Instruction *Start = DS.getNextRealInstruction(Bounds);
    auto [It, Inserted] = Targets.try_emplace({Start, ID.getBase()});
    if (Inserted) {
      It->second = hoistBoundsLoad(Start, ID.getBase());
    }
    HoistTarget Target = It->second;
    if (Bounds.isVector()) {
      Target = PTU.getFirstCommonInsertionPoint(
          {Target, PTU.getFirstIPAfter(Bounds.getMask())});
    }

    Instruction *IP = Target.getIP();
    if (ID.Call->getParent() == IP->getParent() &&
        (IP == ID.Call || IP->comesBefore(ID.Call))) {
      IP = ID.Call->getNextNode();
    }
    Bounds.Call->moveBefore(IP);
  }
}

void PTSanOptimizer::mergeLoadBoundAnchors() {
  using BoundsKey = std::tuple<Value *, Value *, Instruction *>;
  DenseMap<BoundsKey, CallInst *> BaseSurvivors;
  DenseMap<BoundsKey, CallInst *> SpanSurvivors;

  for (LoadBoundAnchor Bounds : DS.collectAnchors<LoadBoundAnchor>(PTA.F)) {
    Value *Mask = Bounds.getMask();
    BoundsKey Key =
        {Bounds.getID(), Mask, DS.getNextRealInstruction(Bounds)};
    auto &Survivors = Bounds.isBase() ? BaseSurvivors : SpanSurvivors;
    if (!Survivors.contains(Key)) {
      Survivors[Key] = Bounds.Call;
      continue;
    }

    CallInst *Survivor = Survivors.at(Key);
    assert(DT.dominates(Survivor, Bounds.Call) &&
           "merged load-bound anchor must dominate duplicate uses");
    Bounds.Call->replaceAllUsesWith(Survivor);
    Bounds.erase();
  }
}

void PTSanOptimizer::foldStaticBoundAnchors() {
  for (LoadBoundAnchor Bounds : DS.collectAnchors<LoadBoundAnchor>(PTA.F)) {
    // Static folding runs before IDs can be materialized as scalar SSA values.
    GetIDAnchor ID = *DS.getAsAnchor<GetIDAnchor>(Bounds.getID());
    Value *BasePtr = ID.getBase();

    optional<StaticObjectBounds> StaticBounds = getStaticBounds(BasePtr);
    if (!StaticBounds || !StaticBounds->CanFoldRuntimeBounds) {
      continue;
    }

    optional<BoundAnchor> Static;
    if (Bounds.isBase()) {
      TaggedAnchor TaggedBase =
          TaggedAnchor::create(DS, Bounds.Call, StaticBounds->Base);
      Static = StaticBoundAnchor::createBase(DS, Bounds.Call,
                                             TaggedBase.Call);
    } else {
      Static = StaticBoundAnchor::createSpan(DS, Bounds.Call,
                                             StaticBounds->Size->getValue());
    }
    Bounds.Call->replaceAllUsesWith(Static->Call);
    Bounds.erase();
  }
}

void PTSanOptimizer::run() {
  PTSanAnchorOptimizer Anchors(PTA);
  PTSanPreanalysisElider Preanalysis(*this);

  // Strip anchors must be canonical before access analysis snapshots their
  // unstripped uses and SCEVs. Otherwise later strip merging can stale those
  // facts before placement.
  Anchors.hoistStripAnchors();
  Anchors.mergeStripAnchors();

  // Recover exact accessed uses before loop preanalysis inserts synthetic uses
  // that can separate access anchors from their original users.
  SmallVector<BoundedPtrCheck, 16> BoundedAccessChecks;
  for (AccessAnchor Access : DS.collectAnchors<AccessAnchor>(PTA.F)) {
    BoundedAccessChecks.push_back(analyzePtrAccess(Access));
  }

  Preanalysis.materializeChecks();

  // Normalize ID and bounds anchors after loop preanalysis has created any
  // synthetic checks and before placing discovered access checks.
  Anchors.hoistGetIDAnchors();
  Anchors.mergeGetIDAnchors();
  hoistLoadBoundAnchors();
  mergeLoadBoundAnchors();
  foldStaticBoundAnchors();

  // Bound-anchor normalization can move dependencies across preanalysis
  // checks, so reposition those checks afterwards.
  for (auto Anchor : DS.collectAnchors<LoopPreanalysisAnchor>(PTA.F)) {
    PlaceablePtrCheck &Check = PTA.PlaceablePtrChecks.at(Anchor.Call);
    Anchor.Call->moveBefore(Check.getCheckIP(DT));
  }

  // Build and place the remaining live discovered checks in discovery order.
  for (const BoundedPtrCheck &Bounded : BoundedAccessChecks) {
    PlaceablePtrCheck Check = placePtrSink(Bounded);
    CallInst *Call = Bounded.Anchor.Call;
    PTA.PlaceablePtrChecks.insert({Call, Check});
    Call->moveBefore(Check.getCheckIP(DT));
  }

  eraseRemovableChecks();

  Preanalysis.elideCoveredChecks();
  Preanalysis.eraseUnusedChecks();

  applyLoopCheckHoisting();

  bool ChecksMerged;
  do {
    ChecksMerged = mergeSubsumingChecks();
    ChecksMerged |= mergeOrderableChecks();
  } while (ChecksMerged);

  Anchors.eraseUnusedDependencyAnchors();
}

} // namespace llvm::ptsan
