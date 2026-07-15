//===-- PTSanLowering.cpp - PTSan lowering phase -------------*- C++ -*-===//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"

namespace llvm::ptsan {

  class PTSanLowerer {
private:
  Function &F;
  PointerTagSanitizer &DS;
  PTSanAnalysisState &PTA;

  uint32_t NextCheckID;

  // How many allocas are in this function
  size_t NumLocals;
  // Did we use a special-case function?
  // 1-4 are special-cases, 0 is n
  uint16_t StackIDCase;

  // These values are actually added to the function
  // and must be referenced across calls

  // Alloca for storing ids in the non-special case
  AllocaInst *IDsAlloc;
  // Return value (single u64) used in the special case
  Instruction *IDsRet;

  Value *HasStackIDRoom;
  Value *StackIDLimit;
  Value *StartingStackID;

  ConstantInt *FunctionID;

  // Lowering entry points
  void lowerPtrSources(const SmallVectorImpl<SourceAnchor> &PtrSrcs);
  Value *lowerGetID(GetIDAnchor IDAnchor);
  Value *lowerBound(BoundAnchor Bounds);
  Value *lowerStrip(StripAnchor Strip);
  Value *lowerTagged(TaggedAnchor Tagged);
  Value *lowerTag(TagAnchor Tag);
  optional<RangeCheckInfo> addPtrCheck(const PlaceablePtrCheck &Op);
  bool getsTagged(Use &U);
  void lowerFunctionExit(Instruction *I);
  void lowerArithmeticValidation(ValidateArithAnchor Op);
  void lowerSpecialStripCall(CallBase *CB);
  void actuallyAddCheck(const RangeCheckInfo &CI);
  void lowerLibcCall(Instruction *I);

public:
  PTSanLowerer(Function &F, PointerTagSanitizer &DS,
                    PTSanAnalysisState &PTA);
  void run();
  void redirectGlobalUsesForFunction();
  // Utils
  void markIRAsFrom(IRBuilderBase &IRB, StringRef Name);
};

static uint64_t getFunctionGUID(const llvm::Function &F) {
  using namespace llvm;

  std::string Accum;
  raw_string_ostream OS(Accum);

  // 1. Module path (guarantees uniqueness across DSOs)
  OS << F.getParent()->getModuleIdentifier() << '\0';

  // 2. Function name
  OS << F.getName() << '\0';

  // 3. Function type (parameter + result types, vararg flag)
  F.getFunctionType()->print(OS);
  OS << '\0';

  OS.flush();
  return xxHash64(Accum);
}

PTSanLowerer::PTSanLowerer(Function &F, PointerTagSanitizer &DS,
                                     PTSanAnalysisState &PTA)
    : F(F), DS(DS), PTA(PTA), NextCheckID(0) {
  FunctionID = ConstantInt::get(DS.U64Ty, getFunctionGUID(F));
}

bool PTSanLoweringPass::run(PTSanAnalysisState &PTA) {
  Function &F = PTA.F;
  PointerTagSanitizer &DS = PTA.DS;

  if (ClValidate) {
    PTSanIRB IRB(DS, &*F.getEntryBlock().getFirstInsertionPt());
    IRB.CreateCall(DS.ValidateFunctionStart);
  }

  DBG(errs() << "\n---------Now lowering PTSan ops----------------\n\n");

  PTSanLowerer Lowerer(F, DS, PTA);
  Lowerer.run();

  Lowerer.redirectGlobalUsesForFunction();

  if (verifyFunction(F, &errs())) {
    errs() << "\n-----------------------------------------------\n";
    errs() << "Found broken function:\n\n" << F << "\n\n";
    report_fatal_error("Function verification failed!");
  }

  return true;
}

void PTSanLowerer::lowerFunctionExit(Instruction *I) {
  if (NumLocals == 0 || ClNoIDs) {
    return;
  }

  PTSanIRB IRB(DS, I);

  ConstantInt *UsedIDs = IRB.getInt64(NumLocals);

  if (isStackIDFastPathDisabled()) {
    assert(StackIDCase == 0);
    assert(IDsAlloc != nullptr);
    assert(IDsRet == nullptr);
    IRB.CreateCall(DS.FreeStackIDsN, {IDsAlloc, UsedIDs});
    return;
  }

  MDBuilder MDB(DS.Ctx);
  MDNode *BW = MDB.createBranchWeights(/*true*/1000, /*false*/1);
  Instruction *FastPath = nullptr, *SlowPath = nullptr;
  SplitBlockAndInsertIfThenElse(HasStackIDRoom, IRB.GetInsertPoint(),
                                &FastPath, &SlowPath,
                                BW, &PTA.DTU, &PTA.LI);
  // In fast path, choose between delaying stack ID reuse for temporal safety or
  // immediately recycling IDs for speed.
  IRB.SetInsertPoint(FastPath);
  if (ClStackTemporalSafety) {
    assert(DS.PendingStackIdsPtr &&
           "pending stack IDs only exist with stack temporal safety");
    Value *PendingIDs16 =
      IRB.CreateLoad(DS.U16Ty, DS.PendingStackIdsPtr, "pending.frees");
    ConstantInt *Enough = IRB.getInt16(64);
    Value *PendingIdsInc16 = IRB.CreateAdd(PendingIDs16, IRB.getInt16(NumLocals), "pending.frees.inc");
    Value *EnoughPendingIDs = IRB.CreateICmpUGE(PendingIdsInc16, Enough, "enough.pending.ids");
    // We usually will not have a big enough chunk
    BW = MDB.createBranchWeights(/*true*/1, /*false*/128);
    Instruction *AfterSplit = &*IRB.GetInsertPoint();
    Instruction *EnoughPendingIDsPath, *NotEnoughPendingIDsPath;
    SplitBlockAndInsertIfThenElse(EnoughPendingIDs, AfterSplit,
                                  &EnoughPendingIDsPath, &NotEnoughPendingIDsPath,
                                  nullptr, &PTA.DTU, &PTA.LI);

    // If we've got a big enough chunk, free ids:
    IRB.SetInsertPoint(EnoughPendingIDsPath);
    IRB.CreateStore(StartingStackID, DS.NextStackIDPtr);
    IRB.CreateStore(IRB.getInt16(0), DS.PendingStackIdsPtr);

    // If we don't have a big enough chunk, just add to pending
    IRB.SetInsertPoint(NotEnoughPendingIDsPath);
    IRB.CreateStore(PendingIdsInc16, DS.PendingStackIdsPtr);

    // Regardless (in the fast path), free ids
    IRB.SetInsertPoint(AfterSplit);

    // First ID in the contiguous run is StartingStackID.
    Value *ID0_64 = IRB.CreateZExt(StartingStackID, DS.U64Ty);

    // &BoundsArray[0][ID0]
    Value *FirstSlot = IRB.CreateInBoundsGEP(
        DS.BoundsArray->getValueType(),
        DS.BoundsArray,
        {IRB.getInt64(0), ID0_64});

    // Total bytes = NumLocals * sizeof(BoundsStructTy)
    uint64_t ElemSz = DS.DL.getTypeStoreSize(DS.BoundsStructTy);
    Value *TotalSz = IRB.getInt64(ElemSz * (uint64_t)NumLocals);

    Align A = DS.DL.getABITypeAlign(DS.BoundsStructTy);

    IRB.CreateMemSet(
        FirstSlot,
        IRB.getInt8(0),
        TotalSz,
        A);
  } else {
    IRB.CreateStore(StartingStackID, DS.NextStackIDPtr);
  }

  // Else: Call function:
  IRB.SetInsertPoint(SlowPath);

  // Case 0 means yes IdsAlloc, no IDsRet, while Case > 0 means inverse
  assert((StackIDCase == 0) == (IDsAlloc != nullptr));
  assert((StackIDCase == 0) == (IDsRet == nullptr));
  switch (StackIDCase) {
    case 0:
      IRB.CreateCall(DS.FreeStackIDsN, {IDsAlloc, UsedIDs});
      break;
    case 1:
      IRB.CreateCall(DS.FreeStackIDs1, {IDsRet});
      break;
    case 2:
      IRB.CreateCall(DS.FreeStackIDs2, {IDsRet});
      break;
    case 3:
      IRB.CreateCall(DS.FreeStackIDs3, {IDsRet});
      break;
    case 4:
      IRB.CreateCall(DS.FreeStackIDs4, {IDsRet});
      break;
  }
}

static void hoistAllocas(Function &F) {
  BasicBlock &Entry = F.getEntryBlock();

  Instruction *InsertBefore = &*Entry.begin();
  // This is the entry point. There shouldn't be a phi here.
  assert(!isa<PHINode>(InsertBefore));

  SmallVector<AllocaInst *, 16> ToMove;

  for (BasicBlock &BB : F) {
    for (Instruction &I : BB) {
      auto *AI = dyn_cast<AllocaInst>(&I);
      if (!AI) continue;

      // Only hoist constant-size allocas in addrspace(0)
      if (!isa<ConstantInt>(AI->getArraySize())) continue;
      if (AI->getAddressSpace() != 0) continue;
      // Don't hoist the first instruciton or ??? will happen
      if (AI == InsertBefore) continue;

      ToMove.push_back(AI);
    }
  }

  for (AllocaInst *AI : ToMove) {
    AI->moveBefore(InsertBefore);
  }
}

static bool areUsesLocal(Value *Start) {
  SmallVector<Value *, 8> Worklist;
  SmallPtrSet<Value *, 8> Visited;
  Worklist.push_back(Start);
  DBG(errs() << "Checking local uses for  " << *Start << "\n");
  while(Worklist.size() > 0) {
    Value *V = Worklist.pop_back_val();
    // If we hit something we're looping on, if it leads to a leak
    // we'll find it later
    if (Visited.contains(V)) {
      continue;
    }

    Visited.insert(V);
    for (auto &Use : V->uses()) {
      Value *U = Use.getUser();
      DBG(errs() << "\tVisiting " << *U << "\n");
      if (isa<LoadInst>(U) || isa<BranchInst>(U)) {
        continue;
      }

      if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
        // storing this into something else -- where we
        // stored it becomes the leak
        if (V == SI->getOperand(0)) {
          DBG(errs() << "Leak node " << *U << "\n");
          return false;
        }

        // Otherwise it's fine:
        continue;
      }

      if (IntrinsicInst *I = dyn_cast<IntrinsicInst>(U)) {
        // Memory Intrinsics are all ends
        if (isa<AnyMemIntrinsic>(I)) {
          continue;
        }

        switch (I->getIntrinsicID()) {
          // ---- Pointer-preserving (propagate) --------------------------------
          case Intrinsic::ptrmask:
          case Intrinsic::launder_invariant_group:
          case Intrinsic::strip_invariant_group:
          case Intrinsic::ptr_annotation:
          case Intrinsic::coro_promise:
            DBG(errs() << "Recursing through ptr-preserving " << *I << "\n");
            Worklist.push_back(I);
            continue;

          // ---- Masked/Vector memory (store-like → possible leak) -------------
          case Intrinsic::masked_store:
            if (V == I->getArgOperand(0)) {
              DBG(errs() << "Leak node (masked.store of V) " << *I << "\n");
              return false;
            }
            continue;

          // masked.scatter: arg0 = values, arg1 = ptrs
          case Intrinsic::masked_scatter:
            if (V == I->getArgOperand(0)) {
              DBG(errs() << "Leak node (masked.scatter of V) " << *I << "\n");
              return false;
            }
            continue;


          // ---- Masked/Vector memory (load-like → end nodes) ------------------
          case Intrinsic::masked_load:
          case Intrinsic::masked_gather:
          // ---- Lifetime, invariant, assume, hints, debug, metadata -----------
          case Intrinsic::lifetime_start:
          case Intrinsic::lifetime_end:
          case Intrinsic::invariant_start:
          case Intrinsic::invariant_end:
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
          case Intrinsic::instrprof_cover:
          case Intrinsic::instrprof_increment:
          case Intrinsic::instrprof_value_profile:
          // ---- Coroutine bits that don't propagate pointers ------------------
          case Intrinsic::coro_begin:
          case Intrinsic::coro_end:
          case Intrinsic::coro_free:
          case Intrinsic::coro_destroy:
          case Intrinsic::coro_resume:
          case Intrinsic::coro_suspend:
          case Intrinsic::coro_id:
          case Intrinsic::coro_size:
          case Intrinsic::coro_save:
          case Intrinsic::coro_done:
          case Intrinsic::coro_noop:
          case Intrinsic::coro_async_resume:
          // ---- EH & platform/runtime-ish intrinsics (end nodes) --------------
          case Intrinsic::eh_typeid_for:
          case Intrinsic::eh_sjlj_setjmp:
          case Intrinsic::eh_sjlj_longjmp:
          case Intrinsic::eh_dwarf_cfa:
          case Intrinsic::eh_recoverfp:
            continue;

          // ---- Default handling ----------------------------------------------
          default: {
            // Pure intrinsics with no writes -> end nodes.
            if (I->doesNotAccessMemory() || I->onlyReadsMemory()) {
              continue;
            }

            // Heuristic 2: If an arg is explicitly write-only (dest-like),
            // and V is used in that write-only position, then we're writing *into* V.
            optional<bool> Read = nullopt;
            for (unsigned op = 0; op < I->getNumOperands(); op++) {
              if (I->getArgOperand(op) == V) {
                if (!(I->paramHasAttr(op, Attribute::WriteOnly))) {
                  Read = true;
                  break;
                }
              }
            }
            assert(Read != nullopt);
            if (*Read == false) {
              continue;
            }

            errs() << "FIXME: Unhandled intrinsic: " << *I << "\n";
            return false;
          }
        }
      }

      if (isa<CallInst>(U) || isa<ReturnInst>(U) || isa<InvokeInst>(U)) {
        DBG(errs() << "Leak node " << *U << "\n");
        return false;
      }

      if (isa<PHINode>(U) ||
          isa<BinaryOperator>(U) ||
          isa<UnaryOperator>(U) ||
          isa<CmpInst>(U) ||
          isa<CastInst>(U) ||
          isa<GetElementPtrInst>(U) ||
          isa<SelectInst>(U) ||
          isa<AtomicRMWInst>(U) ||
          isa<AtomicCmpXchgInst>(U) ||
          isa<InsertElementInst>(U)) {
          DBG(errs() << "Recursing on " << *U << "\n");
        Worklist.push_back(U);
        continue;
      }

      errs() << "Unknown (assumed to be leak) node " << *U << "\n";
      die();
    }
  }

  return true;
}

bool PTSanLowerer::getsTagged(Use &U) {
  User *User = U.getUser();
  assert(DS.isInstr(User) && "expected PTSan-created use");

  if (DS.getAsAnchor<TaggedAnchor>(User))
    return true;

  if (DS.getAsAnchor<GetIDAnchor>(User))
    return true;

  if (DS.getAsAnchor<SourceAnchor>(User) ||
      DS.getAsAnchor<CheckAnchor>(User) ||
      DS.getAsAnchor<BoundAnchor>(User) ||
      DS.getAsAnchor<StripAnchor>(User) ||
      DS.getAsAnchor<TagAnchor>(User) ||
      DS.getAsAnchor<ValidateArithAnchor>(User) ||
      DS.getAsAnchor<FunctionExitAnchor>(User) ||
      DS.getAsAnchor<LibcCallAnchor>(User) ||
      DS.getAsAnchor<SpecialStripAnchor>(User)) {
    return false;
  }

  if (DS.isAnchor(static_cast<Value *>(User))) {
    errs() << "Unhandled PTSan anchor tagged-use disposition: "
           << *User << "\n";
    die();
  }

  return false;
}

void PTSanLowerer::lowerPtrSources(
    const SmallVectorImpl<SourceAnchor> &PtrSrcs) {
  assert(NumLocals == PtrSrcs.size());
  if (NumLocals == 0 || ClNoIDs) {
    return;
  }
  // The stack-ID fast path intentionally uses a u16 add/compare sequence:
  //
  //   requested = ptsan_next_stack_id + NumLocals
  //   requested <= ptsan_stack_id_limit
  //
  // Runtime bucket configuration keeps stack_id_limit in the low half of the
  // u16 space. This compile-time guard handles pathological single functions
  // without changing that hot path or adding a widened runtime check.
  if (NumLocals > 0x7fff) {
    report_fatal_error(
        "PTSan cannot assign more than 0x7fff stack IDs to one function");
  }

  PTSanIRB IRB(DS, &*F.getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
  markIRAsFrom(IRB, "ptsan.ir.lowerPtrSources");
  SCEVExpander Expander(PTA.SE, DS.DL, "ptr.sources");

  auto GetSource = [&](unsigned i) {
    return PtrSrcs[i].getSource();
  };

  auto GetSourceSize = [&](unsigned i) -> const SCEV * {
    Value *Source = GetSource(i);
    if (auto *AI = dyn_cast<AllocaInst>(Source)) {
      auto Size = AI->getAllocationSize(DS.DL);
      assert(Size && !Size->isScalable() &&
             "PTSan source alloca should have a fixed size");
      assert(Size->getFixedValue() < 0xFFFFFFFFULL);
      return PTA.SE.getConstant(DS.U32Ty, Size->getFixedValue());
    }

    if (F.getName() == "main" && F.arg_size() >= 2 &&
        Source == F.getArg(1)) {
      Value *ArgC = F.getArg(0);
      const SCEV *One = PTA.SE.getConstant(DS.U32Ty, 1);
      const SCEV *PtrSz =
          PTA.SE.getConstant(DS.U32Ty, DS.DL.getPointerSize());
      return PTA.SE.getMulExpr(
          PTA.SE.getAddExpr(PTA.SE.getUnknown(ArgC), One), PtrSz);
    }

    errs() << "Unknown PTSan source anchor operand: " << *Source << "\n";
    die();
  };

  auto GetFastCaseIDs = [&](PTSanIRB &IRB, Value *RequestedStackID) {
    SmallVector<Value *, 8> IDs;
    IDs.resize(NumLocals);

    for (unsigned i = 0; i < NumLocals; i++) {
      Value *ID = (i == 0)
          ? StartingStackID
          : IRB.CreateAdd(StartingStackID, IRB.getInt16(i), "next.plus");
      IDs[i] = ID;
    }

    //ptsan_next_stack_id += N;
    IRB.CreateStore(RequestedStackID, DS.NextStackIDPtr);

    return IDs;
  };

  auto WriteBounds = [&](PTSanIRB &IRB, const SmallVector<Value *, 8> &IDs) {
    for (size_t i = 0; i < PtrSrcs.size(); i++) {
      Value *Source = GetSource(i);

      // Keep the tagged base in sync with static-bounds lowering.
      Value *Base  = IRB.CreatePtrToInt(Source, DS.U64Ty);

      Expander.setInsertPoint(IRB.GetInsertPoint());
      Value *Span32 = Expander.expandCodeFor(GetSourceSize(i), DS.U32Ty);
      Value *Span64 = IRB.CreateZExt(Span32, DS.U64Ty, "span.u64");

      Value *ID_64 = IRB.CreateZExt(IDs[i], DS.U64Ty);

      Value *Shifted = IRB.CreateShl(ID_64, 48);
      Value *TaggedAddr = IRB.CreateOr(Base, Shifted);

      Value *Agg   = PoisonValue::get(DS.BoundsStructTy);
      Agg          = IRB.CreateInsertValue(Agg, TaggedAddr, {0});
      Agg          = IRB.CreateInsertValue(Agg, Span64, {1});

      Value *Slot = IRB.CreateInBoundsGEP(
                            DS.BoundsArray->getValueType(),
                            DS.BoundsArray,
                            {IRB.getInt64(0), ID_64});

      Value *IsDefaultID = IRB.CreateICmpEQ(IDs[i], DS.DefaultIDNo);

      Value *WriteTo = IRB.CreateSelect(IsDefaultID, DS.OverflowIDBoundsPtr, Slot);
      IRB.CreateStore(Agg, WriteTo);
    }
  };

  auto TagPtrs = [&](PTSanIRB &IRB, const SmallVector<Value *, 8> &IDs) {
    for (size_t i = 0; i < PtrSrcs.size(); i++) {
      Value *V = GetSource(i);

      // I'm assuming the optimizer will remove any duplicates of these
      Value *Base  = IRB.CreatePtrToInt(V, DS.U64Ty);
      Value *ID_64 = IRB.CreateZExt(IDs[i], DS.U64Ty);

      Value *Shifted = IRB.CreateShl(ID_64, 48);
      Value *Tagged  = IRB.CreateIntToPtr(
                        IRB.CreateOr(Base, Shifted), V->getType());
      SmallVector<Use *, 8> InstrUses;
      for (Use &U : V->uses()) {
        if (DS.isInstr(U.getUser()) && !getsTagged(U)) {
          InstrUses.push_back(&U);
        }
      }

      V->replaceNonMetadataUsesWith(Tagged);

      for (Use *U : InstrUses) {
        U->set(V);
      }
    }
  };

  auto ValidatePtrSrcs = [&](PTSanIRB &IRB, const SmallVector<Value *, 8> &IDs) {

    Value *FnNameCStr = IRB.CreateGlobalStringPtr(F.getName(), F.getName());

    for (size_t i = 0; i < PtrSrcs.size(); i++) {
      Value *Source = GetSource(i);
      bool IsLocal = areUsesLocal(Source);
      ConstantInt *IsLocalVal = ConstantInt::get(DS.BoolTy, IsLocal);
      IRB.CreateCall(DS.LogAllocaInfo, {FnNameCStr, IDs[i], IsLocalVal, HasStackIDRoom});
    }
  };

  // Used for special-case calls
  auto GetBase = [&](unsigned i) { return GetSource(i); };

  auto GetSpanU32 = [&](unsigned i) {
    Expander.setInsertPoint(IRB.GetInsertPoint());
    return Expander.expandCodeFor(GetSourceSize(i), DS.U32Ty);
  };

  auto GetSpanU64 = [&](unsigned i) {
    return IRB.CreateZExt(GetSpanU32(i), DS.U64Ty, "span.u64");
  };


  auto MakeTwoSpans = [&](unsigned i0, unsigned i1) {
    Value *A64 = GetSpanU64(i0);
    Value *B64 = GetSpanU64(i1);
    Value *Sp  = IRB.CreateOr(A64, IRB.CreateShl(B64, IRB.getInt64(32)));
    return Sp;
  };

  auto UseStackIDSlowPathOnly = [&]() {
    StackIDCase = 0;
    IDsRet = nullptr;

    hoistAllocas(F);

    ConstantInt *UsedIDs = IRB.getInt64(NumLocals);
    IDsAlloc = IRB.CreateAlloca(DS.U16Ty, UsedIDs, "stack.ids");
    HasStackIDRoom = ConstantInt::getFalse(DS.BoolTy);

    IRB.CreateCall(DS.GetStackIDsN, {IDsAlloc, UsedIDs});

    SmallVector<Value *, 8> IDs;
    IDs.resize(NumLocals);
    for (unsigned i = 0; i < NumLocals; i++) {
      Value *IDPtr = IRB.CreateInBoundsGEP(
          DS.U16Ty, IDsAlloc, {IRB.getInt64(i)});
      IDs[i] = IRB.CreateLoad(DS.U16Ty, IDPtr);
    }

    if (ClValidate) {
      ValidatePtrSrcs(IRB, IDs);
    }

    WriteBounds(IRB, IDs);
    TagPtrs(IRB, IDs);
  };

  if (isStackIDFastPathDisabled()) {
    UseStackIDSlowPathOnly();
    return;
  }

  // Can we use the special-case slow case helper?
  if (NumLocals <= 4) {
    StackIDCase = NumLocals;
  } else {
    StackIDCase = 0;
  }

  // Set up preamble
  hoistAllocas(F);

  ConstantInt *UsedIDs = IRB.getInt64(NumLocals);

  StartingStackID = IRB.CreateLoad(DS.U16Ty, DS.NextStackIDPtr);
  Value *RequestedStackID = IRB.CreateAdd(StartingStackID, IRB.getInt16(NumLocals));
  StackIDLimit = IRB.CreateLoad(DS.U16Ty, DS.StackIDLimitPtr);
  HasStackIDRoom = IRB.CreateICmpULE(RequestedStackID, StackIDLimit, "has.stack.id.room");

  if (StackIDCase == 0) {
    IDsAlloc = IRB.CreateAlloca(DS.U16Ty, UsedIDs, "stack.ids");
  } else {
    IDsAlloc = nullptr;
  }

  MDBuilder MDB(DS.Ctx);
  MDNode *BW = MDB.createBranchWeights(/*true*/1000, /*false*/1);
  Instruction *ThenTerm = nullptr, *ElseTerm = nullptr;
  SplitBlockAndInsertIfThenElse(HasStackIDRoom, IRB.GetInsertPoint(),
                                &ThenTerm, &ElseTerm,
                                BW, &PTA.DTU, &PTA.LI);


  // THEN path: materialize IDs via post-increment semantics --------------------
  IRB.SetInsertPoint(ThenTerm);
  SmallVector<Value *, 8> ThenIDs = GetFastCaseIDs(IRB, RequestedStackID);

  // In the special case functions, the bounds writing is done in the call, so
  // we only need to write bounds in the fast case here
  if (StackIDCase != 0) {
    WriteBounds(IRB, ThenIDs);
  }

  // ELSE path: call helper and read back scalars --------------------------------
  IRB.SetInsertPoint(ElseTerm);

  SmallVector<Value *, 8> ElseIDs;
  ElseIDs.resize(NumLocals);

  Value *IDsRetElse = nullptr;

  switch(StackIDCase) {
    case 0: {
      IRB.CreateCall(DS.GetStackIDsN, {IDsAlloc, UsedIDs});
      for (unsigned i = 0; i < NumLocals; i++) {
        Value *IDPtr = IRB.CreateInBoundsGEP(
                              DS.U16Ty,
                              IDsAlloc,
                              {IRB.getInt64(i)});
        ElseIDs[i] = IRB.CreateLoad(DS.U16Ty, IDPtr);
      }
      break;
    }
    case 1: {
      IDsRetElse = IRB.CreateCall(DS.GetStackIDs1, {GetBase(0), GetSpanU64(0)});
      ElseIDs[0] = IDsRetElse;
      break;
    }
    case 2: {
      Value *Sp01 = MakeTwoSpans(0, 1);
      IDsRetElse = IRB.CreateCall(DS.GetStackIDs2, {GetBase(0), GetBase(1), Sp01}); // returns i32
      Value *R0 = IRB.CreateTrunc(IDsRetElse, DS.U16Ty);
      Value *R1 = IRB.CreateTrunc(IRB.CreateLShr(IDsRetElse, IRB.getInt32(16)), DS.U16Ty);
      ElseIDs[0] = R0;
      ElseIDs[1] = R1;
      break;
    }
    case 3: {
      Value *Sp01 = MakeTwoSpans(0, 1);
      Value *Sp2  = GetSpanU32(2);
      IDsRetElse = IRB.CreateCall(DS.GetStackIDs3, {GetBase(0), GetBase(1), GetBase(2), Sp01, Sp2}); // returns i64
      Value *R0 = IRB.CreateTrunc(IDsRetElse, DS.U16Ty);
      Value *R1 = IRB.CreateTrunc(IRB.CreateLShr(IDsRetElse, IRB.getInt64(16)), DS.U16Ty);
      Value *R2 = IRB.CreateTrunc(IRB.CreateLShr(IDsRetElse, IRB.getInt64(32)), DS.U16Ty);
      ElseIDs[0] = R0;
      ElseIDs[1] = R1;
      ElseIDs[2] = R2;
      break;
    }
    case 4: {
      Value *Sp01 = MakeTwoSpans(0, 1);
      Value *Sp23 = MakeTwoSpans(2, 3);
      IDsRetElse = IRB.CreateCall(DS.GetStackIDs4, {GetBase(0), GetBase(1), GetBase(2), GetBase(3), Sp01, Sp23}); // returns i64
      Value *R0 = IRB.CreateTrunc(IDsRetElse, DS.U16Ty);
      Value *R1 = IRB.CreateTrunc(IRB.CreateLShr(IDsRetElse, IRB.getInt64(16)), DS.U16Ty);
      Value *R2 = IRB.CreateTrunc(IRB.CreateLShr(IDsRetElse, IRB.getInt64(32)), DS.U16Ty);
      Value *R3 = IRB.CreateTrunc(IRB.CreateLShr(IDsRetElse, IRB.getInt64(48)), DS.U16Ty);
      ElseIDs[0] = R0;
      ElseIDs[1] = R1;
      ElseIDs[2] = R2;
      ElseIDs[3] = R3;
      break;
    }
    default:
      errs() << "Unexpected StackIDCase " << StackIDCase << "\n";
      die();
  }

  // MERGE: create PHIs named ID_0 .. ID_{N-1} -----------------------------------
  BasicBlock *ContBB = ThenTerm->getParent()->getUniqueSuccessor();
  assert(ContBB);
  IRB.SetInsertPoint(&*ContBB->getFirstNonPHI());
  SmallVector<Value *, 8> StackIDPHIs;
  StackIDPHIs.resize(NumLocals);

  for (unsigned i = 0; i < NumLocals; i++) {
    PHINode *Phi = IRB.CreatePHI(DS.U16Ty, /*NumReservedValues=*/2,
                                      Twine("ID_") + Twine(i));
    Phi->addIncoming(ThenIDs[i], ThenTerm->getParent());
    Phi->addIncoming(ElseIDs[i], ElseTerm->getParent());
    StackIDPHIs[i] = (Phi);
  }

  // Merge IDsRet if I have to
  if (StackIDCase > 0) {
    assert(IDsRetElse != nullptr);
    assert(IDsAlloc == nullptr);
    PHINode *RetPhi = IRB.CreatePHI(IDsRetElse->getType(), 2, "ptsan.ids.ret");
    Value *Poison = PoisonValue::get(IDsRetElse->getType());
    RetPhi->addIncoming(Poison, ThenTerm->getParent());
    RetPhi->addIncoming(IDsRetElse, ElseTerm->getParent());
    IDsRet = RetPhi;
  } else {
    IDsRet = nullptr;
    assert(IDsRetElse == nullptr);
    assert(IDsAlloc != nullptr);
  }

  // Now load/use our merged values ------------------------------------
  IRB.SetInsertPoint(&*ContBB->getFirstInsertionPt());
  if (ClValidate) {
    ValidatePtrSrcs(IRB, StackIDPHIs);
  }

  // Non-special-case we write bounds here
  if (StackIDCase == 0) {
    WriteBounds(IRB, StackIDPHIs);
  }

  TagPtrs(IRB, StackIDPHIs);
}


Value *PTSanLowerer::lowerGetID(GetIDAnchor IDAnchor) {
  Value *Ptr = IDAnchor.getBase();

  PTSanIRB IRB(DS, IDAnchor.Call);
  markIRAsFrom(IRB, "ptsan.ir.getid");
  Value *BaseInt = IRB.CreatePtrToInt(Ptr, DS.U64Ty,
                                      Ptr->getName() + ".asint");
  return IRB.CreateLShr(BaseInt, 48, Ptr->getName() + ".id");
}

Value *PTSanLowerer::lowerBound(BoundAnchor Bounds) {
  if (auto LoadBounds = DS.getAsAnchor<LoadBoundAnchor>(Bounds.Call)) {
    Value *ID = LoadBounds->getID();
    unsigned FieldIndex = Bounds.isBase() ? 0 : 1;

    PTSanIRB IRB(DS, Bounds.Call);
    markIRAsFrom(IRB, "ptsan.ir.loadbounds");

    Value *GEP = IRB.CreateInBoundsGEP(
        DS.BoundsArray->getValueType(),
        DS.BoundsArray,
        {IRB.getInt64(0), ID, IRB.getInt32(FieldIndex)},
        ID->getName() + (Bounds.isBase() ? ".baseGep" : ".spanGep"));
    return IRB.CreateLoad(DS.U64Ty, GEP,
                          ID->getName() +
                              (Bounds.isBase() ? ".base" : ".span"));
  }

  auto StaticBounds = *DS.getAsAnchor<StaticBoundAnchor>(Bounds.Call);
  Value *Lowered = StaticBounds.getStaticValue();
  if (Bounds.isBase()) {
    PTSanIRB IRB(DS, Bounds.Call);
    markIRAsFrom(IRB, "ptsan.ir.staticbase");
    Lowered = IRB.CreatePtrToInt(Lowered, DS.U64Ty,
                                 Lowered->getName() + ".static.base");
  }

  return Lowered;
}

optional<RangeCheckInfo>
PTSanLowerer::addPtrCheck(const PlaceablePtrCheck &Check) {
  uint32_t CheckID = NextCheckID++;

  DBG(errs() << "\n\nLowering check " << CheckID << " on "
             << *Check.BasePtr << "\n");
  DBG(errs() << "Check type is " << Check.CheckTy << "\n");
  DBG(errs() << "Covered uses are:\n");
  for (Use *U: Check.UsesCovering) {
    DBG(errs() << "\t" << *U->getUser() << "\n");
  }

  if (ClNoPTSanOpts) {
    assert(Check.UsesCovering.size() == 1);
    assert(Check.CheckTy != COMPOUND);
  }

  StringRef Name = Check.BasePtr->getName();
  Value *PtrID = Check.Anchor.getID();
  BoundAnchor BaseBound = Check.Anchor.getBaseBound();
  BoundAnchor SpanBound = Check.Anchor.getSpanBound();
  assert(Check.Anchor.hasMaterializedRange() &&
         "check range metadata should be materialized before lowering");
  assert(BaseBound.isBase() && SpanBound.isSpan() &&
         "check should carry scalar base/span bounds");
  Value *Base = BaseBound.Call;
  Value *Span = SpanBound.Call;

  DBG(
    PTA.SE.verify();
    assert(PTA.DT.verify());
    assert(PTA.PDT.verify());
    PTA.LI.verify(PTA.DT);
  );

  Instruction *CheckIP = Check.getCheckIP(PTA.DT);

  DBG(errs() << "CheckIP: " << *CheckIP << "\n");
  DBG(errs() << "Base bounds IP: " << *BaseBound.getTarget().getIP() << "\n");
  DBG(errs() << "Span bounds IP: " << *SpanBound.getTarget().getIP() << "\n");

  DBG(errs() << "Instigating instructions are:\n");
  for (Use *U : Check.InstigatingUses) {
    Instruction *I = cast<Instruction>(U->getUser());
    DBG(errs() << "\t" << *I << "\n");
    assert(I == CheckIP || PTA.PDT.dominates(I, CheckIP));
    assert(I == CheckIP || PTA.DT.dominates(CheckIP, I));
    assert(PTA.PTU.hoistTargetDominates(BaseBound.getTarget(), CheckIP));
    assert(PTA.PTU.hoistTargetDominates(BaseBound.getTarget(), I));
    assert(PTA.PTU.hoistTargetDominates(SpanBound.getTarget(), CheckIP));
    assert(PTA.PTU.hoistTargetDominates(SpanBound.getTarget(), I));
  }

  Value *MinPtr = Check.Anchor.getRangeMinPtr();
  Value *Size = Check.Anchor.getRangeSize();

  Instruction *MinIP = PTA.PTU.getFirstIPAfter(MinPtr).getIP();
  assert(DS.getAsAnchor<TaggedAnchor>(MinPtr) &&
         "check range min should be materialized as a tagged value");

  PTSanIRB IRB(DS, MinIP);
  markIRAsFrom(IRB, "ptsan.ir.ptrcheck.preamble");
  Value *Min = IRB.CreatePtrToInt(MinPtr, Base->getType());

  DBG(errs() << "Min value " << *Min << "\n");
  DBG(errs() << "Size value " << *Size << "\n");

  Value *SizeVal = Size;

  // We insert at CheckIP, but all those values had better be live there.
  // Check the materialized operands directly instead of using a collapsed
  // common insertion point: around invoke/unwind edges, the latter can name the
  // invoke even when the actual operands were created before it.
  assert(([&]() {
    for (Value *V : {Min, SizeVal, Span, Base}) {
      Instruction *I = dyn_cast<Instruction>(V);
      if (!I)
        continue;
      if (I == CheckIP || !PTA.DT.dominates(I, CheckIP))
        return false;
    }
    return true;
  }()) && "check operands must dominate CheckIP");

  IRB.SetInsertPoint(CheckIP);

  // Offset into the buffer
  Value *Offset = IRB.CreateSub(Min, Base, Name + ".offset");

  Value *OutOfRange = nullptr;
  if (ClAddOverflowChecks) {
    Value *EndWithOverflow = IRB.CreateBinaryIntrinsic(
        Intrinsic::uadd_with_overflow, Offset, Size, nullptr,
        Name + ".end.ov");
    Value *End = IRB.CreateExtractValue(EndWithOverflow, {0}, Name + ".end");
    Value *EndOverflow =
        IRB.CreateExtractValue(EndWithOverflow, {1}, Name + ".end.oor");
    Value *PastEnd = IRB.CreateICmpUGT(End, Span, Name + ".cmp");
    OutOfRange = IRB.CreateOr(EndOverflow, PastEnd, Name + ".oor");
  } else {
    Value *CmpSize = IRB.CreateICmpULE(Size, Span, Name + ".szChk");
    // Space before which offset will overflow
    Value *Slack = IRB.CreateSub(Span, Size, Name + ".slack");
    Value *CmpLower = IRB.CreateICmpULE(Offset, Slack, Name + ".cmp");
    Value *InRange = IRB.CreateAnd(CmpLower, CmpSize, Name + ".inRange");
    OutOfRange = IRB.CreateNot(InRange, ".outOfRange");
  }
  Instruction *OORInst = dyn_cast<Instruction>(OutOfRange);
  if (OORInst == nullptr) {
    // Always OOB
    if (OutOfRange == ConstantInt::getTrue(DS.Ctx)) {
      DBG(errs() << "Always OOB??\n");
      OORInst = SelectInst::Create(OutOfRange, OutOfRange, OutOfRange, "ptsan.bounds", CheckIP);
    } else if (OutOfRange == ConstantInt::getFalse(DS.Ctx)) {
      DBG(errs() << "Check will always be in bounds\n");
      return nullopt;
    } else {
      errs() << "Unhandled non-inst Out Of Range: " << *OutOfRange << "\n";
      die();
    }
  }


  for (Use *U : Check.InstigatingUses) {
    Instruction *I = cast<Instruction>(U->getUser());
    assert(PTA.DT.dominates(OORInst, I));
    assert(PTA.PDT.dominates(I, OORInst));
  }
  // We need SE info here, so we don't want to actually add the checks (which changes
  // CFG, which invalidates SE info) until they're all done. So put them here,
  // and then call actuallyAddCheck later

  assert(isa<Instruction>(OORInst));
  return RangeCheckInfo{{OORInst, AFTER},
                        Check.CheckTy,
                        CheckID,
                        PtrID,
                        OORInst,
                        Min,
                        Size};
}


void PTSanLowerer::lowerLibcCall(Instruction *I) {
  PTSanIRB IRB(DS, I);

  LoadInst *Old = IRB.CreateLoad(DS.U32Ty, DS.PTSanInLibc);
  Value *Inc = IRB.CreateAdd(Old, ConstantInt::get(DS.U32Ty, 1));
  IRB.CreateStore(Inc, DS.PTSanInLibc);

  // Insert: ptsan_in_libc-- immediately after I.
  Instruction *Next = I->getNextNode();
  assert(Next);

  IRB.SetInsertPoint(Next);

  // This isn't quite a --, but it should be equivalent and much cleaner
  IRB.CreateStore(Old, DS.PTSanInLibc);
}

void PTSanLowerer::lowerArithmeticValidation(
    ValidateArithAnchor Anchor) {
  PTSanIRB IRB(DS, Anchor.Call);

  auto materializeAsInt = [&](Value *V) -> Value * {
    if (V->getType()->isPointerTy()) {
      TaggedAnchor Tagged = TaggedAnchor::create(DS, Anchor.Call, V);
      return IRB.CreatePtrToInt(Tagged.Call, DS.U64Ty);
    }

    return V;
  };

  IRB.CreateCall(DS.ValidateArithmetic,
                 {materializeAsInt(Anchor.getLHS()),
                  materializeAsInt(Anchor.getRHS())});
}

void PTSanLowerer::lowerSpecialStripCall(CallBase *CB) {
  Function *Callee = CB->getCalledFunction();
  assert(Callee && "special strip calls must have a direct callee");
  assert(Callee->getName() == "makecontext" &&
         "unknown special strip call helper");

  SmallVector<Value *, 8> Args;
  Args.reserve(CB->arg_size());
  for (Use &U : CB->args()) {
    Args.push_back(U.get());
  }

  DebugLoc DL = CB->getDebugLoc();

  PTSanIRB IRB(DS, CB);
  CallInst *BeforeCall = IRB.CreateCall(DS.MakecontextStripBefore, Args);
  BeforeCall->setDebugLoc(DL);
  BeforeCall->setCallingConv(Callee->getCallingConv());
  BeforeCall->setDoesNotThrow();

  assert(!CB->isTerminator() && "Need to support invokes here :(");
  IRB.SetInsertPoint(CB->getNextNode());
  CallInst *AfterCall = IRB.CreateCall(DS.MakecontextStripAfter, Args);
  AfterCall->setDebugLoc(DL);
  AfterCall->setCallingConv(Callee->getCallingConv());
  AfterCall->setDoesNotThrow();
}

void PTSanLowerer::run() {
  for (auto ValidationAnchor : DS.collectAnchors<ValidateArithAnchor>(F)) {
    lowerArithmeticValidation(ValidationAnchor);
    ValidationAnchor.erase();
  }

  for (auto Special : DS.collectAnchors<SpecialStripAnchor>(F)) {
    auto *CB = cast<CallBase>(Special.getPositionedInstruction());
    lowerSpecialStripCall(CB);
    Special.erase();
  }

  for (StripAnchor Strip : DS.collectAnchors<StripAnchor>(F)) {
    Value *True = lowerStrip(Strip);
    Strip.Call->replaceAllUsesWith(True);
    Strip.erase();
  }

  SmallVector<RangeCheckInfo> RangeChecks;
  auto lowerCheckAnchor = [&](CheckAnchor CheckAnchor) {
    PlaceablePtrCheck &Op = PTA.PlaceablePtrChecks.at(CheckAnchor.Call);
    if (auto Check = addPtrCheck(Op)) {
      RangeChecks.push_back(*Check);
    }
    PTA.PlaceablePtrChecks.erase(CheckAnchor.Call);
    CheckAnchor.erase();
  };

  for (auto CheckAnchor : DS.collectAnchors<AccessAnchor>(F)) {
    lowerCheckAnchor(CheckAnchor);
  }

  for (auto CheckAnchor : DS.collectAnchors<LoopPreanalysisAnchor>(F)) {
    lowerCheckAnchor(CheckAnchor);
  }

  assert(PTA.PlaceablePtrChecks.empty() &&
         "all optimized check facts should be consumed by lowering");

  // Srcs after sinks so the sinks see any writes
  auto PtrSrcs = DS.collectAnchors<SourceAnchor>(F);
  NumLocals = PtrSrcs.size();
  lowerPtrSources(PtrSrcs);
  for (SourceAnchor Src : PtrSrcs) {
    Src.erase();
  }

  for (TaggedAnchor Tagged : DS.collectAnchors<TaggedAnchor>(F)) {
    Tagged.Call->replaceAllUsesWith(lowerTagged(Tagged));
    Tagged.erase();
  }

  for (TagAnchor Tag : DS.collectAnchors<TagAnchor>(F)) {
    Value *Tagged = lowerTag(Tag);
    Tag.Call->replaceAllUsesWith(Tagged);
    Tag.erase();
  }

  // Function exits need source-lowering state. Lower exits and libc markers
  // before range-check CFG insertion so actuallyAddCheck is the last CFG
  // mutation in this pass.
  for (auto Ret : DS.collectAnchors<FunctionExitAnchor>(F)) {
    lowerFunctionExit(Ret.getPositionedInstruction());
    Ret.erase();
  }

  for (auto Libc : DS.collectAnchors<LibcCallAnchor>(F)) {
    lowerLibcCall(Libc.getPositionedInstruction());
    Libc.erase();
  }

  // Range checks split blocks, so keep them as the final queued
  // check-lowering step.
  for (const RangeCheckInfo &Elt : RangeChecks) {
    actuallyAddCheck(Elt);
  }

  for (BoundAnchor Bounds : DS.collectAnchors<BoundAnchor>(F)) {
    Bounds.Call->replaceAllUsesWith(lowerBound(Bounds));
    Bounds.erase();
  }

  for (GetIDAnchor ID : DS.collectAnchors<GetIDAnchor>(F)) {
    ID.Call->replaceAllUsesWith(lowerGetID(ID));
    ID.erase();
  }
}

void PTSanLowerer::redirectGlobalUsesForFunction() {
  DBG(errs() << "------Removing constexprs on globals---------\n");

  for (auto &GI : DS.GlobalVariableProxies) {
    materializeConstantUsersOfGlobal(F, GI.first);
  }

  for (auto &GA : DS.GlobalAliasProxies) {
    materializeConstantUsersOfGlobal(F, GA.first);
  }

  DBG(errs() << "-------Redirecting instructions-----------\n");

  auto redirectUsesOf = [&](GlobalValue *Real, GlobalValue *Proxy) {
    Instruction * TheLoad = nullptr;
    Value * IDTopBits = nullptr;

    DBG(errs() << "Redirecting instructions for " << *Real << "\n");
    // Gotta do it this way instead of Real->uses because that's not stable
    // under iteration;
    for (auto UI = Real->use_begin(), UE = Real->use_end(); UI != UE; ) {
      Use &U = *UI++;
      if (Instruction *I = dyn_cast<Instruction>(U.getUser())) {
        // Kinda jank but w/e
        if (I->getFunction() != &F) {
          continue;
        }

        DBG(errs() << "\tUse is " << *I << "\n");

        if (TheLoad == nullptr) {
          PTSanIRB IRB(DS, &*F.getEntryBlock().getFirstInsertionPt());
          TheLoad = IRB.CreateLoad(DS.PtrTy, Proxy, "tagged");
        }

        if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(I)) {
          switch(II->getIntrinsicID()) {
            // threadlocal_address does *not* want a load. All reads *from* the threadlocal
            // address need an extra layer indirection though >.<
            case Intrinsic::threadlocal_address:
              // We want to | in the proxy id for each uses of the threadlocal variable:
            if (IDTopBits == nullptr) {
              PTSanIRB IRB(DS, TheLoad->getNextNode());
              Value *AsInt = IRB.CreatePtrToInt(TheLoad, DS.U64Ty, Real->getName() + ".asint");
              IDTopBits = IRB.CreateAnd(AsInt, DS.IDMask, Real->getName() + ".id-top");
            }

            assert(PTA.DT.dominates(TheLoad, II));
            PTSanIRB IRB(DS, II->getNextNode());
            Value *TLAsInt = IRB.CreatePtrToInt(II, DS.U64Ty, Real->getName() + ".tla.asint");
            Value *TLTagged = IRB.CreateOr(TLAsInt, IDTopBits, Real->getName() + ".tla.asint.tagged");
            Value *TLTaggedPtr = IRB.CreateIntToPtr(TLTagged, DS.PtrTy, Real->getName() + ".tla.tagged");

            for (auto WI = II->use_begin(), WE = II->use_end(); WI != WE; ) {
              Use &W = *WI++;
              if (W.getUser() != TLAsInt) {
                W.set(TLTaggedPtr);
              }
            }
            continue;
          }
        }

        U.set(TheLoad);
      }
    }
  };

  for (auto &GI : DS.GlobalVariableProxies) {
    redirectUsesOf(GI.first, GI.second);
  }

  for (auto &GA : DS.GlobalAliasProxies) {
    redirectUsesOf(GA.first, GA.second);
  }
}

void PTSanLowerer::actuallyAddCheck(const RangeCheckInfo &CI) {
  if(ClValidate) {
    PTSanIRB IRB(DS, cast<Instruction>(CI.OutOfRange));
    Constant *CheckTy   = ConstantInt::get(DS.U32Ty, CI.Type);
    IRB.CreateCall(DS.LogCheckCalled, {CheckTy});
  }

  MDBuilder MDB(DS.Ctx);
  MDNode *BW = MDB.createBranchWeights(1, 1000);

  Instruction *ThenTerm =
      SplitBlockAndInsertIfThen(CI.OutOfRange, CI.IP.getIP(), false, BW, &PTA.DTU, &PTA.LI);
  PTSanIRB IRB(DS, ThenTerm);
  Value *CheckID = ConstantInt::get(DS.U32Ty, CI.CheckID);

  std::string Desc = ChkTyToString(CI.Type);
  llvm::Value *DescPtr = IRB.CreateGlobalStringPtr(Desc);

  IRB.CreateCall(DS.ReportGenericError, {CheckID, FunctionID, CI.PtrID, DescPtr, CI.Min, CI.Size});
}

bool isUntaggedConstantPtr(Value *V) {
  uint64_t N;
  return match(V, m_IntToPtr(m_ConstantInt(N))) ||
         match(V, m_ConstantInt(N)) ||
         isa<ConstantPointerNull>(V);
}

static bool isUntaggedStripValue(Value *V) {
  if (isUntaggedConstantPtr(V) || isa<ConstantInt>(V)) {
    return true;
  }

  if (auto *PTI = dyn_cast<PtrToIntOperator>(V)) {
    return isUntaggedConstantPtr(PTI->getPointerOperand());
  }

  return false;
}

Value *PTSanLowerer::lowerStrip(StripAnchor Strip) {
  Value *V = Strip.getValue();
  if (ClNoStrip) {
    return V;
  }

  // PTSan doesn't strip constants (mostly null, but `inttoptr -1` is possible).
  if (isUntaggedStripValue(V)) {
    return V;
  }

  PTSanIRB IRB(DS, Strip.Call);
  markIRAsFrom(IRB, "ptsan.ir.lowerStrip");

  if (Strip.isIntegerStrip()) {
    return IRB.CreateAnd(V, DS.AddrMask, V->getName() + ".true");
  }

  assert(Strip.isPointerStrip() && "expected pointer or integer strip anchor");

  Function *PMFn =
      Intrinsic::getDeclaration(F.getParent(), Intrinsic::ptrmask,
                                {DS.PtrTy, DS.U64Ty});
  return IRB.CreateCall(PMFn, {V, DS.AddrMask}, V->getName() + ".true");
}

Value *PTSanLowerer::lowerTagged(TaggedAnchor Tagged) {
  return Tagged.getValue();
}

Value *PTSanLowerer::lowerTag(TagAnchor Tag) {
  Value *V = Tag.getValue();
  if (ClNoStrip) {
    return V;
  }

  PTSanIRB IRB(DS, Tag.Call);
  markIRAsFrom(IRB, "ptsan.ir.lowerTag");

  Type *IntTy = DS.U64Ty;
  if (auto *VTy = dyn_cast<VectorType>(V->getType())) {
    IntTy = VectorType::get(DS.U64Ty, VTy->getElementCount());
  }

  Value *ValueBits = IRB.CreatePtrToInt(V, IntTy, V->getName() + ".asint");
  Value *TagBits = IRB.CreateShl(Tag.getID(), 48, V->getName() + ".tagbits");
  if (auto *VTy = dyn_cast<VectorType>(V->getType())) {
    TagBits = IRB.CreateVectorSplat(VTy->getElementCount(), TagBits,
                                    V->getName() + ".tagbits.splat");
  }
  Value *TaggedBits =
      IRB.CreateOr(ValueBits, TagBits, V->getName() + ".tagged.asint");
  return IRB.CreateIntToPtr(TaggedBits, V->getType(),
                            V->getName() + ".tagged");
}

static DIScope *getOrCreateInlinee(DIBuilder &DIB,
                                        DICompileUnit *CU,
                                        Module &M,
                                        StringRef Name) {
  if (auto *NM = M.getNamedMetadata("ptsan.inlinees")) {
    for (auto *Op : NM->operands()) {
      auto *T = cast<MDNode>(Op);
      if (auto *S = dyn_cast<MDString>(T->getOperand(0));
          S && S->getString() == Name)
        return cast<DIScope>(T->getOperand(1));
    }
  }

  // Build a void(void) subroutine type
  auto *VoidTy = DIB.createUnspecifiedType("void");
  auto TypeArr = DIB.getOrCreateTypeArray({VoidTy /*return only*/});
  auto *SubTy   = DIB.createSubroutineType(TypeArr);

  auto *File = CU->getFile();
  DINode::DIFlags DIFlags = DINode::FlagPrototyped | DINode::FlagArtificial;

  DISubprogram::DISPFlags SPFlags =
      DISubprogram::SPFlagOptimized |
      DISubprogram::SPFlagDefinition;

  auto *SP = DIB.createFunction(/*Scope=*/File, Name, /*LinkageName=*/"",
                                /*File=*/File, /*Line=*/1,
                                /*Type=*/SubTy, /*ScopeLine=*/1,
                                /*Flags=*/DIFlags, /*SPFlags=*/SPFlags);
  auto Block = DIB.createLexicalBlock(SP, File, 1, 1);

  auto *NM = M.getOrInsertNamedMetadata("ptsan.inlinees");
  NM->addOperand(MDNode::get(M.getContext(),
               {MDString::get(M.getContext(), Name), Block}));
  return Block;
}

static DebugLoc makeInlineAt(IRBuilderBase &IRB, DIScope *Inlinee) {
  LLVMContext &Ctx = IRB.getContext();
  DebugLoc Caller = IRB.getCurrentDebugLocation();

  if (Caller) {
    // Use caller’s existing chain.
    return DebugLoc(DILocation::get(Ctx, 1, 1, Inlinee, Caller.get()));
  }

  // No current DL (common at entry). Use the enclosing function’s SP.
  Function *F = IRB.GetInsertBlock()->getParent();
  if (DISubprogram *CallerSP = F->getSubprogram()) {
    DILocation *At = DILocation::get(Ctx, /*line*/1, /*col*/1, CallerSP);
    return DebugLoc(DILocation::get(Ctx, 1, 1, Inlinee, At));
  }

  die();
}


void PTSanLowerer::markIRAsFrom(IRBuilderBase &IRB, StringRef Name) {
  if (!ClCreateIRDbg) {
    return;
  }
  auto *SP = getOrCreateInlinee(DS.DIB, DS.CU, DS.M, Name);
  IRB.SetCurrentDebugLocation(makeInlineAt(IRB, SP));
}

} // namespace llvm::ptsan
