//===-- PTSanOptimization.cpp - PTSan optimization phase -------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"

namespace llvm::ptsan {

class PTSanOptimizer {
private:
  PTSanAnalysisState &PTA;
  PointerTagSanitizer &DS;
  DominatorTree &DT;
  PostDominatorTree &PDT;
  LoopInfo &LI;
  ScalarEvolution &SE;
  PTSanUtils &PTU;

  // Loop -> [complete, solved bases]
  KnownLoopBoundsTy KnownLoopBounds;
  DenseMap<CallInst *, PreanalysisElisionKey> PreanalysisAnchors;

  optional<StaticObjectBounds> getStaticBounds(Value *V);
  HoistTarget hoistBoundsLoad(Instruction *Start, Value *BasePtr);
  SmallPtrSet<Instruction *, 32>
    getValidCheckIPs(HoistTarget MustDominate, Instruction *User,
                     AccessRange &Range);
  optional<PlaceablePtrCheck>
  buildLoopHoistedCheck(const PlaceablePtrCheck &Check);
  void applyLoopCheckHoisting();
  bool analyzeLoopBoundsInfo(const Loop *L, Value *Base,
                             SmallVector<AccessRangePtr> &Ranges);
  void getKnownLoopBounds();
  void hoistLoadBoundAnchors();
  void mergeLoadBoundAnchors();
  void foldStaticBoundAnchors();
  BoundedPtrCheck analyzePtrAccess(AccessAnchor Access);
  void eraseCheckAnchor(CheckAnchor Anchor);
  bool isRemovableCheck(const BoundedPtrCheck &Op);
  bool shouldEraseRemovableCheck(const BoundedPtrCheck &Op);
  void eraseRemovableChecks();
  PreanalysisElisionSet erasePreanalyzedChecks();
  void eraseUnusedPreanalysisChecks(
      const PreanalysisElisionSet &PreanalysisElisions);

public:
  PTSanOptimizer(PTSanAnalysisState &PTA)
      : PTA(PTA), DS(PTA.DS), DT(PTA.DT), PDT(PTA.PDT), LI(PTA.LI),
        SE(PTA.SE), PTU(PTA.PTU) {}

  void run();
  PlaceablePtrCheck placePtrSink(const BoundedPtrCheck &Check);

  void mergeOrderableChecks();
  void mergeSubsumingChecks();
};


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

  if (GV->isConstant() && !ClStrictMode) {
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
  if (auto *LI = dyn_cast<LoadInst>(V)) {
    Value *LoadPtr = LI->getPointerOperand();

    if (auto *Proxy = dyn_cast<GlobalValue>(LoadPtr)) {
      if (!DS.GlobalProxyTargets.contains(Proxy)) {
        return nullopt;
      }

      optional<uint64_t> SizeBytes =
          getStaticGlobalSize(DS.GlobalProxyTargets.at(Proxy), DS.DL);
      if (!SizeBytes) {
        return nullopt;
      }
      auto *Size =
          cast<SCEVConstant>(SE.getConstant(DS.U64Ty, *SizeBytes));
      return StaticObjectBounds{LI, Size};
    }

    return nullopt;
  }

  if (auto *AI = dyn_cast<AllocaInst>(V)) {
    auto AS = AI->getAllocationSize(DS.DL);
    if (!AS || AS->isScalable()) {
      return nullopt;
    }
    auto *Size =
        cast<SCEVConstant>(SE.getConstant(DS.U64Ty, AS->getFixedValue()));
    return StaticObjectBounds{AI, Size};
  }

  auto *GV = dyn_cast<GlobalVariable>(V);
  optional<uint64_t> SizeBytes = getStaticGlobalSize(GV, DS.DL);
  if (SizeBytes && !ClInstrumentGlobals) {
    auto *Base = ConstantExpr::getIntToPtr(
        ConstantInt::get(DS.U64Ty, 0x10000), DS.PtrTy);
    auto *Size = cast<SCEVConstant>(
        SE.getConstant(DS.U64Ty, 0x0000ffffffffffffULL));
    return StaticObjectBounds{Base, Size};
  }

  if (SizeBytes && !DS.InstrVals.contains(GV)) {
    auto *Size = cast<SCEVConstant>(SE.getConstant(DS.U64Ty, *SizeBytes));
    return StaticObjectBounds{GV, Size};
  }

  return nullopt;
}


// Hoist a bounds load. All we want to do is hoist the load to as close
// to BasePtr as possible, while making sure the underlying pointer isn't
// invalidated by an intervening call.
// This is surprisingly complicated:
HoistTarget PTSanOptimizer::hoistBoundsLoad(Instruction *Start,
                                                Value *BasePtr) {
  auto isBlockingCall = [this](Instruction &I) {
    if(this->DS.isInstr(&I)) {
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

    // Don't break on functions which won't free
    // or won't return
    Function *Callee = CB->getCalledFunction();
    if (Callee && (
      Callee->hasFnAttribute(Attribute::NoFree) ||
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
  };

  auto findLastBlockingCall = [&isBlockingCall](BasicBlock::reverse_iterator Begin,
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
  if (PTU.isUnfreeableMemory(BasePtr)) {
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
  for (BasicBlock *BB : depth_first(&PTA.F.getEntryBlock())) {
    for (Instruction &I : *BB) {
      if (isBlockingCall(I) &&
          isPotentiallyReachable(&I, Start, nullptr, &DT, &LI)) {
        PoisonBlocks.insert(I.getParent());
        DBG(errs() << "\tGot call block " << I.getParent()->getName() << "\n");
      }
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

  // Now find each BB which a.) if we hit it, we have hit pointer source/Target
  // (target dominates bb), and b.) if we hit BB, we will hit use/Start
  // (bb post-dominates Start)
  for (BasicBlock *BB : depth_first(&PTA.F.getEntryBlock())) {
    // We special-case these. Could use strict dominance to handle this case,
    // but I think it's clearer to do them explicitly
    if (BB == StartBB || BB == TargetBB) {
      continue;
    }

    // If it's a valid BB, then within the BB, we hoist to after the last call
    // site if any
    // It's legal if:
    // - If we hit BB, we must have hit TargetBB
    // - If we hit StartBB, we must have hit BB.
    if (DT.dominates(TargetBB, BB) && DT.dominates(BB, StartBB)/*PDT.dominates(StartBB, BB)*/) {
      DBG(errs() << "\tFound legal insert block " << BB->getName() << "\n");
      //errs() << "\t\t" << TargetBB->getName() << " dominates BB\n";
      //errs() << "\t\t" << "BB post-dominates " << StartBB->getName() << "\n";
      Instruction *Call = findLastBlockingCall(BB->rbegin(), BB->rend());
      if (Call) {
        // If a block ends in a (potentially freeing) invoke, we can't hoist to it
        if (!isa<InvokeInst>(Call)) {
          InsertPoints.push_back({Call, AFTER});
        }
      } else {
        InsertPoints.push_back({DS.getFirstRealInsertionPt(*BB), BEFORE});
      }
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


bool isStdLibraryGlobal(const GlobalVariable &GV) {
  auto *MD = GV.getMetadata(LLVMContext::MD_dbg);
  if (!MD) return false;

  // errs() << "Checking " << GV << "\n";
  auto DIGE = dyn_cast<DIGlobalVariableExpression>(MD);
  if (!DIGE) {
    errs() << "no DIGE\n";
    return false;
  }

  auto *DIG = dyn_cast<DIGlobalVariable>(DIGE->getVariable());
  if (!DIG) {
    errs() << "not a DIG\n";
    return false;
  }

  const DIScope *Scope = DIG->getScope();
  while (Scope) {
    if (auto *NS = dyn_cast<DINamespace>(Scope)) {
      if (NS->getName() == "std") {
        return true;
      }
    }

    Scope = Scope->getScope();                 // walk up
  }
  return false;                                // reached CU / file without “std”
}

static const GlobalVariable *getNonStrictSkippedGlobal(Value *V) {
  if (ClStrictMode) {
    return nullptr;
  }

  auto *GV = dyn_cast<GlobalVariable>(V->stripPointerCasts());
  if (!GV) {
    return nullptr;
  }

  if (GV->isConstant() || isStdLibraryGlobal(*GV)) {
    return GV;
  }

  return nullptr;
}


SmallPtrSet<Instruction *, 32>
PTSanOptimizer::getValidCheckIPs(HoistTarget MustDominate,
                                      Instruction *Sink, AccessRange &Range) {
  DBG(errs() << "\n----Getting checkIP-----\n");
  SCEVExpander Expander(SE, DS.DL, "valid-check-ips");
  SmallPtrSet<Instruction *, 32> CheckIPs;

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

  SmallPtrSet<Instruction *, 8> Deps = Range.getDeps(&PTU);

  BasicBlock *BB = Sink->getParent();
  auto IPs =  make_range(BB->getFirstInsertionPt(), BB->end());
  for (Instruction &IP : IPs) {
    if (DS.isAnchor(&IP)) {
      continue;
    }

    DBG(errs() << "Considering " << IP << "\n");
    bool ValidIP = true;

    for (Instruction *Dep : Deps) {
      // Deps need to properly dominate, which this does,
      // but BoundsIP only needs to tie
      if (!(DT.dominates(Dep, &IP))) {
        DBG(errs() << "\tdep " << *Dep << "\n\t\tdoes not dominate IP^\n");
        ValidIP = false;
        break;
      }
    }

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
  Value *Ptr = Check.getUnstrippedPtr(DS);
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

    auto AccessExtent = PTU.getLoopAccessExtent(Ptr, Check.Size, L);

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

bool PTSanOptimizer::analyzeLoopBoundsInfo(
    const Loop *L, Value *Base, SmallVector<AccessRangePtr> &Ranges) {
  DBG(errs() << "\nAnalyzing Loop uses of " << *Base << " in " << *L << "\n");

  PTSanIRB IRB(DS, L->getLoopPreheader()->getTerminator());
  Value *False = ConstantInt::getFalse(IRB.getInt1Ty());
  Instruction *FakeUse =
      SelectInst::Create(False, Base, Base, "ptsan.fake.use", L->getLoopPreheader()->getTerminator());
  DBG(errs() << "Target instruction is " << *FakeUse << "\n");
  HoistTarget BoundsTarget = hoistBoundsLoad(FakeUse, Base);

  assert(Base->getType()->isPointerTy());

  SmallVector<CheckAnchor> CreatedPreanalysisAnchors;
  for (auto Range: Ranges) {
    auto CheckIPs = getValidCheckIPs(BoundsTarget, FakeUse, *Range);
    // XXX: Support partial hoists for a given base
    if (CheckIPs.size() == 0) {
      for (CheckAnchor Anchor : CreatedPreanalysisAnchors) {
        eraseCheckAnchor(Anchor);
      }
      return false;
    }

    Instruction *IP = nullptr;
    for (Instruction *NewIP : CheckIPs) {
      if (!IP || DT.dominates(NewIP, IP)) {
        IP = NewIP;
      }
    }
    assert(IP && "preanalysis check must have an insertion point");
    Instruction *BoundsIP = BoundsTarget.getIP();
    GetIDAnchor ID = GetIDAnchor::create(DS, BoundsIP, Base);
    BoundAnchor BaseBound =
        LoadBoundAnchor::createBase(DS, BoundsIP, ID);
    BoundAnchor SpanBound =
        LoadBoundAnchor::createSpan(DS, BoundsIP, ID);
    auto Anchor =
        LoopPreanalysisAnchor::create(DS, IP, ID, BaseBound, SpanBound);
    PtrOp Op(FakeUse, 1, CHK_TY::PREANALYZED_LOOP, nullptr);
    BoundedPtrCheck BoundedCheck(Anchor, Op, Base, Range);
    if (shouldEraseRemovableCheck(BoundedCheck)) {
      eraseCheckAnchor(Anchor);
      continue;
    }

    PlaceablePtrCheck Check(BoundedCheck, CheckIPs, Range);
    assert(!PTA.PlaceablePtrChecks.count(Anchor.Call) &&
           "expected new preanalysis check anchor");
    PTA.PlaceablePtrChecks.insert({Anchor.Call, Check});
    PreanalysisAnchors.insert({Anchor.Call, {L, Base}});
    CreatedPreanalysisAnchors.push_back(Anchor);
  }

  return true;
}


void PTSanOptimizer::getKnownLoopBounds() {
  if (!ClPTSanPreanalyzeLoops || ClNoPTSanOpts) {
    return;
  }

  SerializeBoundsInfo SBI(DS.Ctx);

  for (Loop *L : LI.getLoopsInPreorder()) {
    DBG(errs() << "Getting loop bounds for " << *L << "\n");
    MDNode *LoopInfo = SBI.getPTSanBounds(L->getLoopID());
    if (!LoopInfo) {
      DBG(errs() << "\tNo loop info\n");
      continue;
    }

    DeserializeBoundsInfo DBI(SE, LI, DT, L->getLoopPreheader()->getTerminator(),
                              /*CanSynthesize=*/false);

    auto KnownBaseRanges = DBI.deserializeSolvedRanges(LoopInfo);

    auto [Complete, BoundsInfo] = KnownBaseRanges;
    DBG(errs() << "\tComplete: " << Complete << "\n");

    DenseSet<Value *> SolvedBases;
    for (auto [Base, AccessedRanges] : BoundsInfo) {
      DBG(
        errs() << "\tBase: " << *Base << "\n";
        for (auto Range : AccessedRanges) {
          Range->dump(4);
        });

      if (analyzeLoopBoundsInfo(L, Base, AccessedRanges)) {
        // Keep solved bases even when all generated checks were removable;
        // their presence still lets preanalysis elide covered pointer ops.
        SolvedBases.insert(Base);
      } else {
        Complete = false;
      }
    }

    KnownLoopBounds.insert({L, {Complete, std::move(SolvedBases)}});
  }
}

PreanalysisElisionSet PTSanOptimizer::erasePreanalyzedChecks() {
  auto hasKnownBoundsForBase =
  [](const KnownLoopBoundsTy &KnownLoopBounds, Value *Base, const Loop *L) {
    if (!KnownLoopBounds.contains(L)) {
      DBG(errs() << "Do not have loop bounds info for " << *L << "\n");
      return false;
    }

    const auto &[Complete, SolvedBases] = KnownLoopBounds.at(L);

    // If a loop is complete, we know all accesses are accounted for, and
    // we don't have to check this, *even if* we can no longer trace the
    // base to any solved bases.
    if (Complete) {
      DBG(errs() << "Loop is complete. Skipping LoopOp\n");
      return true;
    }

    if (SolvedBases.contains(Base)) {
      DBG(errs() << "Incomplete loop, but solved base pointer!\n");
      return true;
    }

    DBG(errs() << "Loop does not have base info for " << *Base << "\n");
    return false;
  };

  DBG(errs() << "----------------------Filtering preanalysis checks-----------------\n");
  PreanalysisElisionSet Out;

  if (!ClPTSanPreanalyzeLoops || ClNoPTSanOpts) {
    DBG(errs() << "Skipping preanalysis filtering\n");
    return Out;
  }

  uint64_t skippedLoopOps = 0;
  uint64_t totalLoopOps = 0;
  for (AccessAnchor Access : DS.collectAnchors<AccessAnchor>(PTA.F)) {
    CallInst *Call = Access.Call;
    assert(PTA.PlaceablePtrChecks.count(Call) &&
           "expected placed access anchor to have lowering facts");
    const PlaceablePtrCheck &Check = PTA.PlaceablePtrChecks.at(Call);

    Instruction *I = cast<Instruction>(Check.U->getUser());

    Loop *L = LI.getLoopFor(I->getParent());
    if (!L) {
      continue;
    }

    totalLoopOps++;
    DBG(errs() << "Considering skipping loop use " << *I << "\n");
    Value *Base = Check.BasePtr;

    // If any enclosing loop for U is solved for U, we don't need to
    // check U. Track which preanalysis checks superseded non-removable
    // bounded ops so we only emit preanalysis checks that remove something.
    // Prefer the outermost solved loop so nested loops do not both keep a
    // check for the same bounded op.
    const Loop *ElidingLoop = nullptr;
    for (; L != nullptr; L = L->getParentLoop()) {
      if (hasKnownBoundsForBase(KnownLoopBounds, Base, L)) {
        ElidingLoop = L;
      }
    }

    if (ElidingLoop) {
      Out.insert({ElidingLoop, Base});

      DBG(errs() << "Eliding loop op\n");
      skippedLoopOps++;
      eraseCheckAnchor(Access);
    }
  }

  DBG(errs() << "Done with preanalysis filtering. Skipped " << skippedLoopOps << "/"
         << totalLoopOps << " loop ops\n");
  return Out;
}

void PTSanOptimizer::eraseUnusedPreanalysisChecks(
    const PreanalysisElisionSet &PreanalysisElisions) {
  for (auto LoopAnchor : DS.collectAnchors<LoopPreanalysisAnchor>(PTA.F)) {
    auto Key = PreanalysisAnchors.at(LoopAnchor.Call);
    if (PreanalysisElisions.contains(Key)) {
      continue;
    }

    DBG(errs() << "Skipping unused preanalysis check for loop/base "
               << *Key.second << "\n");
    eraseCheckAnchor(LoopAnchor);
  }
}


bool PTSanOptimizer::isRemovableCheck(const BoundedPtrCheck &Op) {
  assert(Op.U && Op.BasePtr && Op.Range);
  Value *Ptr = Op.getUnstrippedPtr(DS);

  // Keep this separate from the SCEV fixed-object proof below. LLVM's
  // dereferenceability/alignment facts can prove some accesses that our
  // object-range comparison cannot.
  if (PTU.isKnownSpatiallyValid(Op.U) && PTU.isUnfreeableMemory(Op.BasePtr)) {
    DBG(errs() << "\tSkipping check for inbounds, nonfreeable "
               << *Op.U->getUser() << "\n");
    return true;
  }

  if (Op.Range->isSizeZero()) {
    DBG(errs() << "\tSkipping check for size-0 access " << *Ptr << "\n");
    return true;
  }

  if (const GlobalVariable *GV = getNonStrictSkippedGlobal(Op.BasePtr)) {
    if (!DS.InstrVals.contains(GV)) {
      DBG(errs() << "\tSkipping non-strict unbounded global access "
                 << *Op.U->getUser() << "\n");
      return true;
    }
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

void PTSanOptimizer::mergeSubsumingChecks() {
  if (isCheckMergeDisabled()) {
    return;
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
  // Pending (established) check -> Dominating instruction
  DenseMap<CallInst *, Instruction *> DominatingInstrs;

  for (CheckAnchor NewAnchor : Checks) {
    PlaceablePtrCheck &NewCheck = PTA.PlaceablePtrChecks.at(NewAnchor.Call);
    bool Merged = false;

    assert(NewCheck.UsesCovering.size() == 1);

    Use *U = *NewCheck.UsesCovering.begin();
    Instruction *I = cast<Instruction>(U->getUser());

    DBG(errs() << "\n\nConsidering Check for " << *I << "\n");

    Value *NewBaseBound = NewCheck.Anchor.getBaseBound().Call;
    Value *NewSpanBound = NewCheck.Anchor.getSpanBound().Call;
    auto Key = std::make_pair(NewBaseBound, NewSpanBound);
    HoistTarget MustDominate = NewCheck.Anchor.getBoundsTarget(PTU);

    if (M.contains(Key)) {
      auto &EstablishedChecks = M.at(Key);
      for (CheckAnchor EAnchor : EstablishedChecks) {
        PlaceablePtrCheck &ECheck = PTA.PlaceablePtrChecks.at(EAnchor.Call);
        assert(NewCheck.BasePtr == ECheck.BasePtr);
        assert(NewBaseBound == ECheck.Anchor.getBaseBound().Call);
        assert(NewSpanBound == ECheck.Anchor.getSpanBound().Call);

        Instruction *DomInst = DominatingInstrs.at(EAnchor.Call);

        DBG(errs() << "\tComparing for sub/super against " << *DomInst << "\n");

        RangeCmpResult Cmp = NewCheck.Range->cmp(&PTU, ECheck.Range.get());

        bool SubsetOrEqual = false;
        bool SupersetOrEqual = false;
        if (Cmp == RCR_EQUAL) {
          SubsetOrEqual = true;
          SupersetOrEqual = true;
          DBG(errs() << "\tGot equal accesses\n");
        } else if (Cmp == RCR_SUBSET) {
          SubsetOrEqual = true;
          DBG(errs() << "\t" << *I << " is subset of \n\t" << *DomInst << "\n");
        } else if (Cmp == RCR_SUPERSET) {
          SupersetOrEqual = true;
          DBG(errs() << "\t" << *I << " is superset of \n\t" << *DomInst << "\n");
        }

        if (!(SupersetOrEqual || SubsetOrEqual)) {
          DBG(errs() << "\tNo sub/super relationship\n");
          continue;
        }

        // Theoretically we could do a more complicated set-dominance/set-post-
        // dominance analysis, but I don't want to right now.
        // XXX: this ^

        // At this point, SubsetOrEqual means that the new check (I) is a subset of the
        // established check (DomInst), same with super.

        // PDT.dominates(A, B) means if we hit B, we will hit A as well

        // If the established check dominates this one, we can subsume the new
        // check into the established one if appropriate:
        if (DT.dominates(DomInst, I)) {
          if (SubsetOrEqual || (SupersetOrEqual && PDT.dominates(I, DomInst))) {
            auto NewRange = ECheck.Range->clone();
            bool Changed = NewRange->mergeIn(&PTU, NewCheck.Range.get());
            if (Changed) {
              auto NewIPs =
                  getValidCheckIPs(MustDominate, DomInst, *NewRange);
              if (NewIPs.size() == 0) {
                DBG(errs() << "\tCannot find IP for combined check\n");
                continue;
              }
              ECheck.Range = NewRange;
              ECheck.CheckIPs = NewIPs;
            }

            Merged = true;
            ECheck.CheckTy = COMPOUND;
            ECheck.UsesCovering.insert(NewCheck.UsesCovering.begin(), NewCheck.UsesCovering.end());
            EAnchor.Call->moveBefore(ECheck.getCheckIP(DT));
            eraseCheckAnchor(NewAnchor);
            DBG(errs() << "\tOld dominates new and subset/super works\n");
            break;
          }
          DBG(errs() << "\tOld dominates new but sub/super doesn't work\n");
        }

        // If newCheck dominates the established check, we can subsume the established
        // check into the new check if appropriate:
        if (DT.dominates(I, DomInst)) {
          if (SupersetOrEqual || (SubsetOrEqual && PDT.dominates(DomInst, I))) {
            auto NewRange = ECheck.Range->clone();
            bool Changed = NewRange->mergeIn(&PTU, NewCheck.Range.get());
            if (Changed) {
              auto NewIPs =
                  getValidCheckIPs(MustDominate, I, *NewRange);
              if (NewIPs.size() == 0) {
                DBG(errs() << "\tCannot find IP for combined check\n");
                continue;
              }
              ECheck.Range = NewRange;
              ECheck.CheckIPs = NewIPs;
            } else {
              // If the range hasn't changed, copy over NewCheck's IPs
              ECheck.CheckIPs = NewCheck.CheckIPs;
            }

            Merged = true;
            ECheck.CheckTy = COMPOUND;
            DominatingInstrs[EAnchor.Call] = I;
            ECheck.InstigatingUses = NewCheck.InstigatingUses;
            ECheck.UsesCovering.insert(NewCheck.UsesCovering.begin(), NewCheck.UsesCovering.end());
            EAnchor.Call->moveBefore(ECheck.getCheckIP(DT));
            eraseCheckAnchor(NewAnchor);
            DBG(errs() << "\tNew dominates old and sub/super works\n");
            break;
          }
          DBG(errs() << "\tNew dominates old, but sub/super doesn't work\n");
        }

        DBG(errs() << "No dominance relationship\n");
      }
    }

    if (!Merged) {
      M[Key].push_back(NewAnchor);
      DominatingInstrs[NewAnchor.Call] = I;
    }
  }
}

void PTSanOptimizer::mergeOrderableChecks() {
  if (isCheckMergeDisabled()) {
    return;
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
        break;
      }
    }

    if (!Merged) {
      M[Key].push_back(NewAnchor);
    }
  }

  DBG(errs() << "\n\n----------Check merging done-----------\n\n");
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
  PtrOp Op(I, OperandIndex, CheckTy, Size);

  Value *Ptr = BoundedPtrCheck::getUnstrippedPtrFromUse(U, DS);
  Value *BasePtr = findPTSanBasePointer(Ptr, PTU);

  const SCEV *Min = SE.getSCEV(Ptr);
  if (!PTU.depsDominateLoc(Min, I)) {
    Min = SE.getUnknown(Ptr);
  }

  const SCEV *SCEVSize = SE.getSCEV(Size);
  AccessRangePtr Range =
      std::make_shared<SimpleAccessRange>(SE, Min, SCEVSize);
  return BoundedPtrCheck(Access, Op, BasePtr, Range);
}

void PTSanOptimizer::eraseCheckAnchor(CheckAnchor Anchor) {
  CallInst *Call = Anchor.Call;

  Anchor.erase();
  PTA.PlaceablePtrChecks.erase(Call);
  PreanalysisAnchors.erase(Call);
}

void PTSanOptimizer::hoistLoadBoundAnchors() {
  for (LoadBoundAnchor Bounds : DS.collectAnchors<LoadBoundAnchor>(PTA.F)) {
    // Load-bound IDs remain GetIDAnchors until late optimization.
    GetIDAnchor ID = *DS.getAsAnchor<GetIDAnchor>(Bounds.getID());
    Instruction *Start = DS.getNextRealInstruction(Bounds);
    Instruction *IP = hoistBoundsLoad(Start, ID.getBase()).getIP();
    if (ID.Call->getParent() == IP->getParent() &&
        (IP == ID.Call || IP->comesBefore(ID.Call))) {
      IP = ID.Call->getNextNode();
    }
    Bounds.Call->moveBefore(IP);
  }
}

void PTSanOptimizer::mergeLoadBoundAnchors() {
  DenseMap<std::pair<Value *, Instruction *>, CallInst *> BaseSurvivors;
  DenseMap<std::pair<Value *, Instruction *>, CallInst *> SpanSurvivors;

  for (LoadBoundAnchor Bounds : DS.collectAnchors<LoadBoundAnchor>(PTA.F)) {
    auto Key =
        std::make_pair(Bounds.getID(), DS.getNextRealInstruction(Bounds));
    auto &Survivors = Bounds.isBase() ? BaseSurvivors : SpanSurvivors;
    auto [It, Inserted] = Survivors.insert({Key, Bounds.Call});
    if (Inserted) {
      continue;
    }

    CallInst *Survivor = It->second;
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
    if (!StaticBounds) {
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

  getKnownLoopBounds();

  // Normalize ID and bounds anchors after loop preanalysis has created any
  // synthetic checks and before placing discovered access checks.
  Anchors.hoistGetIDAnchors();
  Anchors.mergeGetIDAnchors();
  hoistLoadBoundAnchors();
  mergeLoadBoundAnchors();
  foldStaticBoundAnchors();

  // Place the remaining live discovered checks.
  for (const BoundedPtrCheck &Bounded : BoundedAccessChecks) {
    PlaceablePtrCheck Check = placePtrSink(Bounded);
    CallInst *Call = Bounded.Anchor.Call;
    PTA.PlaceablePtrChecks.insert({Call, Check});
    Call->moveBefore(Check.getCheckIP(DT));
  }

  eraseRemovableChecks();

  auto PreanalysisElisions = erasePreanalyzedChecks();
  eraseUnusedPreanalysisChecks(PreanalysisElisions);

  applyLoopCheckHoisting();

  mergeSubsumingChecks();
  mergeOrderableChecks();

  Anchors.eraseUnusedDependencyAnchors();
}

} // namespace llvm::ptsan
