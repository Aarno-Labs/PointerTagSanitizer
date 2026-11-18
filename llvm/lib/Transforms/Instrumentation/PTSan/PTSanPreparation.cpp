//===-- PTSanPreparation.cpp - PTSan preparation phase ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PTSanPhases.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

namespace llvm::ptsan {

static void expandModfIntrinsics(Function &F) {
  // Mirror SelectionDAGLegalize::expandModf() in
  // llvm/lib/CodeGen/SelectionDAG/LegalizeDAG.cpp. Code generation otherwise
  // prefers the modf libcall when available and can use the destination of a
  // result store as the call's output pointer. By then PTSan has stripped that
  // store destination, so instrumented libc receives a raw pointer. Expand
  // while the result stores are still visible to PTSan.
  for (Instruction &I : make_early_inc_range(instructions(F))) {
    auto *Modf = dyn_cast<IntrinsicInst>(&I);
    if (!Modf || Modf->getIntrinsicID() != Intrinsic::modf) {
      continue;
    }

    IRBuilder<> IRB(Modf);
    IRB.SetCurrentDebugLocation(Modf->getDebugLoc());
    Value *Input = Modf->getArgOperand(0);
    Value *IntPart =
        IRB.CreateUnaryIntrinsic(Intrinsic::trunc, Input, Modf, "modf.int");
    Value *FracPart = IRB.CreateFSubFMF(Input, IntPart, Modf, "modf.frac");

    Value *FracToUse = FracPart;
    if (!Modf->hasNoInfs()) {
      Value *Abs = IRB.CreateFAbs(Input, Modf, "modf.abs");
      Value *IsInf = IRB.CreateFCmpFMF(
          FCmpInst::FCMP_OEQ, Abs, ConstantFP::getInfinity(Input->getType()),
          Modf, "modf.isinf");
      FracToUse = IRB.CreateSelect(IsInf, ConstantFP::getZero(Input->getType()),
                                   FracPart, "modf.frac.select");
    }

    Value *ResultFrac =
        IRB.CreateCopySign(FracToUse, Input, Modf, "modf.frac.signed");
    Value *Result = PoisonValue::get(Modf->getType());
    Result = IRB.CreateInsertValue(Result, ResultFrac, 0, "modf.result.frac");
    Result = IRB.CreateInsertValue(Result, IntPart, 1, "modf.result");
    Modf->replaceAllUsesWith(Result);
    Modf->eraseFromParent();
  }
}

static void foldObstackPointerReconstruction(Function &F) {
  // Legacy obstack_free reconstructs target as chunk + (target - chunk),
  // including a literal NULL target. Recover the original pointer and its ID
  // before instrumentation can mix tagged integer bits with a stripped GEP
  // base. Other uses of the offset remain.
  const DataLayout &DL = F.getDataLayout();
  for (Instruction &I : make_early_inc_range(instructions(F))) {
    auto *GEP = dyn_cast<GetElementPtrInst>(&I);
    if (!GEP || !GEP->getType()->isPointerTy() || GEP->getNumIndices() != 1 ||
        !GEP->getSourceElementType()->isIntegerTy(8)) {
      continue;
    }
    Value *Base = GEP->getPointerOperand();
    Value *Offset = GEP->getOperand(1);
    unsigned AddressSpace = GEP->getPointerAddressSpace();
    unsigned PointerBits = DL.getPointerSizeInBits(AddressSpace);
    Value *TargetBits;
    if (DL.isNonIntegralPointerType(Base->getType()) ||
        !Offset->getType()->isIntegerTy(PointerBits) ||
        DL.getIndexSizeInBits(AddressSpace) != PointerBits ||
        !match(Offset, m_Sub(m_Value(TargetBits),
                            m_PtrToIntOrAddr(m_Specific(Base))))) {
      continue;
    }
    Value *Target;
    if (match(TargetBits, m_Zero())) {
      Target = ConstantPointerNull::get(cast<PointerType>(GEP->getType()));
    } else if (!match(TargetBits, m_PtrToIntOrAddr(m_Value(Target))) ||
               Target->getType() != GEP->getType()) {
      continue;
    }
    GEP->replaceAllUsesWith(Target);
    GEP->eraseFromParent();
  }
}

static void normalizeNullGEPs(Function &F) {
  // SCEV expansion can express an integer-to-pointer cast as byte arithmetic
  // from null. Make the cast explicit so discovery does not inherit null's
  // stripped representation when the integer contains an encoded pointer.
  const DataLayout &DL = F.getDataLayout();
  for (Instruction &I : make_early_inc_range(instructions(F))) {
    auto *GEP = dyn_cast<GetElementPtrInst>(&I);
    if (!GEP || !GEP->getType()->isPointerTy() || GEP->getNumIndices() != 1 ||
        !GEP->getSourceElementType()->isIntegerTy(8) ||
        !isa<ConstantPointerNull>(GEP->getPointerOperand())) {
      continue;
    }
    Value *Offset = GEP->getOperand(1);
    unsigned AddressSpace = GEP->getPointerAddressSpace();
    unsigned PointerBits = DL.getPointerSizeInBits(AddressSpace);
    // Full-width indices avoid GEP's sign extension of narrower integers.
    if (DL.isNonIntegralPointerType(GEP->getType()) ||
        !Offset->getType()->isIntegerTy(PointerBits) ||
        DL.getIndexSizeInBits(AddressSpace) != PointerBits) {
      continue;
    }
    IRBuilder<> IRB(GEP);
    Value *Pointer = IRB.CreateIntToPtr(Offset, GEP->getType());
    Pointer->takeName(GEP);
    GEP->replaceAllUsesWith(Pointer);
    GEP->eraseFromParent();
  }
}

// Emit an uninstrumented forwarding function for one of these three forms:
//
//   void entry_for_no_args() {
//     ptsan_signal_entry_0(no_args_handler);
//   }
//
//   void entry_for_plain(int signal) {
//     ptsan_signal_entry_1(signal, plain_handler);
//   }
//
//   void entry_for_siginfo(int signal, siginfo_t *info, void *context) {
//     ptsan_signal_entry_3(signal, info, context, siginfo_handler);
//   }
//
// The compiler-rt helper performs setup, calls the original handler, and does
// cleanup on normal return. The generated function only supplies the handler's
// address; it needs no saved state or cleanup of its own.
static Function *createSignalEntry(Function &Handler) {
  Module &M = *Handler.getParent();
  LLVMContext &C = M.getContext();
  auto *Entry = Function::Create(Handler.getFunctionType(),
                                GlobalValue::InternalLinkage,
                                "__ptsan_signal_entry." + Handler.getName(), M);
  Entry->addFnAttr(Attribute::DisableSanitizerInstrumentation);
  // The kernel enters this wrapper rather than the original handler, so it
  // needs the same branch-target landing pad.
  for (StringRef Name : {"target-cpu", "target-features",
                        "branch-target-enforcement"}) {
    Attribute Attr = Handler.getFnAttribute(Name);
    if (Attr.isValid()) {
      Entry->addFnAttr(Attr);
    }
  }

  Type *Void = Type::getVoidTy(C);
  Type *Int = Type::getInt32Ty(C);
  Type *Ptr = PointerType::getUnqual(C);
  FunctionCallee Helper;
  if (Handler.arg_empty()) {
    Helper = M.getOrInsertFunction("ptsan_signal_entry_0", Void, Ptr);
  } else if (Handler.arg_size() == 1) {
    Helper = M.getOrInsertFunction("ptsan_signal_entry_1", Void, Int, Ptr);
  } else {
    Helper = M.getOrInsertFunction("ptsan_signal_entry_3", Void, Int, Ptr, Ptr, Ptr);
  }

  SmallVector<Value *> Args;
  for (Argument &A : Entry->args()) {
    Args.push_back(&A);
  }
  Args.push_back(&Handler);
  IRBuilder<> B(BasicBlock::Create(C, "entry", Entry));
  B.CreateCall(Helper, Args)->setTailCallKind(CallInst::TCK_Tail);
  B.CreateRetVoid();
  return Entry;
}

// A permanent function-to-entry binding lets the kernel snapshot the handler
// together with its flags and mask. A mutable per-signal dispatch table cannot
// preserve that association across concurrent sigaction calls and delivery.
bool PTSanPreparationPass::run(Module &M) {
  if (!Triple(M.getTargetTriple()).isMusl()) {
    return false;
  }

  LLVMContext &C = M.getContext();
  Type *Ptr = PointerType::getUnqual(C);
  Type *Void = Type::getVoidTy(C);
  Type *Int = Type::getInt32Ty(C);
  auto *NoArgsHandlerTy = FunctionType::get(Void, false);
  auto *PlainHandlerTy = FunctionType::get(Void, {Int}, false);
  auto *SiginfoHandlerTy = FunctionType::get(Void, {Int, Ptr, Ptr}, false);

  SmallVector<Function *> Handlers;
  for (Function &F : M) {
    // Externally visible definitions may be registered by another translation
    // unit, even without a local address-taking use.
    if (F.isDeclaration() || F.hasAvailableExternallyLinkage() ||
        (F.hasLocalLinkage() && !F.hasAddressTaken()) ||
        F.getCallingConv() != CallingConv::C) {
      continue;
    }
    // Match the whole signature: void(), void(int), or void(int, ptr, ptr),
    // with address-space-zero pointers and no variadic arguments. The void()
    // form supports legacy handlers that ignore the kernel's arguments.
    if (F.getFunctionType() != NoArgsHandlerTy &&
        F.getFunctionType() != PlainHandlerTy &&
        F.getFunctionType() != SiginfoHandlerTy) {
      continue;
    }
    bool Compatible = true;
    // Any of these attributes mean it can't be a signal handler
    for (Argument &A : F.args()) {
      if (A.hasByValAttr() || A.hasStructRetAttr() || A.hasInAllocaAttr() ||
          A.hasAttribute(Attribute::Nest) ||
          A.hasAttribute(Attribute::SwiftSelf) ||
          A.hasAttribute(Attribute::SwiftError)) {
        Compatible = false;
      }
    }
    if (Compatible) {
      Handlers.push_back(&F);
    }
  }
  if (Handlers.empty()) {
    return false;
  }

  // Emit separate records so a COMDAT handler, its wrapper, and its record
  // are discarded together. The linker concatenates the surviving records.
  auto *RecordTy = StructType::get(C, {Ptr, Ptr});
  SmallVector<GlobalValue *> Records;
  for (Function *Handler : Handlers) {
    Function *Entry = createSignalEntry(*Handler);
    Entry->setComdat(Handler->getComdat());
    auto *Record = new GlobalVariable(
        M, RecordTy, true, GlobalValue::InternalLinkage,
        ConstantStruct::get(RecordTy, {Handler, Entry}),
        "__ptsan_signal_entries." + Handler->getName());
    Record->setComdat(Handler->getComdat());
    Record->setSection("ptsan_signal_entries");
    Record->setAlignment(Align(8));
    Records.push_back(Record);
  }
  appendToUsed(M, Records);
  return true;
}

bool PTSanPreparationPass::run(PTSanAnalysisState &PTA) {
  expandModfIntrinsics(PTA.F);
  foldObstackPointerReconstruction(PTA.F);
  normalizeNullGEPs(PTA.F);

  // Clang can use TLS symbols directly in dynamic initializers. Normalize
  // those uses so discovery gives them the same source anchors as explicit
  // threadlocal.address calls, including references inside constant GEPs.
  for (auto &Entry : PTA.DS.TLSIDs) {
    GlobalValue *Global = Entry.first;
    materializeConstantUsersOfGlobal(PTA.F, Global);
    SmallVector<Use *, 8> Uses;
    for (Use &U : Global->uses()) {
      auto *I = dyn_cast<Instruction>(U.getUser());
      if (!I || I->getFunction() != &PTA.F) {
        continue;
      }
      if (auto *II = dyn_cast<IntrinsicInst>(I);
          II && II->getIntrinsicID() == Intrinsic::threadlocal_address) {
        continue;
      }
      Uses.push_back(&U);
    }
    if (Uses.empty()) {
      continue;
    }
    IRBuilder<> IRB(&*PTA.F.getEntryBlock().getFirstInsertionPt());
    Value *Address = IRB.CreateThreadLocalAddress(Global);
    for (Use *U : Uses) {
      U->set(Address);
    }
  }

  for (Loop *L : PTA.LI) {
    formLCSSARecursively(*L, PTA.DT, &PTA.LI, &PTA.SE);
  }

  for (Loop *L : PTA.LI.getLoopsInPreorder()) {
    InsertPreheaderForLoop(L, &PTA.DT, &PTA.LI, nullptr,
                           /*PreserveLCSSA=*/true);
  }

  for (Loop *L : PTA.LI.getLoopsInPreorder()) {
    simplifyLoop(L, &PTA.DT, &PTA.LI, &PTA.SE, nullptr, nullptr,
                 /*PreserveLCSSA=*/true);
  }

  PTA.PDT.recalculate(PTA.F);

  return true;
}

} // namespace llvm::ptsan
