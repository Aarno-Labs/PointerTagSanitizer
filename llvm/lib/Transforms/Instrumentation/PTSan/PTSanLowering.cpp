//===-- PTSanLowering.cpp - PTSan lowering phase -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"
#include "llvm/ADT/PostOrderIterator.h"
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
  PTSanPolarityReader Polarities;
  bool UseInlineStrip;

  uint32_t NextCheckID;

  // Number of allocated stack IDs to retire at each function exit.
  size_t NumAllocatedSourceIDs;

  // These values are actually added to the function
  // and must be referenced across calls

  // Slow-path IDs retained until function exit.
  AllocaInst *IDsAlloc;

  Value *HasStackIDRoom;
  Value *StartingStackID;
  Value *StackIDCursor = nullptr;
  Value *FrameSalt = nullptr;
  bool UseFastStackIDs = !isStackIDFastPathDisabled();
  bool UseFrameSalt = false;
  bool HasRepeatableAllocas = false;
  SmallVector<Value *, 3> VAListIDs;

  ConstantInt *FunctionID;

  // Lowering entry points
  void prepareSources();
  // Allocate NumSourceIDs IDs and initialize bounds for the supplied sources.
  // Their IDs come first; the remaining IDs have bounds initialized later.
  SmallVector<Value *, 8> allocateSourceIDs(size_t NumSourceIDs,
                                         ArrayRef<SourceAnchor> PtrSrcs);
  void writeSourceBounds(PTSanIRB &IRB, Value *Source, Value *Size, Value *ID);
  uint64_t getSourceOriginQuantum(Value *Source);
  Value *takeFrameSalt(PTSanIRB &IRB);
  Value *materializeAsInt(PTSanIRB &IRB, Value *V, const Twine &Name = "");
  Value *lowerGetID(GetIDAnchor IDAnchor);
  Value *lowerBound(BoundAnchor Bounds);
  Value *lowerID16Strip(ID16StripAnchor Strip);
  Value *lowerID32Strip(ID32StripAnchor Strip);
  Value *lowerTagged(TaggedAnchor Tagged);
  Value *lowerID16Tag(ID16TagAnchor Tag);
  Value *lowerID32Tag(ID32TagAnchor Tag);
  optional<RangeCheckInfo> addPtrCheck(const PlaceablePtrCheck &Op);
  void lowerFunctionExit(Instruction *I);
  void releaseFastStackIDs(PTSanIRB &IRB);
  void lowerSetjmp(CallBase *CB);
  void lowerVAStart(IntrinsicInst *Start);
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
    : F(F), DS(DS), PTA(PTA), Polarities(DS, F),
      UseInlineStrip(DS.shouldUseInlineStrip()),
      NextCheckID(0) {
  FunctionID = ConstantInt::get(DS.U64Ty, getFunctionGUID(F));
}

bool PTSanLoweringPass::run(PTSanAnalysisState &PTA,
                           FunctionAnalysisManager &FAM) {
  Function &F = PTA.F;
  PointerTagSanitizer &DS = PTA.DS;

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
  // No cursor means source lowering did not allocate any stack IDs.
  if (!StackIDCursor) {
    return;
  }

  PTSanIRB IRB(DS, I);
  if (NumAllocatedSourceIDs == 0 || !UseFastStackIDs) {
    IRB.CreateCall(DS.FreeStackIDs, {StackIDCursor});
    return;
  }

  MDBuilder MDB(DS.Ctx);
  MDNode *BW = MDB.createBranchWeights(/*true*/1000, /*false*/1);
  Instruction *FastPath = nullptr, *SlowPath = nullptr;
  SplitBlockAndInsertIfThenElse(HasStackIDRoom, IRB.GetInsertPoint(),
                                &FastPath, &SlowPath,
                                BW, &PTA.DTU, &PTA.LI);

  // Fast fixed IDs use their bucket; repeatable allocas still use the log.
  IRB.SetInsertPoint(FastPath);
  if (HasRepeatableAllocas) {
    IRB.CreateCall(DS.FreeStackIDs, {StackIDCursor});
  }
  releaseFastStackIDs(IRB);

  // Slow fixed IDs and repeatable allocas share one cursor and one cleanup.
  IRB.SetInsertPoint(SlowPath);
  IRB.CreateCall(DS.FreeStackIDs, {StackIDCursor});
}

void PTSanLowerer::releaseFastStackIDs(PTSanIRB &IRB) {
  if (!ClStackTemporalSafety) {
    IRB.CreateStore(StartingStackID, DS.NextStackIDPtr);
    return;
  }

  if (Cl32BitIDs) {
    // Keep the decode bias and origin: stale pointers must still decode to
    // their original address until reuse. A fresh salt protects the next frame,
    // so ID32 does not need ID16's delayed-reuse batching.
    for (size_t I = 0; I < NumAllocatedSourceIDs; ++I) {
      Value *ID = IRB.CreateAdd(StartingStackID, IRB.getInt64(I));
      IRB.CreateStore(IRB.getInt64(0), DS.getBoundsPtr(IRB, ID, 1));
    }
    IRB.CreateStore(StartingStackID, DS.NextStackIDPtr);
    return;
  } else {
    assert(DS.PendingStackIdsPtr &&
           "pending stack IDs only exist with stack temporal safety");
    Value *PendingIDs16 =
      IRB.CreateLoad(DS.IDTy, DS.PendingStackIdsPtr, "pending.frees");
    ConstantInt *Enough = ConstantInt::get(DS.IDTy, 64);
    Value *PendingIdsInc16 = IRB.CreateAdd(
        PendingIDs16, ConstantInt::get(DS.IDTy, NumAllocatedSourceIDs),
        "pending.frees.inc");
    Value *EnoughPendingIDs = IRB.CreateICmpUGE(PendingIdsInc16, Enough, "enough.pending.ids");
    Instruction *AfterSplit = &*IRB.GetInsertPoint();
    Instruction *EnoughPendingIDsPath, *NotEnoughPendingIDsPath;
    SplitBlockAndInsertIfThenElse(EnoughPendingIDs, AfterSplit,
                                  &EnoughPendingIDsPath, &NotEnoughPendingIDsPath,
                                  nullptr, &PTA.DTU, &PTA.LI);

    // If we've got a big enough chunk, free ids:
    IRB.SetInsertPoint(EnoughPendingIDsPath);
    IRB.CreateStore(StartingStackID, DS.NextStackIDPtr);
    IRB.CreateStore(ConstantInt::get(DS.IDTy, 0), DS.PendingStackIdsPtr);

    // If we don't have a big enough chunk, just add to pending
    IRB.SetInsertPoint(NotEnoughPendingIDsPath);
    IRB.CreateStore(PendingIdsInc16, DS.PendingStackIdsPtr);

    // Regardless (in the fast path), free ids
    IRB.SetInsertPoint(AfterSplit);

    // First ID in the contiguous run is StartingStackID.
    Value *ID0_64 = StartingStackID;

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
  }
}

// These hooks surround direct libc calls, not uses of a returned pointer.
// callbr is inline asm and cannot call any of the hooked functions.
static Instruction *getNormalReturnIP(CallBase *Call) {
  if (auto *Invoke = dyn_cast<InvokeInst>(Call)) {
    return &*Invoke->getNormalDest()->getFirstInsertionPt();
  }
  return cast<CallInst>(Call)->getNextNode();
}

// Snapshot immediately before each setjmp, including each loop iteration.
// A nonzero return retires allocations made since that particular snapshot.
void PTSanLowerer::lowerSetjmp(CallBase *CB) {
  // XXX: We should key the cursor off of the actual saved environment, and
  // not the setjmp; see setjmp_same_site_xfail.c
  PTSanIRB IRB(DS, CB);
  Value *Cursor = IRB.CreateLoad(DS.U64Ty, DS.StackIDCursorPtr,
                                "saved.stack.cursor");
  Value *NextID = IRB.CreateLoad(DS.U64Ty, DS.NextStackIDPtr, "saved.stack.id");
  Value *PendingIDs = IRB.CreateLoad(DS.IDTy, DS.PendingStackIdsPtr,
                                     "saved.pending.ids");
  Value *InSignalContext = IRB.CreateLoad(
      DS.U8Ty, DS.InSignalContextPtr, "saved.signal.context");
  // Test the result immediately after the call (or on an invoke's normal
  // edge). Initial setjmp returns zero; returning through longjmp does not.
  IRB.SetInsertPoint(getNormalReturnIP(CB));
  Value *ReturnedAgain = IRB.CreateICmpNE(
      CB, ConstantInt::get(CB->getType(), 0), "setjmp.returned.again");
  // Put cleanup in the nonzero branch, updating dominator and loop analyses.
  // Restore is that branch's terminator, so insert cleanup before it.
  Instruction *Restore = SplitBlockAndInsertIfThen(
      ReturnedAgain, IRB.GetInsertPoint(), false, nullptr, &PTA.DTU, &PTA.LI);
  IRB.SetInsertPoint(Restore);
  IRB.CreateCall(DS.RestoreStackIDs,
                 {Cursor, NextID, PendingIDs, InSignalContext});
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
      if (!AI) {
        continue;
      }

      // Only hoist constant-size allocas in addrspace(0)
      if (!isa<ConstantInt>(AI->getArraySize())) {
        continue;
      }
      if (AI->getAddressSpace() != 0) {
        continue;
      }
      // Don't hoist the first instruciton or ??? will happen
      if (AI == InsertBefore) {
        continue;
      }

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

      if (isa<CallBase>(U) || isa<ReturnInst>(U)) {
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

uint64_t PTSanLowerer::getSourceOriginQuantum(Value *Source) {
  // Preserve the source's existing alignment without changing its layout.
  uint64_t Alignment = 1;
  if (auto *AI = dyn_cast<AllocaInst>(Source)) {
    Alignment = std::max(Alignment, AI->getAlign().value());
  } else if (auto *Arg = dyn_cast<Argument>(Source)) {
    // We handle ByVal args
    if (Arg->hasByValAttr()) {
      Alignment = std::max(
          Alignment, DS.DL
                         .getValueOrABITypeAlignment(Arg->getParamAlign(),
                                                     Arg->getParamByValType())
                         .value());
    } else {
      // This is only relevant for argv, but we do have to handle it
      Alignment =
          std::max(Alignment, Arg->getParamAlign().valueOrOne().value());
    }
  }
  if (Alignment > (1ULL << 31)) {
    report_fatal_error(
        "PTSan 32-bit IDs cannot preserve requested alignment above 2 GiB");
  }
  uint64_t Quantum = std::max<uint64_t>(ClID32OriginQuantum, Alignment);
  if (ClStackTemporalSafety && Quantum >= (1ULL << 31))
    report_fatal_error(
        "PTSan 32-bit stack origins require alignment below 2 GiB");
  return Quantum;
}

Value *PTSanLowerer::takeFrameSalt(PTSanIRB &IRB) {
  Instruction *Continue = &*IRB.GetInsertPoint();
  Value *Next =
      IRB.CreateLoad(DS.PtrTy, DS.StackOriginNextPtr, "frame.origin.next");
  Value *Salt = IRB.CreateLoad(DS.U32Ty, Next, "frame.salt.cached");
  // Cached salts are nonzero multiples of 4096; zero marks the end.
  Value *Usable = IRB.CreateICmpNE(Salt, IRB.getInt32(0));
  Instruction *Consume, *Refill;
  SplitBlockAndInsertIfThenElse(
      Usable, Continue->getIterator(), &Consume, &Refill,
      MDBuilder(DS.Ctx).createBranchWeights(1000, 1), &PTA.DTU, &PTA.LI);
  IRB.SetInsertPoint(Consume);
  IRB.CreateStore(IRB.CreateInBoundsGEP(DS.U32Ty, Next, IRB.getInt64(1)),
                  DS.StackOriginNextPtr);

  IRB.SetInsertPoint(Refill);
  Value *Refilled = IRB.CreateCall(DS.RefillStackOrigins);
  IRB.SetInsertPoint(Continue);
  PHINode *Selected = IRB.CreatePHI(DS.U32Ty, 2, "frame.salt");
  Selected->addIncoming(Salt, Consume->getParent());
  Selected->addIncoming(Refilled, Refill->getParent());
  return Selected;
}

void PTSanLowerer::writeSourceBounds(PTSanIRB &IRB, Value *Source, Value *Size,
                                   Value *ID) {
  Value *Base = IRB.CreatePtrToInt(Source, DS.U64Ty);
  if (Cl32BitIDs) {
    uint64_t Quantum = getSourceOriginQuantum(Source);
    // Non-repeating VLAs and main's argv also reach this writer.
    auto *ConstantSize = dyn_cast<ConstantInt>(Size);
    if (ConstantSize && ConstantSize->getZExtValue() >= (1ULL << 31)) {
      report_fatal_error("PTSan 32-bit IDs cannot represent objects of 2 GiB or larger");
    }
    if (!ConstantSize) {
      Value *DoesNotFit = IRB.CreateICmpUGE(Size, IRB.getInt64(1ULL << 31));
      Instruction *Continue = &*IRB.GetInsertPoint();
      Instruction *ThenTerm = SplitBlockAndInsertIfThen(
          DoesNotFit, Continue->getIterator(), true,
          MDBuilder(DS.Ctx).createBranchWeights(1, 1000), &PTA.DTU, &PTA.LI);
      PTSanIRB FailureIRB(DS, ThenTerm);
      FailureIRB.CreateCall(Intrinsic::getDeclaration(&DS.M, Intrinsic::trap));
      IRB.SetInsertPoint(Continue);
    }
    Value *Salt;
    if (FrameSalt) {
      Salt = IRB.CreateZExt(FrameSalt, DS.U64Ty);
    } else if (ClStackTemporalSafety || !ConstantSize) {
      Salt = IRB.CreateZExt(
          IRB.CreateCall(DS.ChooseStackOrigin, {IRB.getInt64(Quantum)}), DS.U64Ty);
    } else {
      Salt = IRB.getInt64(Quantum & 0x7fffffff);
    }
    // Quantum is a power of two: this mask computes raw_base % Quantum.
    Value *Remainder = IRB.CreateAnd(Base, IRB.getInt64(Quantum - 1));
    Value *Origin =
        IRB.CreateTrunc(IRB.CreateAdd(Salt, Remainder), DS.U32Ty);
    Value *OriginSlot = DS.getOriginPtr(IRB, ID);
    IRB.CreateStore(Origin, OriginSlot);
    Base = IRB.CreateSub(Base, IRB.CreateZExt(Origin, DS.U64Ty));
  } else {
    // Default-mode metadata and static bounds both retain the tagged base.
    Base = IRB.CreateOr(Base, IRB.CreateShl(ID, 48));
  }
  Value *Bounds = PoisonValue::get(DS.BoundsStructTy);
  Bounds = IRB.CreateInsertValue(Bounds, Base, {0});
  Bounds = IRB.CreateInsertValue(Bounds, Size, {1});
  Value *Slot = DS.getBoundsPtr(IRB, ID);
  // Only non-strict ID16 can fall back to a shared default ID on exhaustion.
  // Redirect these writes so they do not replace that ID's permissive bounds.
  if (DS.DefaultIDNo) {
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
    if (HasRepeatableAllocas) {
      PTSanIRB IRB(DS, &*F.getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
      StackIDCursor = IRB.CreateLoad(DS.U64Ty, DS.StackIDCursorPtr);
    }
    return {};
  }

  PTSanIRB IRB(DS, &*F.getEntryBlock().getFirstNonPHIOrDbgOrAlloca());
  markIRAsFrom(IRB, "ptsan.ir.lowerPtrSources");
  auto GetSpan = [&](unsigned i) -> Value * {
    Value *Source = PtrSrcs[i].getSource();
    if (auto *AI = dyn_cast<AllocaInst>(Source)) {
      auto Size = AI->getAllocationSize(DS.DL);
      assert(Size && !Size->isScalable() &&
             "PTSan source alloca should have a fixed size");
      return IRB.getInt64(Size->getFixedValue());
    }

    if (F.getName() == "main" && F.arg_size() >= 2 &&
        Source == F.getArg(1)) {
      Value *ArgC = IRB.CreateZExt(F.getArg(0), DS.U64Ty);
      Value *Count = IRB.CreateAdd(ArgC, IRB.getInt64(1));
      return IRB.CreateMul(Count, IRB.getInt64(DS.DL.getPointerSize()));
    }

    if (auto *Arg = dyn_cast<Argument>(Source)) {
      assert(Arg->hasByValAttr() &&
             "non-main PTSan argument source must be passed by value");
      TypeSize Size =
          DS.DL.getTypeAllocSize(Arg->getParamByValType());
      assert(!Size.isScalable() &&
             "PTSan does not support scalable pass-by-value objects");
      return IRB.getInt64(Size.getFixedValue());
    }

    errs() << "Unknown PTSan source anchor operand: " << *Source << "\n";
    die();
  };

  auto GetFastCaseIDs = [&](PTSanIRB &IRB, Value *RequestedStackID) {
    SmallVector<Value *, 8> IDs;
    IDs.resize(NumSourceIDs);
    Value *FirstID = IRB.CreateTrunc(StartingStackID, DS.IDTy);

    for (unsigned i = 0; i < NumSourceIDs; i++) {
      Value *ID = (i == 0)
          ? FirstID
          : IRB.CreateAdd(FirstID, ConstantInt::get(DS.IDTy, i), "next.plus");
      IDs[i] = ID;
    }

    //ptsan_next_stack_id += N;
    IRB.CreateStore(RequestedStackID, DS.NextStackIDPtr);

    return IDs;
  };

  auto WriteBounds = [&](const SmallVector<Value *, 8> &IDs) {
    for (size_t i = 0; i < PtrSrcs.size(); i++) {
      Value *Source = PtrSrcs[i].getSource();
      Value *Span64 = GetSpan(i);
      Value *ID_64 = IRB.CreateZExt(IDs[i], DS.U64Ty);
      writeSourceBounds(IRB, Source, Span64, ID_64);
    }
  };

  auto WidenIDs = [&](SmallVector<Value *, 8> IDs) {
    for (Value *&ID : IDs) {
      ID = IRB.CreateZExt(ID, DS.U64Ty);
    }
    return IDs;
  };

  auto GetGenericIDs = [&]() {
    StackIDCursor = IRB.CreateLoad(DS.U64Ty, DS.StackIDCursorPtr);
    IRB.CreateCall(DS.GetStackIDs, {IDsAlloc, IRB.getInt64(NumSourceIDs)});
    SmallVector<Value *, 8> IDs;
    IDs.resize(NumSourceIDs);
    for (unsigned i = 0; i < NumSourceIDs; i++) {
      Value *IDPtr = IRB.CreateInBoundsGEP(
          DS.IDTy, IDsAlloc, {IRB.getInt64(i)});
      IDs[i] = IRB.CreateLoad(DS.IDTy, IDPtr);
    }
    return IDs;
  };

  hoistAllocas(F);
  IDsAlloc = IRB.CreateAlloca(DS.IDTy, IRB.getInt64(NumSourceIDs), "stack.ids");

  if (!UseFastStackIDs) {
    HasStackIDRoom = ConstantInt::getFalse(DS.BoolTy);
    SmallVector<Value *, 8> IDs = GetGenericIDs();
    WriteBounds(IDs);
    return WidenIDs(std::move(IDs));
  }

  StartingStackID = IRB.CreateLoad(DS.U64Ty, DS.NextStackIDPtr);
  Value *RequestedStackID = IRB.CreateAdd(
      StartingStackID, IRB.getInt64(NumSourceIDs));
  Value *StackIDLimit = IRB.CreateLoad(DS.U64Ty, DS.StackIDLimitPtr);
  HasStackIDRoom = IRB.CreateICmpULE(RequestedStackID, StackIDLimit, "has.stack.id.room");

  MDBuilder MDB(DS.Ctx);
  MDNode *BW = MDB.createBranchWeights(/*true*/1000, /*false*/1);
  Instruction *ThenTerm = nullptr, *ElseTerm = nullptr;
  SplitBlockAndInsertIfThenElse(HasStackIDRoom, IRB.GetInsertPoint(),
                                &ThenTerm, &ElseTerm,
                                BW, &PTA.DTU, &PTA.LI);

  // The fast path allocates a contiguous range without calling the runtime.
  IRB.SetInsertPoint(ThenTerm);
  SmallVector<Value *, 8> ThenIDs = GetFastCaseIDs(IRB, RequestedStackID);
  Value *FastSalt = UseFrameSalt ? takeFrameSalt(IRB) : nullptr;
  // Only repeatable allocas need a log checkpoint on the fast path.
  // Fixed-only fast frames never use this cursor for cleanup, so use a
  // constant to avoid a TLS load.
  Value *FastCursor = HasRepeatableAllocas
                          ? IRB.CreateLoad(DS.U64Ty, DS.StackIDCursorPtr)
                          : (Value *)IRB.getInt64(0);

  IRB.SetInsertPoint(ElseTerm);
  SmallVector<Value *, 8> ElseIDs = GetGenericIDs();
  Value *SlowSalt = UseFrameSalt
                        ? IRB.CreateCall(DS.ChooseStackOrigin, {IRB.getInt64(4096)})
                        : nullptr;

  // Merge fast- and slow-path IDs before initializing bounds.
  BasicBlock *ContBB = ThenTerm->getParent()->getUniqueSuccessor();
  assert(ContBB);
  IRB.SetInsertPoint(&*ContBB->getFirstNonPHI());
  PHINode *CursorPhi = IRB.CreatePHI(DS.U64Ty, 2, "stack.cursor");
  CursorPhi->addIncoming(FastCursor, ThenTerm->getParent());
  CursorPhi->addIncoming(StackIDCursor, ElseTerm->getParent());
  StackIDCursor = CursorPhi;
  if (UseFrameSalt) {
    PHINode *SaltPhi = IRB.CreatePHI(DS.U32Ty, 2, "stack.frame.salt");
    SaltPhi->addIncoming(FastSalt, ThenTerm->getParent());
    SaltPhi->addIncoming(SlowSalt, ElseTerm->getParent());
    FrameSalt = SaltPhi;
  }
  SmallVector<Value *, 8> StackIDPHIs;
  StackIDPHIs.resize(NumSourceIDs);

  for (unsigned i = 0; i < NumSourceIDs; i++) {
    PHINode *Phi = IRB.CreatePHI(DS.IDTy, /*NumReservedValues=*/2,
                                      Twine("ID_") + Twine(i));
    Phi->addIncoming(ThenIDs[i], ThenTerm->getParent());
    Phi->addIncoming(ElseIDs[i], ElseTerm->getParent());
    StackIDPHIs[i] = Phi;
  }

  IRB.SetInsertPoint(&*ContBB->getFirstInsertionPt());
  WriteBounds(StackIDPHIs);
  return WidenIDs(std::move(StackIDPHIs));
}

// SysV x86-64 and AAPCS64 pass IR byval arguments on the stack even when
// registers remain. This is an LLVM lowering rule, not C aggregate placement.
// The runtime owns the marker contents, shared by the producer and scanner.
void PTSanLowerer::appendVarargSentinels() {
  Module &M = *PTA.F.getParent();

  // Append a byval sentinel after the stack arguments. At va_start, the runtime
  // scans to this marker to determine the size without duplicating ABI argument
  // placement rules.
  for (VarargCallAnchor Anchor : DS.collectAnchors<VarargCallAnchor>(F)) {
    auto *CB = cast<CallBase>(Anchor.getTarget());
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
    Anchor.erase();
  }
}

// Convert pointers to integer bits, preserving vector shape; leave integers alone.
Value *PTSanLowerer::materializeAsInt(PTSanIRB &IRB, Value *V,
                                    const Twine &Name) {
  if (V->getType()->isPtrOrPtrVectorTy()) {
    Type *IntTy = V->getType()->getWithNewType(DS.U64Ty);
    return IRB.CreatePtrToInt(V, IntTy, Name);
  }
  return V;
}

// Conditional ID32 pseudo-IR (bits are u64; vectors work lane-wise):
//   candidate = bits >> 32
//   encoded = signed(bits) < -1
//   id = select(encoded, candidate, 0)
// The signed comparison tests the marker bit but excludes the all-ones sentinel.
// Unconditional strict decoding needs only: id = bits >> 32.
// ID16 needs only: id = bits >> 48.
Value *PTSanLowerer::lowerGetID(GetIDAnchor IDAnchor) {
  Value *Ptr = IDAnchor.getBase();
  Type *IDTy = Ptr->getType()->getWithNewType(DS.U64Ty);

  PTSanIRB IRB(DS, IDAnchor.Call);
  markIRAsFrom(IRB, "ptsan.ir.getid");
  Value *BaseInt = materializeAsInt(IRB, Ptr, Ptr->getName() + ".asint");
  if (Cl32BitIDs) {
    if (!usesConditionalID32Decode()) {
      return IRB.CreateLShr(BaseInt, 32, Ptr->getName() + ".id");
    }
    Value *ID = IRB.CreateLShr(BaseInt, 32);
    Value *IsEnriched = IRB.CreateICmpSLT(BaseInt, ConstantInt::getSigned(IDTy, -1));
    return IRB.CreateSelect(IsEnriched, ID, ConstantInt::get(IDTy, 0),
                          Ptr->getName() + ".id");
  } else {
    return IRB.CreateLShr(BaseInt, ConstantInt::get(IDTy, 48),
                          Ptr->getName() + ".id");
  }
}

Value *PTSanLowerer::lowerBound(BoundAnchor Bounds) {
  if (auto LoadBounds = DS.getAsAnchor<LoadBoundAnchor>(Bounds.Call)) {
    Value *ID = LoadBounds->getID();
    unsigned FieldIndex = Bounds.isBase() ? 0 : 1;

    PTSanIRB IRB(DS, Bounds.Call);
    markIRAsFrom(IRB, "ptsan.ir.loadbounds");

    Value *GEP;
    if (Bounds.isOrigin()) {
      GEP = DS.getOriginPtr(IRB, ID);
    } else {
      GEP = DS.getBoundsPtr(IRB, ID, FieldIndex);
    }
    if (auto *GEPInst = dyn_cast<Instruction>(GEP)) {
      GEPInst->setName(ID->getName() +
                       (Bounds.isOrigin() ? ".originGep"
                                         : Bounds.isBase() ? ".baseGep" : ".spanGep"));
    }
    Type *ResultTy = Bounds.Call->getType();

    if (LoadBounds->isVector()) {
      return IRB.CreateMaskedGather(
          ResultTy, GEP, Align(Bounds.isOrigin() ? 4 : 8), LoadBounds->getMask(),
          Constant::getNullValue(ResultTy),
          ID->getName() + (Bounds.isBase() ? ".base" : ".span"));
    }

    return IRB.CreateLoad(ResultTy, GEP,
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
    if (Cl32BitIDs) {
      Lowered = IRB.CreateSub(
          Lowered, IRB.CreateZExt(StaticBounds.getOrigin(), DS.U64Ty),
          "ptsan.static.decode.bias");
    }
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
  optional<LoadBoundAnchor> OriginBound;
  if (Cl32BitIDs) {
    OriginBound = Check.Anchor.getOrigin();
  }
  assert(Check.Anchor.hasMaterializedRange() &&
         "check range metadata should be materialized before lowering");
  assert(BaseBound.isBase() && SpanBound.isSpan() &&
         "check should carry base/span bounds");
  assert((!Cl32BitIDs || OriginBound->isOrigin()) &&
         "ID32 check should carry an origin");
  Value *Base = BaseBound.Call;
  Value *Span = SpanBound.Call;

  DBG(
    assert(PTA.DT.verify());
    assert(PTA.PDT.verify());
    PTA.LI.verify(PTA.DT);
  );

  Instruction *CheckIP = Check.Anchor.Call;

  DBG(errs() << "CheckIP: " << *CheckIP << "\n");
  DBG(errs() << "Base bounds IP: " << *BaseBound.getTarget().getIP() << "\n");
  DBG(errs() << "Span bounds IP: " << *SpanBound.getTarget().getIP() << "\n");
  if (OriginBound) {
    DBG(errs() << "Origin IP: " << *OriginBound->getTarget().getIP() << "\n");
  }

  DBG(errs() << "Instigating instructions are:\n");
  for (Use *U : Check.InstigatingUses) {
    Instruction *I = cast<Instruction>(U->getUser());
    DBG(errs() << "\t" << *I << "\n");
    assert(I == CheckIP || PTA.DT.dominates(CheckIP, I));
    assert(PTA.PTU.hoistTargetDominates(BaseBound.getTarget(), CheckIP));
    assert(PTA.PTU.hoistTargetDominates(BaseBound.getTarget(), I));
    assert(PTA.PTU.hoistTargetDominates(SpanBound.getTarget(), CheckIP));
    assert(PTA.PTU.hoistTargetDominates(SpanBound.getTarget(), I));
    if (OriginBound) {
      assert(PTA.PTU.hoistTargetDominates(OriginBound->getTarget(), CheckIP));
      assert(PTA.PTU.hoistTargetDominates(OriginBound->getTarget(), I));
    }
  }

  Value *MinPtr = Check.Anchor.getRangeMinPtr();
  Value *Size = Check.Anchor.getRangeSize();

  // Checks are lowered before pointer conversions erase their solved polarity.
  bool MinIsStripped =
      Polarities.getPolarity(MinPtr) == Polarity::Stripped;

  // Range materialization may recover a callbr value from a block-local PHI.
  // Cast at its checked use, not after a definition with multiple successors.
  PTSanIRB IRB(DS, CheckIP);
  markIRAsFrom(IRB, "ptsan.ir.ptrcheck.preamble");
  Type *CheckIntTy = MinPtr->getType()->getWithNewType(DS.U64Ty);
  Value *MinBits = IRB.CreatePtrToInt(MinPtr, CheckIntTy);

  DBG(errs() << "Min value " << *MinBits << "\n");
  DBG(errs() << "Size value " << *Size << "\n");

  if (Check.Mask) {
    Base = PTA.PTU.splatToMatchType(Base, CheckIntTy, CheckIP);
    Span = PTA.PTU.splatToMatchType(Span, CheckIntTy, CheckIP);
  }

  // Offset into the buffer
  Value *Offset;
  if (Cl32BitIDs) {
    Value *Origin = IRB.CreateZExt(
        OriginBound->Call, PtrID->getType()->getWithNewType(DS.U64Ty));
    Origin = PTA.PTU.splatToMatchType(Origin, CheckIntTy, CheckIP);

    if (MinIsStripped) {
      Value *RawBase = IRB.CreateAdd(Base, Origin, Name + ".raw.base");
      Offset = IRB.CreateSub(MinBits, RawBase, Name + ".offset");
    } else {
      Type *PayloadTy = CheckIntTy->getWithNewType(DS.U32Ty);
      Value *Payload =
          IRB.CreateZExt(IRB.CreateTrunc(MinBits, PayloadTy), CheckIntTy);
      Offset = IRB.CreateSub(Payload, Origin);
      // Tagged polarity can still permit raw pointers under this decode policy.
      if (usesConditionalID32Decode()) {
        Value *IsEnriched =
            IRB.CreateICmpSLT(MinBits, ConstantInt::getSigned(CheckIntTy, -1));
        Value *RawBase = IRB.CreateAdd(Base, Origin, Name + ".raw.base");
        Value *RawOffset = IRB.CreateSub(MinBits, RawBase);
        Offset = IRB.CreateSelect(IsEnriched, Offset, RawOffset, Name + ".offset");
      }
    }
  } else {
    Value *CheckBase = Base;
    if (MinIsStripped) {
      CheckBase = IRB.CreateAnd(
          Base, PTA.PTU.splatToMatchType(DS.AddrMask, CheckIntTy, CheckIP));
    }
    Offset = IRB.CreateSub(MinBits, CheckBase, Name + ".offset");
  }

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

  // The runtime permits zero-byte accesses, even when the pointer is invalid.
  OutOfRange = IRB.CreateAnd(
      OutOfRange, IRB.CreateICmpNE(Size, Constant::getNullValue(Size->getType())));

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
  }
  assert(isa<Instruction>(OORInst));
  return RangeCheckInfo{{OORInst, AFTER},
                        Check.CheckTy,
                        CheckID,
                        PtrID,
                        OORInst,
                        MinBits,
                        Size};
}


void PTSanLowerer::lowerLibcCall(Instruction *I) {
  PTSanIRB IRB(DS, I);

  LoadInst *Old = IRB.CreateLoad(DS.U32Ty, DS.PTSanInLibc);
  Value *Inc = IRB.CreateAdd(Old, ConstantInt::get(DS.U32Ty, 1));
  IRB.CreateStore(Inc, DS.PTSanInLibc);

  // Restore on normal return, including calls made into invokes by discovery.
  IRB.SetInsertPoint(getNormalReturnIP(cast<CallBase>(I)));

  // This isn't quite a --, but it should be equivalent and much cleaner
  IRB.CreateStore(Old, DS.PTSanInLibc);
}

void PTSanLowerer::lowerArithmeticValidation(
    ValidateArithAnchor Anchor) {
  PTSanIRB IRB(DS, Anchor.Call);

  IRB.CreateCall(DS.ValidateArithmetic,
                 {materializeAsInt(IRB, Anchor.getLHS()),
                  materializeAsInt(IRB, Anchor.getRHS())});
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

  IRB.SetInsertPoint(getNormalReturnIP(CB));
  CallInst *AfterCall = IRB.CreateCall(DS.MakecontextStripAfter, Args);
  AfterCall->setDebugLoc(DL);
  AfterCall->setCallingConv(Callee->getCallingConv());
  AfterCall->setDoesNotThrow();
}

void PTSanLowerer::prepareSources() {
  auto isDynamicAlloca = [&](SourceAnchor Source) {
    auto *AI = dyn_cast<AllocaInst>(Source.getSource());
    return AI && !AI->getAllocationSize(DS.DL);
  };

  // Resolve source-backed IDs before lowering their consumers.
  auto resolveSourceID = [&](SourceAnchor Source, Value *ID) {
    for (User *U : make_early_inc_range(Source.Call->users())) {
      if (auto Request = DS.getAsAnchor<GetIDAnchor>(U)) {
        Request->Call->replaceAllUsesWith(ID);
        Request->erase();
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
      Value *ID = IRB.CreateLoad(DS.IDTy, DS.TLSIDs.at(Global));
      resolveSourceID(Source, IRB.CreateZExt(ID, DS.U64Ty));
      continue;
    }

    // The inline salt cache has a fixed 4 KiB alignment contract. Keep
    // unusual alignment/quantum requirements on the existing full slow path.
    if (Cl32BitIDs && ClStackTemporalSafety &&
        getSourceOriginQuantum(Source.getSource()) > 4096) {
      UseFastStackIDs = false;
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

  Triple Target(DS.M.getTargetTriple());

  // Allocate IDs for all sources. AllocateSourceIDs will also set bounds
  // for all statically-known buffers. Dynamic remain below
  size_t NumPtrSourceIDs = StaticPtrSrcs.size() + DynamicPtrSrcs.size();
  UseFrameSalt = Cl32BitIDs && ClStackTemporalSafety && UseFastStackIDs &&
                 NumPtrSourceIDs != 0;
  unsigned NumVAListIDs = 0;
  if (!DS.collectAnchors<VAStartAnchor>(F).empty()) {
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
  HasRepeatableAllocas = !RepeatablePtrSrcs.empty();
  SmallVector<Value *, 8> IDs = allocateSourceIDs(NumSourceIDs, StaticPtrSrcs);
  assert(IDs.size() == NumSourceIDs);
  VAListIDs.assign(IDs.begin() + NumPtrSourceIDs, IDs.end());
  for (size_t I = 0; I < StaticPtrSrcs.size(); ++I) {
    resolveSourceID(StaticPtrSrcs[I], IDs[I]);
  }

  // Handle dynamic allocs.
  // TODO: nonrepeating dynamic allocas still receive IDs at function entry.
  // If their storage is allocated after setjmp, cursor cleanup cannot retire
  // that metadata on longjmp. Moving their ID acquisition is a separate change.
  for (size_t I = 0; I < DynamicPtrSrcs.size(); ++I) {
    Value *ID = IDs[StaticPtrSrcs.size() + I];
    resolveSourceID(DynamicPtrSrcs[I], ID);
    auto *AI = cast<AllocaInst>(DynamicPtrSrcs[I].getSource());
    PTSanIRB IRB(DS, AI->getNextNode());
    Value *Count = IRB.CreateZExtOrTrunc(AI->getArraySize(), DS.U64Ty);
    uint64_t ElementSize =
        DS.DL.getTypeAllocSize(AI->getAllocatedType()).getFixedValue();
    Value *Size = IRB.CreateMul(Count, IRB.getInt64(ElementSize));
    writeSourceBounds(IRB, AI, Size, ID);
  }

  for (SourceAnchor Source : RepeatablePtrSrcs) {
    auto *AI = cast<AllocaInst>(Source.getSource());
    PTSanIRB IRB(DS, AI->getNextNode());
    Value *Count = IRB.CreateZExtOrTrunc(AI->getArraySize(), DS.U64Ty);
    uint64_t ElementSize =
        DS.DL.getTypeAllocSize(AI->getAllocatedType()).getFixedValue();
    Value *Size = IRB.CreateMul(Count, IRB.getInt64(ElementSize));
    Value *ID = IRB.CreateCall(
        DS.GetRepeatableAllocaID,
        {AI, Size, IRB.getInt64(AI->getAlign().value())});
    resolveSourceID(Source, IRB.CreateZExt(ID, DS.U64Ty));
  }

  // Log allocated source IDs, including nonrepeating dynamic allocas.
  if (ClValidate && NumPtrSourceIDs != 0) {
    PTSanIRB IRB(DS, &*F.getEntryBlock().getFirstInsertionPt());
    Value *FnNameCStr = IRB.CreateGlobalStringPtr(F.getName(), F.getName());
    auto logSources = [&](ArrayRef<SourceAnchor> Sources, size_t IDOffset) {
      for (size_t I = 0; I < Sources.size(); ++I) {
        IRB.SetInsertPoint(Sources[I].Call);
        Value *ID = IRB.CreateTrunc(IDs[IDOffset + I], DS.IDTy);
        bool IsLocal = areUsesLocal(Sources[I].Call);
        ConstantInt *IsLocalVal = ConstantInt::get(DS.BoolTy, IsLocal);
        CallInst *Log = IRB.CreateCall(
            DS.LogAllocaInfo, {FnNameCStr, ID, IsLocalVal, HasStackIDRoom});
        // Keep integer-extension attributes in sync with the runtime declaration.
        Log->setAttributes(Log->getCalledFunction()->getAttributes());
      }
    };
    logSources(StaticPtrSrcs, 0);
    logSources(DynamicPtrSrcs, StaticPtrSrcs.size());
  }

}

void PTSanLowerer::lowerVAStart(IntrinsicInst *Start) {
  // va_start and va_copy instances share backing storage for this invocation.
  // Initialize after each start; retire with the function's other stack IDs.
  PTSanIRB IRB(DS, Start->getNextNode());
  switch (Triple(DS.M.getTargetTriple()).getArch()) {
  case Triple::aarch64:
  case Triple::aarch64_be:
  case Triple::aarch64_32: {
    // AAPCS64 has separate GP and FP/SIMD save areas, plus stack arguments.
    // Their sizes are encoded in the initial va_list offsets by the backend.
    IRB.CreateCall(DS.InitVAListAArch64,
                  {Start->getArgOperand(0), VAListIDs[0], VAListIDs[1],
                   VAListIDs[2]});
    break;
  }
  case Triple::x86_64: {
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
    IRB.CreateCall(DS.InitVAListX86_64,
                  {Start->getArgOperand(0), VAListIDs[0], VAListIDs[1],
                   IRB.getInt64(RegisterSize)});
    break;
  }
  default:
    llvm_unreachable("Unsupported PTSan va_list architecture");
  }
}

void PTSanLowerer::run() {
#ifndef NDEBUG
  // Check the original execution paths before lowering adds failure exits.
  for (const auto &[Call, Check] : PTA.PlaceablePtrChecks) {
    for (Use *U : Check.InstigatingUses) {
      assert(PTA.PDT.dominates(cast<Instruction>(U->getUser()), Call));
    }
  }
#endif

  prepareSources();

  // Lower consumers before the anchors they depend on, regardless of block
  // layout. Snapshot the order because lowering can split blocks and update DT.
  SmallVector<CallInst *, 32> Anchors;
  for (DomTreeNode *Node : post_order(PTA.DT.getRootNode())) {
    for (Instruction &I : reverse(*Node->getBlock())) {
      if (DS.isAnchor(&I)) {
        Anchors.push_back(cast<CallInst>(&I));
      }
    }
  }

  for (CallInst *Call : Anchors) {
    Value *Lowered = nullptr;
    if (auto Check = DS.getAsAnchor<CheckAnchor>(Call)) {
      PlaceablePtrCheck &Op = PTA.PlaceablePtrChecks.at(Call);
      if (auto RangeCheck = addPtrCheck(Op)) {
        actuallyAddCheck(*RangeCheck);
      }
      PTA.PlaceablePtrChecks.erase(Call);
    } else if (auto Strip = DS.getAsAnchor<ID16StripAnchor>(Call)) {
      Lowered = lowerID16Strip(*Strip);
    } else if (auto Strip = DS.getAsAnchor<ID32StripAnchor>(Call)) {
      Lowered = lowerID32Strip(*Strip);
    } else if (auto Tag = DS.getAsAnchor<ID16TagAnchor>(Call)) {
      Lowered = lowerID16Tag(*Tag);
    } else if (auto Tag = DS.getAsAnchor<ID32TagAnchor>(Call)) {
      Lowered = lowerID32Tag(*Tag);
    } else if (auto Tagged = DS.getAsAnchor<TaggedAnchor>(Call)) {
      Lowered = lowerTagged(*Tagged);
    } else if (auto Bounds = DS.getAsAnchor<BoundAnchor>(Call)) {
      Lowered = lowerBound(*Bounds);
    } else if (auto ID = DS.getAsAnchor<GetIDAnchor>(Call)) {
      Lowered = lowerGetID(*ID);
    } else if (auto Source = DS.getAsAnchor<SourceAnchor>(Call)) {
      // Check plans can name sources outside SSA; erase them after consumers.
      Lowered = Source->getSource();
    } else if (auto Start = DS.getAsAnchor<VAStartAnchor>(Call)) {
      lowerVAStart(cast<IntrinsicInst>(Start->getTarget()));
    } else if (auto Setjmp = DS.getAsAnchor<SetjmpAnchor>(Call)) {
      lowerSetjmp(cast<CallBase>(Setjmp->getTarget()));
    } else if (auto Exit = DS.getAsAnchor<FunctionExitAnchor>(Call)) {
      lowerFunctionExit(Exit->getTarget());
    } else if (auto Validation = DS.getAsAnchor<ValidateArithAnchor>(Call)) {
      lowerArithmeticValidation(*Validation);
    } else if (auto Special = DS.getAsAnchor<SpecialStripAnchor>(Call)) {
      lowerSpecialStripCall(cast<CallBase>(Special->getTarget()));
    } else if (auto Libc = DS.getAsAnchor<LibcCallAnchor>(Call)) {
      lowerLibcCall(Libc->getTarget());
    } else if (DS.getAsAnchor<LoopPreheaderAnchor>(Call)) {
      // No runtime operation: check anchors already record their placement.
    } else {
      continue;
    }
    if (Lowered) {
      Call->replaceAllUsesWith(Lowered);
    }
    Anchor(DS, Call).erase();
  }

  assert(PTA.PlaceablePtrChecks.empty() &&
         "all optimized check facts should be consumed by lowering");

  // Add the ABI-only operand after discovery and boundary rewriting, so it
  // does not affect instrumentation of the original call's arguments.
  // Strip-only callers also need it when calling an instrumented va_start.
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
      SplitBlockAndInsertIfThen(CI.OutOfRange, CI.IP.getIP(), ClAbortOnError,
                                BW, &PTA.DTU, &PTA.LI);
  PTSanIRB IRB(DS, ThenTerm);
  Value *CheckID = ConstantInt::get(DS.U32Ty, CI.CheckID);

  if (CI.Min->getType()->isVectorTy()) {
    IRB.CreateCall(DS.ReportVectorError, {CheckID, FunctionID});
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
  // Preserve explicit code addresses and literals. This does not imply that
  // stripping an unknown value with the same bits would leave it unchanged.
  if (isa<Function>(V) || isUntaggedConstantPtr(V) || isa<ConstantInt>(V)) {
    return true;
  }

  if (auto *PTI = dyn_cast<PtrToIntOperator>(V)) {
    Value *Pointer = PTI->getPointerOperand();
    return isa<Function>(Pointer) || isUntaggedConstantPtr(Pointer);
  }

  return false;
}

Value *PTSanLowerer::lowerID16Strip(ID16StripAnchor Strip) {
  Value *V = Strip.getValue();
  // Preserve explicit code addresses and literal pointers (including -1).
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
    Value *Bits = materializeAsInt(IRB, V);
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
  Value *Mask = PTA.PTU.splatToMatchType(DS.AddrMask, MaskTy, Strip.Call);

  Function *PMFn = Intrinsic::getDeclaration(F.getParent(), Intrinsic::ptrmask,
                                             {V->getType(), Mask->getType()});
  return IRB.CreateCall(PMFn, {V, Mask}, V->getName() + ".true");
}

// Pseudo-IR; bias = object_base - origin comes from the explicit base operand:
//   bits = ptrtoint(value)
//   payload = bits & 0xffffffff
//   decoded = bias + payload
//   encoded = signed(bits) < -1
//   result = inttoptr(select(encoded, decoded, bits))
// Unconditional strict decoding omits the comparison/select and returns decoded.
// Function symbols and literal pointers are left unchanged.
// Integer strips omit the pointer casts. Vectors use the same operations
// lane-wise, splatting a scalar bias when necessary.
Value *PTSanLowerer::lowerID32Strip(ID32StripAnchor Strip) {
  Value *V = Strip.getValue();
  // Preserve explicit code addresses and literal pointers (including -1).
  if (isUntaggedStripValue(V)) {
    return V;
  }

  PTSanIRB IRB(DS, Strip.Call);
  markIRAsFrom(IRB, "ptsan.ir.lowerStrip");

  Value *Bits = materializeAsInt(IRB, V);
  Type *IntTy = Bits->getType();
  // Decoding consumes an explicit anchor operand; it never loads metadata itself.
  Value *Bias = PTA.PTU.splatToMatchType(Strip.getBaseBound().Call, IntTy,
                                       Strip.Call);
  Value *Payload = IRB.CreateZExt(
      IRB.CreateTrunc(Bits, IntTy->getWithNewType(DS.U32Ty)), IntTy);
  Value *Raw = IRB.CreateAdd(Bias, Payload);
  if (usesConditionalID32Decode()) {
    Value *IsEnriched = IRB.CreateICmpSLT(Bits, ConstantInt::getSigned(IntTy, -1));
    Raw = IRB.CreateSelect(IsEnriched, Raw, Bits);
  }
  return Strip.isPointerStrip() ? IRB.CreateIntToPtr(Raw, V->getType()) : Raw;
}

Value *PTSanLowerer::lowerTagged(TaggedAnchor Tagged) {
  return Tagged.getValue();
}

// Pseudo-IR, with value already stripped and id widened to u64:
//   bits = ptrtoint(value)
//   tag = id << 48
//   result = inttoptr(bits | tag)
// Vectors use the same operations lane-wise, splatting a scalar ID if needed.
Value *PTSanLowerer::lowerID16Tag(ID16TagAnchor Tag) {
  Value *V = Tag.getValue();
  PTSanIRB IRB(DS, Tag.Call);
  markIRAsFrom(IRB, "ptsan.ir.lowerTag");

  assert(V->getType()->isPtrOrPtrVectorTy() &&
         "tag lowering expects pointer or pointer vector");
  Type *IntTy = V->getType()->getWithNewType(DS.U64Ty);

  Value *ValueBits = IRB.CreatePtrToInt(V, IntTy, V->getName() + ".asint");
  Value *ID = PTA.PTU.splatToMatchType(Tag.getID(), IntTy, Tag.Call);
  Value *TagBits = IRB.CreateShl(ID, ConstantInt::get(ID->getType(), 48),
                                 V->getName() + ".tagbits");
  Value *TaggedBits =
      IRB.CreateOr(ValueBits, TagBits, V->getName() + ".tagged.asint");
  return IRB.CreateIntToPtr(TaggedBits, V->getType(),
                            V->getName() + ".tagged");
}

// Pseudo-IR, with value stripped and id/bias supplied by anchor operands:
//   bits = ptrtoint(value)
//   payload = bits - bias
//   tag = id << 32
//   fits = payload <= 0xffffffff
//   candidate = tag | payload
//   packed = select(fits, candidate, fallback)
//   result = inttoptr(select(id == 0, bits, packed))
// Arithmetic is u64; fits rejects negative and oversized payloads alike.
// fallback is zero in strict mode and bits otherwise. The final zero-ID
// select preserves never-tagged pointers. Vectors work lane-wise, splatting
// scalar ID/base operands as needed. bias = object_base - origin, so payload
// includes the allocation's origin.
// Marked modes allocate high-half IDs, so the tag already contains the marker.
// Strict mode omits the runtime zero-ID passthrough unless mixed decoding is
// enabled. This does not change ID zero's empty bounds. Literal pointers are
// left unchanged.
Value *PTSanLowerer::lowerID32Tag(ID32TagAnchor Tag) {
  Value *V = Tag.getValue();
  // Preserve null and literal sentinels by value, not by allocation ID.
  if (isUntaggedConstantPtr(V)) {
    return V;
  }

  PTSanIRB IRB(DS, Tag.Call);
  markIRAsFrom(IRB, "ptsan.ir.lowerTag");

  Type *IntTy = V->getType()->getWithNewType(DS.U64Ty);
  Value *ValueBits = IRB.CreatePtrToInt(V, IntTy, V->getName() + ".asint");
  Value *ID = PTA.PTU.splatToMatchType(Tag.getID(), IntTy, Tag.Call);
  Value *Bias = PTA.PTU.splatToMatchType(Tag.getBaseBound().Call, IntTy, Tag.Call);
  Value *Payload = IRB.CreateSub(ValueBits, Bias);
  Value *TagBits = IRB.CreateShl(ID, 32);
  Value *Fits = IRB.CreateICmpULE(Payload, ConstantInt::get(IntTy, UINT32_MAX));
  Value *Fallback = ClStrictMode ? ConstantInt::get(IntTy, 0) : ValueBits;
  // A non-fitting payload is discarded, so the candidate needs no payload mask.
  Value *Packed = IRB.CreateSelect(
      Fits, IRB.CreateOr(TagBits, Payload), Fallback);
  if (usesConditionalID32Decode()) {
    Packed = IRB.CreateSelect(IRB.CreateICmpEQ(ID, ConstantInt::get(IntTy, 0)),
                              ValueBits, Packed);
  }
  return IRB.CreateIntToPtr(Packed, V->getType());
}

static DIScope *getOrCreateInlinee(DIBuilder &DIB,
                                        DICompileUnit *CU,
                                        Module &M,
                                        StringRef Name) {
  if (auto *NM = M.getNamedMetadata("ptsan.inlinees")) {
    for (auto *Op : NM->operands()) {
      auto *T = cast<MDNode>(Op);
      if (auto *S = dyn_cast<MDString>(T->getOperand(0));
          S && S->getString() == Name) {
        return cast<DIScope>(T->getOperand(1));
      }
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
