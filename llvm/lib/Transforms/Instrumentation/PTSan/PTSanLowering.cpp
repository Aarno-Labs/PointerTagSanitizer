//===-- PTSanLowering.cpp - PTSan lowering phase -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/TargetParser/Triple.h"

namespace llvm::ptsan {

  class PTSanLowerer {
private:
  Function &F;
  PointerTagSanitizer &DS;
  PTSanAnalysisState &PTA;
  bool UseInlineStrip;

  uint32_t NextCheckID;

  // Number of allocated stack IDs to retire at each function exit.
  size_t NumAllocatedSourceIDs;
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
  Value *StartingStackID;
  Value *RepeatableAllocaIDs = nullptr;

  ConstantInt *FunctionID;

  // Lowering entry points
  DenseMap<Value *, Value *> lowerSources();
  // Allocate NumSourceIDs IDs and initialize bounds for the supplied sources.
  // Their IDs come first; the remaining IDs have bounds initialized later.
  SmallVector<Value *, 8> allocateSourceIDs(size_t NumSourceIDs,
                                         ArrayRef<SourceAnchor> PtrSrcs);
  void writeSourceBounds(PTSanIRB &IRB, Value *Source, Value *Size, Value *ID,
                         bool IsNotDefault = false);
  Value *lowerGetID(GetIDAnchor IDAnchor);
  Value *lowerBound(BoundAnchor Bounds);
  Value *lowerStrip(StripAnchor Strip);
  Value *lowerTagged(TaggedAnchor Tagged);
  Value *lowerTag(TagAnchor Tag);
  optional<RangeCheckInfo> addPtrCheck(const PlaceablePtrCheck &Op);
  void lowerFunctionExit(Instruction *I);
  void releaseFastStackIDs(PTSanIRB &IRB);
  void releaseSlowStackIDs(PTSanIRB &IRB);
  void lowerSetjmps(Instruction *FrameSetupEnd);
  void lowerArithmeticValidation(ValidateArithAnchor Op);
  void lowerSpecialStripCall(CallBase *CB);
  void actuallyAddCheck(const RangeCheckInfo &CI);
  void lowerLibcCall(Instruction *I);
  void appendVarargSentinels();

public:
  PTSanLowerer(Function &F, PointerTagSanitizer &DS,
                    PTSanAnalysisState &PTA);
  void run();
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
    : F(F), DS(DS), PTA(PTA), UseInlineStrip(DS.shouldUseInlineStrip()),
      NextCheckID(0) {
  FunctionID = ConstantInt::get(DS.U64Ty, getFunctionGUID(F));
}

bool PTSanLoweringPass::run(PTSanAnalysisState &PTA,
                           FunctionAnalysisManager &FAM) {
  Function &F = PTA.F;
  PointerTagSanitizer &DS = PTA.DS;

  if (ClValidate) {
    PTSanIRB IRB(DS, &*F.getEntryBlock().getFirstInsertionPt());
    IRB.CreateCall(DS.ValidateFunctionStart);
  }

  DBG(errs() << "\n---------Now lowering PTSan ops----------------\n\n");

  PTSanLowerer Lowerer(F, DS, PTA);
  Lowerer.run();

  // Constant-expression materialization can create identical instructions for
  // duplicate PHI edges, temporarily giving those edges different Values.
  // Reunify the materializations before verifying the lowered function.
  FAM.invalidate(F, PreservedAnalyses::none());
  PreservedAnalyses PA = EarlyCSEPass().run(F, FAM);
  FAM.invalidate(F, PA);

  if (verifyFunction(F, &errs())) {
    errs() << "\n-----------------------------------------------\n";
    errs() << "Found broken function:\n\n" << F << "\n\n";
    report_fatal_error("Function verification failed!");
  }

  return true;
}

void PTSanLowerer::lowerFunctionExit(Instruction *I) {
  if (RepeatableAllocaIDs) {
    PTSanIRB IRB(DS, I);
    IRB.CreateCall(DS.FreeRepeatableAllocaIDs, {RepeatableAllocaIDs});
  }
  if (NumAllocatedSourceIDs == 0 || ClNoIDs) {
    return;
  }

  PTSanIRB IRB(DS, I);

  if (isStackIDFastPathDisabled()) {
    assert(StackIDCase == 0);
    releaseSlowStackIDs(IRB);
    return;
  }

  MDBuilder MDB(DS.Ctx);
  MDNode *BW = MDB.createBranchWeights(/*true*/1000, /*false*/1);
  Instruction *FastPath = nullptr, *SlowPath = nullptr;
  SplitBlockAndInsertIfThenElse(HasStackIDRoom, IRB.GetInsertPoint(),
                                &FastPath, &SlowPath,
                                BW, &PTA.DTU, &PTA.LI);
  IRB.SetInsertPoint(FastPath);
  releaseFastStackIDs(IRB);
  IRB.SetInsertPoint(SlowPath);
  releaseSlowStackIDs(IRB);
}

void PTSanLowerer::releaseFastStackIDs(PTSanIRB &IRB) {
  // In fast path, choose between delaying stack ID reuse for temporal safety or
  // immediately recycling IDs for speed.
  if (ClStackTemporalSafety) {
    assert(DS.PendingStackIdsPtr &&
           "pending stack IDs only exist with stack temporal safety");
    Value *PendingIDs16 =
      IRB.CreateLoad(DS.U16Ty, DS.PendingStackIdsPtr, "pending.frees");
    ConstantInt *Enough = IRB.getInt16(64);
    Value *PendingIdsInc16 = IRB.CreateAdd(
        PendingIDs16, IRB.getInt16(NumAllocatedSourceIDs), "pending.frees.inc");
    Value *EnoughPendingIDs = IRB.CreateICmpUGE(PendingIdsInc16, Enough, "enough.pending.ids");
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

    // Total bytes = NumAllocatedSourceIDs * sizeof(BoundsStructTy)
    uint64_t ElemSz = DS.DL.getTypeStoreSize(DS.BoundsStructTy);
    Value *TotalSz = IRB.getInt64(ElemSz * (uint64_t)NumAllocatedSourceIDs);

    Align A = DS.DL.getABITypeAlign(DS.BoundsStructTy);

    IRB.CreateMemSet(
        FirstSlot,
        IRB.getInt8(0),
        TotalSz,
        A);
  } else {
    IRB.CreateStore(StartingStackID, DS.NextStackIDPtr);
  }
}

void PTSanLowerer::releaseSlowStackIDs(PTSanIRB &IRB) {
  ConstantInt *UsedIDs = IRB.getInt64(NumAllocatedSourceIDs);
  // Case 0 means yes IdsAlloc, no IDsRet, while Case > 0 means inverse
  assert((StackIDCase == 0) == (IDsAlloc != nullptr));
  assert((StackIDCase == 0) == (IDsRet == nullptr));
  switch (StackIDCase) {
    case 0:
      IRB.CreateCall(DS.FreeStackIDsN, {IDsAlloc, UsedIDs});
      break;
    case 1:
      IRB.CreateCall(DS.FreeStackIDs1, {IDsRet})
          ->addParamAttr(0, Attribute::ZExt);
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

void PTSanLowerer::lowerSetjmps(Instruction *FrameSetupEnd) {
  // Discovery marks the setjmp-family calls whose second return needs cleanup.
  auto Setjmps = DS.collectAnchors<SetjmpAnchor>(F);
  if (Setjmps.empty()) {
    return;
  }

  // Keep the checkpoint on this frame's stack so its storage needs no explicit
  // cleanup, even if a later longjmp abandons this frame.
  PTSanIRB EntryIRB(DS, &*F.getEntryBlock().getFirstInsertionPt());
  Value *Checkpoint = EntryIRB.CreateAlloca(
      ArrayType::get(DS.U64Ty, 2), nullptr, "ptsan.stack.checkpoint");

  // Save once per invocation, after this frame's fixed IDs and repeatable-
  // alloca container exist. All setjmps in this frame preserve those resources.
  PTSanIRB IRB(DS, FrameSetupEnd);
  IRB.CreateCall(DS.SaveStackIDCheckpoint, {Checkpoint});
  // Save the fast-track cursor here so we can restore when we jump back
  auto *NextID = IRB.CreateLoad(DS.U16Ty, DS.NextStackIDPtr, "saved.stack.id");
  Value *PendingIDs = nullptr;
  if (ClStackTemporalSafety) {
    PendingIDs = IRB.CreateLoad(DS.U16Ty, DS.PendingStackIdsPtr,
                                "saved.pending.ids");
  }

  for (SetjmpAnchor Anchor : Setjmps) {
    auto *CB = cast<CallBase>(Anchor.getPositionedInstruction());
    // Test the result immediately after the call (or on an invoke's normal
    // edge). Initial setjmp returns zero; returning through longjmp does not.
    IRB.SetInsertPoint(PTA.PTU.getFirstIPAfter(CB).getIP());
    Value *ReturnedAgain = IRB.CreateICmpNE(
        CB, ConstantInt::get(CB->getType(), 0), "setjmp.returned.again");
    // Put cleanup in the nonzero branch, updating dominator and loop analyses.
    // Restore is that branch's terminator, so insert cleanup before it.
    Instruction *Restore = SplitBlockAndInsertIfThen(
        ReturnedAgain, IRB.GetInsertPoint(), false, nullptr, &PTA.DTU, &PTA.LI);
    IRB.SetInsertPoint(Restore);
    // Recycle slow IDs and containers beyond the saved collection depths.
    // The runtime does not restore the fast-stack state; that is done below.
    IRB.CreateCall(DS.RestoreStackIDCheckpoint, {Checkpoint});

    if (ClStackTemporalSafety) {
      // Clear bounds in [saved cursor, current cursor), including IDs whose
      // normal frame-exit cleanup was skipped by longjmp.
      Value *CurrentID = IRB.CreateLoad(DS.U16Ty, DS.NextStackIDPtr);
      Value *Count = IRB.CreateZExt(IRB.CreateSub(CurrentID, NextID), DS.U64Ty);
      Value *FirstSlot = IRB.CreateInBoundsGEP(
          DS.BoundsArray->getValueType(), DS.BoundsArray,
          {IRB.getInt64(0), IRB.CreateZExt(NextID, DS.U64Ty)});
      uint64_t BoundsSize = DS.DL.getTypeAllocSize(DS.BoundsStructTy);
      Value *Bytes = IRB.CreateMul(Count, IRB.getInt64(BoundsSize));
      IRB.CreateMemSet(FirstSlot, IRB.getInt8(0), Bytes,
                      DS.DL.getABITypeAlign(DS.BoundsStructTy));
      IRB.CreateStore(PendingIDs, DS.PendingStackIdsPtr);
    }

    // Rewind the fast-stack allocator in both modes. Temporal mode additionally
    // cleared bounds above; without it, cursor restoration alone is enough.
    IRB.CreateStore(NextID, DS.NextStackIDPtr);
    // Remove the discovery marker, not the original setjmp call.
    Anchor.erase();
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

void PTSanLowerer::writeSourceBounds(PTSanIRB &IRB, Value *Source, Value *Size,
                                   Value *ID, bool IsNotDefault) {
  // Keep the tagged base in sync with static-bounds lowering.
  Value *Base = IRB.CreatePtrToInt(Source, DS.U64Ty);
  Value *TaggedAddr = IRB.CreateOr(Base, IRB.CreateShl(ID, 48));
  Value *Bounds = PoisonValue::get(DS.BoundsStructTy);
  Bounds = IRB.CreateInsertValue(Bounds, TaggedAddr, {0});
  Bounds = IRB.CreateInsertValue(Bounds, Size, {1});
  Value *Slot = IRB.CreateInBoundsGEP(DS.BoundsArray->getValueType(),
                                   DS.BoundsArray, {IRB.getInt64(0), ID});
  // Strict-mode allocation terminates on exhaustion. The successful bucket
  // fast path also cannot produce a default ID, in either mode.
  if (!ClStrictMode && !IsNotDefault) {
    Value *IsDefaultID =
        IRB.CreateICmpEQ(ID, IRB.CreateZExt(DS.DefaultIDNo, DS.U64Ty));
    Slot = IRB.CreateSelect(IsDefaultID, DS.OverflowIDBoundsPtr, Slot);
  }
  IRB.CreateStore(Bounds, Slot);
}

// PtrSrcs are only the sources with known bounds at entry
SmallVector<Value *, 8>
PTSanLowerer::allocateSourceIDs(size_t NumSourceIDs,
                               ArrayRef<SourceAnchor> PtrSrcs) {
  assert(NumSourceIDs >= PtrSrcs.size());
  NumAllocatedSourceIDs = NumSourceIDs;
  if (NumSourceIDs == 0) {
    return {};
  }

  if (ClNoIDs) {
    return SmallVector<Value *, 8>(NumSourceIDs, ConstantInt::get(DS.U64Ty, 0));
  }
  // The stack-ID fast path intentionally uses a u16 add/compare sequence:
  //
  //   requested = ptsan_next_stack_id + NumSourceIDs
  //   requested <= ptsan_stack_id_limit
  //
  // Runtime bucket configuration keeps stack_id_limit in the low half of the
  // u16 space. This compile-time guard handles pathological single functions
  // without changing that hot path or adding a widened runtime check.
  if (NumSourceIDs > 0x7fff) {
    report_fatal_error(
        "PTSan cannot assign more than 0x7fff stack IDs to one function");
  }

  PTSanIRB IRB(DS, &*F.getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
  markIRAsFrom(IRB, "ptsan.ir.lowerPtrSources");
  auto GetSpanU32 = [&](unsigned i) -> Value * {
    Value *Source = PtrSrcs[i].getSource();
    if (auto *AI = dyn_cast<AllocaInst>(Source)) {
      auto Size = AI->getAllocationSize(DS.DL);
      assert(Size && !Size->isScalable() &&
             "PTSan source alloca should have a fixed size");
      assert(Size->getFixedValue() < 0xFFFFFFFFULL);
      return IRB.getInt32(Size->getFixedValue());
    }

    if (F.getName() == "main" && F.arg_size() >= 2 &&
        Source == F.getArg(1)) {
      Value *ArgC = F.getArg(0);
      Value *Count = IRB.CreateAdd(ArgC, IRB.getInt32(1));
      return IRB.CreateMul(Count, IRB.getInt32(DS.DL.getPointerSize()));
    }

    if (auto *Arg = dyn_cast<Argument>(Source)) {
      assert(Arg->hasByValAttr() &&
             "non-main PTSan argument source must be passed by value");
      TypeSize Size =
          DS.DL.getTypeAllocSize(Arg->getParamByValType());
      assert(!Size.isScalable() &&
             "PTSan does not support scalable pass-by-value objects");
      return IRB.getInt32(Size.getFixedValue());
    }

    errs() << "Unknown PTSan source anchor operand: " << *Source << "\n";
    die();
  };

  auto GetSpanU64 = [&](unsigned i) {
    return IRB.CreateZExt(GetSpanU32(i), DS.U64Ty, "span.u64");
  };

  auto GetFastCaseIDs = [&](PTSanIRB &IRB, Value *RequestedStackID) {
    SmallVector<Value *, 8> IDs;
    IDs.resize(NumSourceIDs);

    for (unsigned i = 0; i < NumSourceIDs; i++) {
      Value *ID = (i == 0)
          ? StartingStackID
          : IRB.CreateAdd(StartingStackID, IRB.getInt16(i), "next.plus");
      IDs[i] = ID;
    }

    //ptsan_next_stack_id += N;
    IRB.CreateStore(RequestedStackID, DS.NextStackIDPtr);

    return IDs;
  };

  auto WriteBounds = [&](const SmallVector<Value *, 8> &IDs,
                         bool IsNotDefault = false) {
    for (size_t i = 0; i < PtrSrcs.size(); i++) {
      Value *Source = PtrSrcs[i].getSource();
      Value *Span64 = GetSpanU64(i);
      Value *ID_64 = IRB.CreateZExt(IDs[i], DS.U64Ty);
      writeSourceBounds(IRB, Source, Span64, ID_64, IsNotDefault);
    }
  };

  auto WidenIDs = [&](SmallVector<Value *, 8> IDs) {
    for (Value *&ID : IDs) {
      ID = IRB.CreateZExt(ID, DS.U64Ty);
    }
    return IDs;
  };

  auto MakeTwoSpans = [&](unsigned i0, unsigned i1) {
    Value *A64 = GetSpanU64(i0);
    Value *B64 = GetSpanU64(i1);
    Value *Sp  = IRB.CreateOr(A64, IRB.CreateShl(B64, IRB.getInt64(32)));
    return Sp;
  };

  auto GetGenericIDs = [&]() {
    IRB.CreateCall(DS.GetStackIDsN, {IDsAlloc, IRB.getInt64(NumSourceIDs)});
    SmallVector<Value *, 8> IDs;
    IDs.resize(NumSourceIDs);
    for (unsigned i = 0; i < NumSourceIDs; i++) {
      Value *IDPtr = IRB.CreateInBoundsGEP(
          DS.U16Ty, IDsAlloc, {IRB.getInt64(i)});
      IDs[i] = IRB.CreateLoad(DS.U16Ty, IDPtr);
    }
    return IDs;
  };

  if (isStackIDFastPathDisabled()) {
    StackIDCase = 0;
    IDsRet = nullptr;
    hoistAllocas(F);
    IDsAlloc = IRB.CreateAlloca(DS.U16Ty, IRB.getInt64(NumSourceIDs), "stack.ids");
    HasStackIDRoom = ConstantInt::getFalse(DS.BoolTy);

    SmallVector<Value *, 8> IDs = GetGenericIDs();
    WriteBounds(IDs);
    return WidenIDs(std::move(IDs));
  }

  // The small slow-path helpers also initialize every ID's bounds. Use them
  // only when the whole batch has bounds available at entry. Otherwise the
  // generic helper allocates IDs without writing bounds.
  if (NumSourceIDs <= 4 && NumSourceIDs == PtrSrcs.size()) {
    StackIDCase = NumSourceIDs;
  } else {
    StackIDCase = 0;
  }

  // Set up preamble
  hoistAllocas(F);

  ConstantInt *UsedIDs = IRB.getInt64(NumSourceIDs);

  StartingStackID = IRB.CreateLoad(DS.U16Ty, DS.NextStackIDPtr);
  Value *RequestedStackID = IRB.CreateAdd(StartingStackID, IRB.getInt16(NumSourceIDs));
  Value *StackIDLimit = IRB.CreateLoad(DS.U16Ty, DS.StackIDLimitPtr);
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
    WriteBounds(ThenIDs, /*IsNotDefault=*/true);
  }

  // ELSE path: call helper and read back scalars --------------------------------
  IRB.SetInsertPoint(ElseTerm);

  SmallVector<Value *, 8> ElseIDs;
  ElseIDs.resize(NumSourceIDs);

  Value *IDsRetElse = nullptr;

  switch (StackIDCase) {
    case 0:
      ElseIDs = GetGenericIDs();
      break;
    case 1:
      IDsRetElse = IRB.CreateCall(
          DS.GetStackIDs1, {PtrSrcs[0].getSource(), GetSpanU64(0)});
      break;
    case 2: {
      Value *Sp01 = MakeTwoSpans(0, 1);
      IDsRetElse = IRB.CreateCall(
          DS.GetStackIDs2,
          {PtrSrcs[0].getSource(), PtrSrcs[1].getSource(), Sp01});
      break;
    }
    case 3: {
      Value *Sp01 = MakeTwoSpans(0, 1);
      Value *Sp2 = GetSpanU32(2);
      IDsRetElse = IRB.CreateCall(
          DS.GetStackIDs3,
          {PtrSrcs[0].getSource(), PtrSrcs[1].getSource(),
           PtrSrcs[2].getSource(), Sp01, Sp2});
      break;
    }
    case 4: {
      Value *Sp01 = MakeTwoSpans(0, 1);
      Value *Sp23 = MakeTwoSpans(2, 3);
      IDsRetElse = IRB.CreateCall(
          DS.GetStackIDs4,
          {PtrSrcs[0].getSource(), PtrSrcs[1].getSource(),
           PtrSrcs[2].getSource(), PtrSrcs[3].getSource(), Sp01, Sp23});
      break;
    }
    default:
      errs() << "Unexpected StackIDCase " << StackIDCase << "\n";
      die();
  }

  // Specialized helpers return consecutive 16-bit IDs packed into an integer.
  if (StackIDCase != 0) {
    for (unsigned i = 0; i < NumSourceIDs; ++i) {
      Value *ID = IRB.CreateLShr(IDsRetElse, 16 * i);
      ElseIDs[i] = IRB.CreateTrunc(ID, DS.U16Ty);
    }
  }

  // MERGE: create PHIs named ID_0 .. ID_{N-1} -----------------------------------
  BasicBlock *ContBB = ThenTerm->getParent()->getUniqueSuccessor();
  assert(ContBB);
  IRB.SetInsertPoint(&*ContBB->getFirstNonPHI());
  SmallVector<Value *, 8> StackIDPHIs;
  StackIDPHIs.resize(NumSourceIDs);

  for (unsigned i = 0; i < NumSourceIDs; i++) {
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
  // Non-special-case we write bounds here
  if (StackIDCase == 0) {
    WriteBounds(StackIDPHIs);
  }

  return WidenIDs(std::move(StackIDPHIs));
}

// SysV x86-64 and AAPCS64 pass IR byval arguments on the stack even when
// registers remain. This is an LLVM lowering rule, not C aggregate placement.
// The runtime owns the marker contents, shared by the producer and scanner.
void PTSanLowerer::appendVarargSentinels() {
  Module &M = *PTA.F.getParent();
  Triple Target(M.getTargetTriple());
  if ((Target.getArch() != Triple::x86_64 && !Target.isAArch64()) ||
      !Target.isOSLinux()) {
    return;
  }
  // Find vararg calls
  SmallVector<CallBase *, 8> Calls;
  for (Instruction &I : instructions(PTA.F)) {
    auto *CB = dyn_cast<CallBase>(&I);
    if (CB && CB->getFunctionType()->isVarArg() && !CB->isInlineAsm() &&
        !isa<IntrinsicInst>(CB) && !PTA.DS.isInstr(CB->getCalledOperand())) {
      Calls.push_back(CB);
    }
  }

  // Append a byval sentinel after the stack arguments. At va_start, the runtime
  // scans to this marker to determine the size without duplicating ABI argument
  // placement rules.
  for (CallBase *CB : Calls) {
    if (CB->getCallingConv() != CallingConv::C || CB->isMustTailCall()) {
      report_fatal_error("PTSan varargs require the C calling convention without musttail");
    }

    SmallVector<Value *, 8> Args(CB->args());
    Args.push_back(DS.PTSanVarargSentinel);

    // Function argument number changing copied from DeadArgumentElimination.cpp
    SmallVector<OperandBundleDef, 1> OpBundles;
    CB->getOperandBundlesAsDefs(OpBundles);

    FunctionCallee Callee(CB->getFunctionType(), CB->getCalledOperand());
    CallBase *NewCB = nullptr;
    if (InvokeInst *II = dyn_cast<InvokeInst>(CB)) {
      NewCB = InvokeInst::Create(Callee, II->getNormalDest(), II->getUnwindDest(),
                                Args, OpBundles, "", CB->getIterator());
    } else {
      NewCB = CallInst::Create(Callee, Args, OpBundles, "", CB->getIterator());
      cast<CallInst>(NewCB)->setTailCallKind(
          cast<CallInst>(CB)->getTailCallKind());
    }
    NewCB->setCallingConv(CB->getCallingConv());
    NewCB->setAttributes(CB->getAttributes());
    NewCB->copyMetadata(*CB);

    // Make sure sentinel is passed by value
    NewCB->addParamAttr(Args.size() - 1,
        Attribute::getWithByValType(M.getContext(),
                                   DS.PTSanVarargSentinel->getValueType()));
    // And actually replace
    PTA.SE.forgetValue(CB);
    CB->replaceAllUsesWith(NewCB);
    NewCB->takeName(CB);
    CB->eraseFromParent();
  }
}

Value *PTSanLowerer::lowerGetID(GetIDAnchor IDAnchor) {
  Value *Ptr = IDAnchor.getBase();
  Type *IDTy = Ptr->getType()->getWithNewType(DS.U64Ty);

  PTSanIRB IRB(DS, IDAnchor.Call);
  markIRAsFrom(IRB, "ptsan.ir.getid");
  Value *BaseInt = IRB.CreatePtrToInt(Ptr, IDTy, Ptr->getName() + ".asint");
  return IRB.CreateLShr(BaseInt, ConstantInt::get(IDTy, 48),
                        Ptr->getName() + ".id");
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

    if (LoadBounds->isVector()) {
      return IRB.CreateMaskedGather(
          ID->getType(), GEP, Align(8), LoadBounds->getMask(), nullptr,
          ID->getName() + (Bounds.isBase() ? ".base" : ".span"));
    }

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
         "check should carry base/span bounds");
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
  Type *CheckIntTy = MinPtr->getType()->getWithNewType(DS.U64Ty);
  Value *Min = IRB.CreatePtrToInt(MinPtr, CheckIntTy);

  DBG(errs() << "Min value " << *Min << "\n");
  DBG(errs() << "Size value " << *Size << "\n");

  if (Check.Mask) {
    Base = PTA.PTU.splatToMatchType(Base, CheckIntTy);
    Span = PTA.PTU.splatToMatchType(Span, CheckIntTy);
  }

  IRB.SetInsertPoint(CheckIP);

  // We insert at CheckIP, but all those values had better be live there.
  // Check the materialized operands directly instead of using a collapsed
  // common insertion point: around invoke/unwind edges, the latter can name the
  // invoke even when the actual operands were created before it.
  assert(([&]() {
    SmallVector<Value *> Operands = {Min, Size, Span, Base};
    if (Check.Mask) {
      Operands.push_back(Check.Mask);
    }
    for (Value *V : Operands) {
      Instruction *I = dyn_cast<Instruction>(V);
      if (!I) {
        continue;
      }
      if (I == CheckIP || !PTA.DT.dominates(I, CheckIP)) {
        return false;
      }
    }
    return true;
  }()) && "check operands must dominate CheckIP");

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

  if (OutOfRange->getType()->isVectorTy()) {
    OutOfRange = IRB.CreateSelect(
        Check.Mask, OutOfRange,
        Constant::getNullValue(OutOfRange->getType()),
        Name + ".active.oor");
    OutOfRange = IRB.CreateOrReduce(OutOfRange);
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

  // Restore on normal return, including calls made into invokes by discovery.
  IRB.SetInsertPoint(PTA.PTU.getFirstIPAfter(I).getIP());

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

  IRB.SetInsertPoint(PTA.PTU.getFirstIPAfter(CB).getIP());
  CallInst *AfterCall = IRB.CreateCall(DS.MakecontextStripAfter, Args);
  AfterCall->setDebugLoc(DL);
  AfterCall->setCallingConv(Callee->getCallingConv());
  AfterCall->setDoesNotThrow();
}

DenseMap<Value *, Value *> PTSanLowerer::lowerSources() {
  DenseMap<Value *, Value *> AllocatedSourceIDs;
  Instruction *FrameSetupEnd =
      &*F.getEntryBlock().getFirstNonPHIOrDbgOrAlloca();

  auto isDynamicAlloca = [&](SourceAnchor Source) {
    auto *AI = dyn_cast<AllocaInst>(Source.getSource());
    return AI && !AI->getAllocationSize(DS.DL);
  };

  // Mark that Source's id is ID
  auto recordSourceID = [&](SourceAnchor Source, Value *ID) {
    for (User *U : Source.Call->users()) {
      if (auto Request = DS.getAsAnchor<GetIDAnchor>(U)) {
        AllocatedSourceIDs[Request->Call] = ID;
      }
    }
  };

  // Populate dynamic and static ptr sources
  auto PtrSrcs = DS.collectAnchors<SourceAnchor>(F);
  SmallVector<SourceAnchor, 8> StaticPtrSrcs;
  SmallVector<SourceAnchor, 8> DynamicPtrSrcs;
  SmallVector<SourceAnchor, 8> RepeatablePtrSrcs;
  for (SourceAnchor Source : PtrSrcs) {
    // if Source has no uses which actually want an ID, don't give it
    // one
    bool NeedsID = llvm::any_of(Source.Call->users(), [&](User *U) {
      return DS.getAsAnchor<GetIDAnchor>(U).has_value();
    });
    if (!NeedsID) {
      continue;
    }

    if (auto *TLS = dyn_cast<IntrinsicInst>(Source.getSource());
        TLS && TLS->getIntrinsicID() == Intrinsic::threadlocal_address) {
      auto *Global = cast<GlobalValue>(TLS->getArgOperand(0));
      PTSanIRB IRB(DS, Source.Call);
      Value *ID = IRB.CreateLoad(DS.U16Ty, DS.TLSIDs.at(Global));
      recordSourceID(Source, IRB.CreateZExt(ID, DS.U64Ty));
      continue;
    }

    if (isDynamicAlloca(Source)) {
      // Repeated allocations need an ID per execution, not per source.
      // Include irreducible cycles, which LoopInfo does not cover.
      auto *AI = cast<AllocaInst>(Source.getSource());
      if (PTA.CI.getCycle(AI->getParent())) {
        RepeatablePtrSrcs.push_back(Source);
      } else {
        DynamicPtrSrcs.push_back(Source);
      }
    } else {
      StaticPtrSrcs.push_back(Source);
    }
  }

  // Gather VA starts
  SmallVector<IntrinsicInst *, 4> VAStarts;
  for (Instruction &I : instructions(F)) {
    auto *II = dyn_cast<IntrinsicInst>(&I);
    if (II && II->getIntrinsicID() == Intrinsic::vastart) {
      VAStarts.push_back(II);
    }
  }
  // Only the Linux SysV x86-64 and AAPCS64 layouts are supported.
  Triple Target(DS.M.getTargetTriple());
  if (VAStarts.size() > 0 &&
      !((Target.getArch() == Triple::x86_64 || Target.isAArch64()) &&
        Target.isOSLinux())) {
    errs() << "PTSan va_list tagging requires x86-64 or AArch64 Linux\n";
    VAStarts.clear();
  }

  // Allocate IDs for all sources. AllocateSourceIDs will also set bounds
  // for all statically-known buffers. Dynamic remain below
  size_t NumPtrSourceIDs = StaticPtrSrcs.size() + DynamicPtrSrcs.size();
  unsigned NumVAListIDs = 0;
  if (!VAStarts.empty()) {
    switch (Target.getArch()) {
    case Triple::x86_64:
      NumVAListIDs = 2;
      break;
    case Triple::aarch64:
    case Triple::aarch64_be:
    case Triple::aarch64_32:
      NumVAListIDs = 3;
      break;
    default:
      llvm_unreachable("Unsupported PTSan va_list architecture");
    }
  }
  size_t NumSourceIDs = NumPtrSourceIDs + NumVAListIDs;
  SmallVector<Value *, 8> IDs = allocateSourceIDs(NumSourceIDs, StaticPtrSrcs);
  assert(IDs.size() == NumSourceIDs);
  for (size_t I = 0; I < StaticPtrSrcs.size(); ++I) {
    recordSourceID(StaticPtrSrcs[I], IDs[I]);
  }

  // Handle dynamic allocs
  for (size_t I = 0; I < DynamicPtrSrcs.size(); ++I) {
    Value *ID = IDs[StaticPtrSrcs.size() + I];
    recordSourceID(DynamicPtrSrcs[I], ID);
    auto *AI = cast<AllocaInst>(DynamicPtrSrcs[I].getSource());
    PTSanIRB IRB(DS, AI->getNextNode());
    Value *Count = IRB.CreateZExtOrTrunc(AI->getArraySize(), DS.U64Ty);
    uint64_t ElementSize =
        DS.DL.getTypeAllocSize(AI->getAllocatedType()).getFixedValue();
    Value *Size = IRB.CreateMul(Count, IRB.getInt64(ElementSize));
    writeSourceBounds(IRB, AI, Size, ID);
  }

  if (!RepeatablePtrSrcs.empty()) {
    PTSanIRB IRB(DS, &*F.getEntryBlock().getFirstInsertionPt());
    RepeatableAllocaIDs = IRB.CreateCall(DS.GetRepeatableAllocaIDsContainer);
  }
  for (SourceAnchor Source : RepeatablePtrSrcs) {
    auto *AI = cast<AllocaInst>(Source.getSource());
    PTSanIRB IRB(DS, AI->getNextNode());
    Value *Count = IRB.CreateZExtOrTrunc(AI->getArraySize(), DS.U64Ty);
    uint64_t ElementSize =
        DS.DL.getTypeAllocSize(AI->getAllocatedType()).getFixedValue();
    Value *Size = IRB.CreateMul(Count, IRB.getInt64(ElementSize));
    Value *ID = IRB.CreateCall(
        DS.GetRepeatableAllocaID, {RepeatableAllocaIDs, AI, Size});
    recordSourceID(Source, IRB.CreateZExt(ID, DS.U64Ty));
  }

  // Log allocated source IDs, including nonrepeating dynamic allocas.
  if (ClValidate && !ClNoIDs && NumPtrSourceIDs != 0) {
    PTSanIRB IRB(DS, &*F.getEntryBlock().getFirstInsertionPt());
    Value *FnNameCStr = IRB.CreateGlobalStringPtr(F.getName(), F.getName());
    auto logSources = [&](ArrayRef<SourceAnchor> Sources, size_t IDOffset) {
      for (size_t I = 0; I < Sources.size(); ++I) {
        IRB.SetInsertPoint(Sources[I].Call);
        Value *ID = IRB.CreateTrunc(IDs[IDOffset + I], DS.U16Ty);
        bool IsLocal = areUsesLocal(Sources[I].Call);
        ConstantInt *IsLocalVal = ConstantInt::get(DS.BoolTy, IsLocal);
        CallInst *Log = IRB.CreateCall(
            DS.LogAllocaInfo, {FnNameCStr, ID, IsLocalVal, HasStackIDRoom});
        for (unsigned Arg : {1, 2, 3})
          Log->addParamAttr(Arg, Attribute::ZExt);
      }
    };
    logSources(StaticPtrSrcs, 0);
    logSources(DynamicPtrSrcs, StaticPtrSrcs.size());
  }

  // Handle VAStart entries
  if (!VAStarts.empty()) {
    // va_start and va_copy instances share backing storage for this invocation.
    // Initialize after each start; retire with the function's other stack IDs.
    switch (Target.getArch()) {
    case Triple::aarch64:
    case Triple::aarch64_be:
    case Triple::aarch64_32: {
      // AAPCS64 has separate GP and FP/SIMD save areas, plus stack arguments.
      // Their sizes are encoded in the initial va_list offsets by the backend.
      for (IntrinsicInst *Start : VAStarts) {
        PTSanIRB IRB(DS, Start->getNextNode());
        IRB.CreateCall(DS.InitVAListAArch64,
                      {Start->getArgOperand(0), IDs[NumPtrSourceIDs],
                       IDs[NumPtrSourceIDs + 1], IDs[NumPtrSourceIDs + 2]});
      }
      break;
    }
    case Triple::x86_64: {
      Value *RegisterID = IDs[NumPtrSourceIDs];
      Value *OverflowID = IDs[NumPtrSourceIDs + 1];
      // SysV x86-64 saves six 8-byte GP argument registers (RDI, RSI, RDX, RCX,
      // R8, R9) and eight 16-byte XMM argument registers (XMM0-XMM7).
      constexpr unsigned GPRegisterSaveBytes = 6 * 8;
      constexpr unsigned XMMRegisterSaveBytes = 8 * 16;
      // This is a per-function constant: disabling SSE removes the XMM save
      // slots. The shared runtime cannot infer the caller's target features.
      unsigned RegisterSize = GPRegisterSaveBytes + XMMRegisterSaveBytes;
      SmallVector<StringRef, 16> Features;
      F.getFnAttribute("target-features").getValueAsString().split(Features, ',');
      if (is_contained(Features, "-sse")) {
        RegisterSize = GPRegisterSaveBytes;
      }
      for (IntrinsicInst *Start : VAStarts) {
        PTSanIRB IRB(DS, Start->getNextNode());
        IRB.CreateCall(DS.InitVAListX86_64,
                       {Start->getArgOperand(0), RegisterID, OverflowID,
                        IRB.getInt64(RegisterSize)});
      }
      break;
    }
    default:
      llvm_unreachable("Unsupported PTSan va_list architecture");
    }
  }

  lowerSetjmps(FrameSetupEnd);

  for (SourceAnchor Src : PtrSrcs) {
    Src.Call->replaceAllUsesWith(Src.getSource());
    Src.erase();
  }
  return AllocatedSourceIDs;
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

  // Preheader markers are needed through check lowering because they are the
  // selected insertion points for loop prechecks.
  for (auto Marker : DS.collectAnchors<LoopPreheaderAnchor>(F)) {
    Marker.erase();
  }

  // Srcs after sinks so the sinks see any writes
  // Keep source-backed GetID requests alive until queued range checks have
  // consumed their raw Value pointers. Resolve their IR uses at the end.
  DenseMap<Value *, Value *> AllocatedSourceIDs = lowerSources();

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
    Value *ResolvedID;
    if (AllocatedSourceIDs.contains(ID.Call)) {
      ResolvedID = AllocatedSourceIDs.at(ID.Call);
    } else {
      ResolvedID = lowerGetID(ID);
    }
    ID.Call->replaceAllUsesWith(ResolvedID);
    ID.erase();
  }
  // Add the ABI-only operand after discovery and boundary rewriting, so it
  // does not affect instrumentation of the original call's arguments.
  appendVarargSentinels();
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

  if (CI.Min->getType()->isVectorTy()) {
    IRB.CreateCall(DS.ReportVectorError, {CheckID, FunctionID});
    return;
  } else {
    std::string Desc = ChkTyToString(CI.Type);
    llvm::Value *DescPtr = IRB.CreateGlobalStringPtr(Desc);

    IRB.CreateCall(DS.ReportGenericError,
      {CheckID, FunctionID, CI.PtrID, DescPtr, CI.Min, CI.Size});
  }
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

  const DataLayout &DL = F.getDataLayout();
  bool IsScalarPointer = V->getType()->isPointerTy() &&
                         DL.getPointerTypeSizeInBits(V->getType()) == 64 &&
                         !DL.isNonIntegralPointerType(V->getType());
  if (UseInlineStrip && (IsScalarPointer || V->getType()->isIntegerTy(64))) {
    // Hide only this PTSan mask from BEXTR/BZHI selection. Keep the mask as an IR
    // operand so the backend can share its materialization between strips.
    auto *FT = FunctionType::get(DS.U64Ty, {DS.U64Ty, DS.U64Ty}, false);
    auto *Asm = InlineAsm::get(FT, "andq $2, $0", "=r,0,r,~{flags}", false);
    Value *Bits = IsScalarPointer ? IRB.CreatePtrToInt(V, DS.U64Ty) : V;
    CallInst *Raw = IRB.CreateCall(Asm, {Bits, DS.AddrMask},
                                  V->getName() + ".true.bits");
    Raw->setDoesNotAccessMemory();
    Raw->setDoesNotThrow();
    Raw->addFnAttr(Attribute::WillReturn);
    return IsScalarPointer
               ? IRB.CreateIntToPtr(Raw, V->getType(), V->getName() + ".true")
               : Raw;
  }

  if (Strip.isIntegerStrip()) {
    return IRB.CreateAnd(V, DS.AddrMask, V->getName() + ".true");
  }

  assert(Strip.isPointerStrip() && "expected pointer or integer strip anchor");

  Type *MaskTy = V->getType()->getWithNewType(DS.U64Ty);
  Value *Mask = PTA.PTU.splatToMatchType(DS.AddrMask, MaskTy);

  Function *PMFn = Intrinsic::getDeclaration(F.getParent(), Intrinsic::ptrmask,
                                             {V->getType(), Mask->getType()});
  return IRB.CreateCall(PMFn, {V, Mask}, V->getName() + ".true");
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

  assert(V->getType()->isPtrOrPtrVectorTy() &&
         "tag lowering expects pointer or pointer vector");
  Type *IntTy = V->getType()->getWithNewType(DS.U64Ty);

  Value *ValueBits = IRB.CreatePtrToInt(V, IntTy, V->getName() + ".asint");
  Value *ID = PTA.PTU.splatToMatchType(Tag.getID(), IntTy);
  Value *TagBits = IRB.CreateShl(ID, ConstantInt::get(ID->getType(), 48),
                                 V->getName() + ".tagbits");
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
