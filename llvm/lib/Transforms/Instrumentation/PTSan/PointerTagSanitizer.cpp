//===-- PointerTagSanitizer.h - Track pointer bounds by tagging upper bits----*- C++ -*-===//
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
#include "llvm/ADT/DenseSet.h"
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
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Instrumentation/PointerTagSanitizer.h"
#include "PTSanInternal.h"
#include "PTSanPhases.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "PTSanUtils.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"

#include <unordered_map>
#include <memory>
#include <stdlib.h>
#include <variant>


using namespace llvm;
using namespace llvm::PatternMatch;
using std::optional;
using std::nullopt;

#define DEBUG_TYPE "ptsan-dbg"
#define INFO_TYPE "ptsan-info"
#define DBG(X) DEBUG_WITH_TYPE(DEBUG_TYPE, X)
#define INFO(X) DEBUG_WITH_TYPE(INFO_TYPE, X)

namespace llvm::ptsan {

cl::opt<bool> ClDisable(
  "ptsan-disable",
  cl::desc("Do not actually run the pass. Useful for testing internals, so you can still use -fsanitize=ptsan"),
  cl::init(false)
);

cl::opt<bool> ClOptimizeAfterward(
  "ptsan-reoptimize",
  cl::desc("Run optimizations after instrumentation. Might make things less stable but should be worth some speedup"),
  cl::init(true)
);

cl::opt<bool> ClNoBoundsLoadHoist(
  "ptsan-no-load-hoist",
  cl::desc("Don't hoist bounds loads. This implies no loop check hoist"),
  cl::init(false)
);

cl::opt<bool> ClNoLoopCheckHoist(
  "ptsan-no-loop-check-hoist",
  cl::desc("Don't hoist bounds checks out of loops"),
  cl::init(false)
);

cl::opt<bool> ClNoCheckMerge(
  "ptsan-no-check-merge",
  cl::desc("Don't merge compatible bounds checks"),
  cl::init(false)
);

cl::opt<bool> ClNoFilterRemovableChecks(
  "ptsan-no-filter-removable-checks",
  cl::desc("Don't remove bounds checks that static analysis proves unnecessary"),
  cl::init(false)
);

cl::opt<bool> ClNoStackIDFastPath(
  "ptsan-no-stack-id-fast-path",
  cl::desc("Don't use inline stack ID allocation or specialized small stack ID helpers"),
  cl::init(false)
);

cl::opt<bool> ClNoChecks(
  "ptsan-nochecks",
  cl::desc("Don't do checking -- just propagate metadata"),
  cl::init(false)
);

cl::opt<bool> ClNoIDs(
  "ptsan-noids",
  cl::desc("Don't generate ids. Metadata will propagate as normal, and checks will happen, but all ids will be 0"),
  cl::init(false)
);

cl::opt<bool> ClNoStrip(
  "ptsan-nostrip",
  cl::desc("Don't strip ids. This will break things unless you're using properly set up lam or ptsan-noids"),
  cl::init(false)
);

cl::opt<bool> ClLAM(
  "ptsan-lam",
  cl::desc("Use LAM-compatible PTSan mode"),
  cl::ValueDisallowed,
  cl::init(false)
);

static unsigned getEffectiveIDBits() { return ClLAM ? 15 : 16; }

static unsigned getDefaultIDNo() { return (1U << getEffectiveIDBits()) - 1; }

cl::opt<bool> ClInstrumentGlobals(
  "ptsan-instrument-globals",
  cl::desc("Instrument Globals"),
  cl::init(true)
);

cl::opt<bool> ClValidate(
  "ptsan-runtime-validate",
  cl::desc("Validate during runtime. This is potentially expensive and verbose"),
  cl::ValueDisallowed,
  cl::init(false)
);

cl::opt<bool> ClValidateArith(
  "ptsan-runtime-validate-arith",
  cl::desc("Validate pointer arithmetic tags. Needs normal validate"),
  cl::init(false)
);

cl::opt<bool> ClPrintStats(
  "ptsan-print-stats",
  cl::desc("Print stats after running with validate. This will not work if not running with validate"),
  cl::init(false)
);

cl::opt<bool> ClClearIdsOnArith(
  "ptsan-clear-ids-on-arith",
  cl::desc("Clear IDs on pointer aritmetic. This is necessary if either a.) somehow zero ids "
          "are being compared to nonzero or b.) The app is comparing pointers to differen buffers (UB!!)"),
  cl::init(true)
);

cl::opt<bool> ClClearIdsOnCmp(
  "ptsan-clear-ids-on-cmp",
  cl::desc("Clear IDs on pointer comparisons. This is necessary if either a.) somehow zero ids "
          "are being compared to nonzero or b.) The app is comparing pointers to differen buffers (UB!!)"),
  cl::init(true)
);

cl::opt<bool> ClQuietErrors(
  "ptsan-quiet-errors",
  cl::desc("Do not print anything on error"),
  cl::init(false)
);

cl::opt<bool> ClStrictMode(
  "ptsan-strict-mode",
  cl::desc("Zero and DefaultID buffers are invalid"),
  cl::init(false)
);

cl::opt<bool> ClNoPTSanOpts(
  "ptsan-no-opts",
  cl::desc("Do not optimize ptsan at all -- every value gets a check"),
  cl::init(false)
);

cl::opt<bool> ClNoStripIDHoist(
  "ptsan-no-strip-id-hoist",
  cl::desc("Do not optimize strip-ID placement"),
  cl::init(false)
);

cl::opt<bool> ClUseLocalStripIDHoist(
  "ptsan-use-local-strip-id-hoist",
  cl::desc("Use local instead of min-cut strip-ID hoisting"),
  cl::init(false)
);

bool isLoopCheckHoistDisabled() {
  return ClNoPTSanOpts || ClNoLoopCheckHoist || ClNoBoundsLoadHoist;
}

bool isCheckMergeDisabled() {
  return ClNoPTSanOpts || ClNoCheckMerge;
}

bool isRemovableCheckFilteringDisabled() {
  return ClNoPTSanOpts || ClNoFilterRemovableChecks;
}

bool isStackIDFastPathDisabled() {
  return ClNoPTSanOpts || ClNoStackIDFastPath;
}

cl::opt<bool> ClUseReturnedMallocSize(
  "ptsan-use-returned-malloc-size",
  cl::desc("Use `malloc_usable_size` to get malloc buffer size, rather than requested size"),
  cl::init(false)
);

cl::opt<bool> ClAbortOnError(
  "ptsan-abort-on-error",
  cl::desc("Abort on error"),
  cl::init(false)
);

// Used in PTSanLoopInfoGather as well
cl::opt<bool> ClPTSanPreanalyzeLoops(
  "ptsan-preanalyze-loops",
  cl::desc("Preanalyze loops for ptsan"),
  cl::init(true)
);

cl::opt<bool> ClStackTemporalSafety(
  "ptsan-stack-temporal-safety",
  cl::desc("Preserve stronger temporal safety for stack objects. Adds nonzero overhead"),
  cl::init(true)
);

cl::opt<unsigned> ClStackIDsPerBucket(
  "ptsan-stack-ids-per-bucket",
  cl::desc("Number of fast-path stack IDs reserved for each PTSan stack bucket"),
  cl::init(512)
);

cl::opt<unsigned> ClStackIDThreadBuckets(
  "ptsan-stack-id-thread-buckets",
  cl::desc("Number of threads that can hold a fast-path PTSan stack ID bucket"),
  cl::init(12)
);

cl::opt<bool> ClAddOverflowChecks(
  "ptsan-add-overflow-checks",
  cl::desc("Use uadd.with.overflow to lower generated bounds checks"),
  cl::init(true)
);

// PTSan assumes all linked modules are built with the same PTSan mode flags.
// Mixing ptsan-stack-temporal-safety settings is unsupported.

cl::opt<int> ClPatchAllWith(
  "ptsan-patch-all-with",
  cl::desc("Patch all bugs with this type. 0 is no patch"),
  cl::init(0)
);

cl::opt<bool> ClCreateIRDbg(
  "ptsan-create-ir-dbg",
  cl::desc("Add debug information to generated IR. This will explode if you don't use -g with it"),
  cl::init(false)
);

cl::opt<std::string> ClStopAfter(
  "ptsan-stop-after",
  cl::desc("Stop after an internal PTSan phase: prepare, discovery, "
           "optimization, metadata-lowering, or lowering"),
  cl::Hidden,
  cl::init("")
);



PointerTagSanitizer::PointerTagSanitizer(Module &M) :
  M(M), Ctx(M.getContext()), DL(M.getDataLayout()),  DIB(M, true) {

  auto *File = DIB.createFile("<ptsan>", "");
  CU = DIB.createCompileUnit(dwarf::DW_LANG_C, File,
                              "ptsan-instrumentation", /*Opt*/true, "", 0);

  PtrTy = PointerType::getUnqual(Ctx);
  UptrTy = DL.getIntPtrType(Ctx);
  U16Ty = IntegerType::get(Ctx, 16);
  U32Ty = IntegerType::get(Ctx, 32);
  U64Ty = IntegerType::get(Ctx, 64);
  U8Ty = IntegerType::get(Ctx, 8);
  BoolTy = IntegerType::get(Ctx, 1);

  AddrMask = ConstantInt::get(U64Ty, 0x0000FFFFFFFFFFFFULL);
  IDMask = ConstantInt::get(U64Ty, 0xffff000000000000);
  DefaultIDNo = ConstantInt::get(U16Ty, getDefaultIDNo());

  {
  SmallVector<Type*, 2> Fields = {
    U16Ty, // Id
    PtrTy, // Pointer to next Node
    };
  IDNodeStructTy =
      StructType::create(Ctx, Fields, "__ptsan_IDNode", /*isPacked=*/false);
  }
  {
  SmallVector<Type*, 2> Fields = {
    U64Ty, // base
    U64Ty, // size
    };
  BoundsStructTy =
      StructType::create(Ctx, Fields, "__ptsan_ObjectBounds", /*isPacked=*/false);
  }
  {
  SmallVector<Type*, 2> Fields = {
    PtrTy, // Base
    U64Ty, // Size
    };
  NewRangeStructTy =
      StructType::create(Ctx, Fields, "__ptsan_NewRange", /*isPacked=*/false);
  }
  {
  SmallVector<Type*, 2> Fields = {
    U32Ty, // Span A
    U32Ty, // Span B
    };
  TwoSpansTy =
      StructType::create(Ctx, Fields, "__ptsan_TwoSpans", /*isPacked=*/false);
  }


  Nullptr = ConstantPointerNull::get(PtrTy);

  initializeModule();
}

void PointerTagSanitizer::markAsInstr(Value *V) {
  InstrVals.insert(V);
}

void PointerTagSanitizer::unmarkAsInstr(Value *V) {
  InstrVals.erase(V);
}

bool PointerTagSanitizer::isInstr(Value *V) {
  return InstrVals.contains(V);
}

void PointerTagSanitizer::initializeModule() {
  Type *VoidTy = Type::getVoidTy(Ctx);
  AttributeList AL;
  AL = AL.addFnAttribute(Ctx, Attribute::NoUnwind);
  AL = AL.addFnAttribute(Ctx, Attribute::WillReturn);

  GetStackIDsN = M.getOrInsertFunction("ptsan_get_stack_ids_n", AL,
    VoidTy, PtrTy, U64Ty);
  markAsInstr(GetStackIDsN.getCallee()->stripPointerCasts());

  GetStackIDs1 = M.getOrInsertFunction("ptsan_get_stack_ids_1", AL,
    U16Ty, PtrTy, U64Ty);
  markAsInstr(GetStackIDs1.getCallee()->stripPointerCasts());

  GetStackIDs2 = M.getOrInsertFunction("ptsan_get_stack_ids_2", AL,
    U32Ty, PtrTy, PtrTy, U64Ty);
  markAsInstr(GetStackIDs2.getCallee()->stripPointerCasts());

  GetStackIDs3 = M.getOrInsertFunction("ptsan_get_stack_ids_3", AL,
    U64Ty, PtrTy, PtrTy, PtrTy, U64Ty, U32Ty);
  markAsInstr(GetStackIDs3.getCallee()->stripPointerCasts());

  GetStackIDs4 = M.getOrInsertFunction("ptsan_get_stack_ids_4", AL,
    U64Ty, PtrTy, PtrTy, PtrTy, PtrTy, U64Ty, U64Ty);
  markAsInstr(GetStackIDs4.getCallee()->stripPointerCasts());

  FreeStackIDsN = M.getOrInsertFunction("ptsan_free_stack_ids_n", AL,
    VoidTy, PtrTy, U64Ty);
  markAsInstr(FreeStackIDsN.getCallee()->stripPointerCasts());

  FreeStackIDs1 = M.getOrInsertFunction("ptsan_free_stack_ids_1", AL,
    VoidTy, U16Ty);
  markAsInstr(FreeStackIDs1.getCallee()->stripPointerCasts());

  FreeStackIDs2 = M.getOrInsertFunction("ptsan_free_stack_ids_2", AL,
    VoidTy, U32Ty);
  markAsInstr(FreeStackIDs2.getCallee()->stripPointerCasts());

  FreeStackIDs3 = M.getOrInsertFunction("ptsan_free_stack_ids_3", AL,
    VoidTy, U64Ty);
  markAsInstr(FreeStackIDs3.getCallee()->stripPointerCasts());

  FreeStackIDs4 = M.getOrInsertFunction("ptsan_free_stack_ids_4", AL,
    VoidTy, U64Ty);
  markAsInstr(FreeStackIDs4.getCallee()->stripPointerCasts());

  LogAllocaInfo = M.getOrInsertFunction("ptsan_log_alloca_info", AL,
    VoidTy, PtrTy, U16Ty, BoolTy, BoolTy);
  markAsInstr(LogAllocaInfo.getCallee()->stripPointerCasts());

  LogCheckCalled = M.getOrInsertFunction("ptsan_log_check_called", AL, VoidTy, U32Ty);
  markAsInstr(LogCheckCalled.getCallee()->stripPointerCasts());

  GetNextGlobalID = M.getOrInsertFunction("ptsan_get_next_global_id", AL,
    U16Ty, PtrTy, U64Ty);
  markAsInstr(GetNextGlobalID.getCallee()->stripPointerCasts());


  ValidateArithmetic = M.getOrInsertFunction("ptsan_validate_arithmetic", AL,
    VoidTy, U64Ty, U64Ty);
  markAsInstr(ValidateArithmetic.getCallee()->stripPointerCasts());

  FunctionType *MakecontextStripTy =
      FunctionType::get(VoidTy, {PtrTy, PtrTy, U32Ty}, /*isVarArg=*/true);
  auto configureMakecontextStripHelper = [&](FunctionCallee Helper) {
    markAsInstr(Helper.getCallee()->stripPointerCasts());
    if (Function *HelperF = dyn_cast<Function>(Helper.getCallee())) {
      HelperF->setOnlyAccessesInaccessibleMemOrArgMem();
    }
  };
  MakecontextStripBefore =
      M.getOrInsertFunction("ptsan_strip_makecontext_before",
                            MakecontextStripTy, AL);
  configureMakecontextStripHelper(MakecontextStripBefore);
  MakecontextStripAfter =
      M.getOrInsertFunction("ptsan_strip_makecontext_after",
                            MakecontextStripTy, AL);
  configureMakecontextStripHelper(MakecontextStripAfter);

  ReportGenericError = M.getOrInsertFunction("ptsan_complain", AL,
    VoidTy, // return type: void
    U32Ty,  // check_id
    U64Ty,  // function_id
    U64Ty,  // ptr_id
    PtrTy,  // type (const char*)
    U64Ty,  // min
    U64Ty   // size
  );
  markAsInstr(ReportGenericError.getCallee()->stripPointerCasts());

  EnableLibcInstr = M.getOrInsertFunction("ptsan_enable_libc_instr", AL, VoidTy);
  markAsInstr(EnableLibcInstr.getCallee()->stripPointerCasts());

  ValidateFunctionStart = M.getOrInsertFunction("ptsan_validate_function_start", AL, VoidTy);
  markAsInstr(ValidateFunctionStart.getCallee()->stripPointerCasts());

  FunctionType *VoidOperationTy =
      FunctionType::get(VoidTy, /*isVarArg=*/true);
  PTSanSource = M.getOrInsertFunction("__ptsan_source", VoidOperationTy, AL);
  PTSanAccess = M.getOrInsertFunction("__ptsan_access", VoidOperationTy, AL);
  PTSanLoopPreanalysis =
      M.getOrInsertFunction("__ptsan_loop_preanalysis", VoidOperationTy, AL);
  PTSanGetID =
      M.getOrInsertFunction("__ptsan_get_id", AL, U64Ty, PtrTy);
  PTSanLoadBase =
      M.getOrInsertFunction("__ptsan_load_base", AL, U64Ty, U64Ty);
  PTSanLoadSpan =
      M.getOrInsertFunction("__ptsan_load_span", AL, U64Ty, U64Ty);
  PTSanStaticBase =
      M.getOrInsertFunction("__ptsan_static_base", AL, U64Ty, PtrTy);
  PTSanStaticSpan =
      M.getOrInsertFunction("__ptsan_static_span", AL, U64Ty, U64Ty);
  PTSanStrip =
      M.getOrInsertFunction("__ptsan_strip", AL, PtrTy, PtrTy);
  PTSanStripI64 =
      M.getOrInsertFunction("__ptsan_strip_i64", AL, U64Ty, U64Ty);
  PTSanTagged =
      M.getOrInsertFunction("__ptsan_tagged", AL, PtrTy, PtrTy);
  PTSanTag =
      M.getOrInsertFunction("__ptsan_tag", AL, PtrTy, PtrTy, U64Ty);
  PTSanFunctionExit =
      M.getOrInsertFunction("__ptsan_function_exit", VoidOperationTy, AL);
  PTSanValidateArith =
      M.getOrInsertFunction("__ptsan_validate_arith", VoidOperationTy, AL);
  PTSanLibcCall =
      M.getOrInsertFunction("__ptsan_libc_call", VoidOperationTy, AL);
  PTSanSpecialStrip =
      M.getOrInsertFunction("__ptsan_special_strip", VoidOperationTy, AL);

  for (FunctionCallee Callee :
       {PTSanSource, PTSanAccess, PTSanLoopPreanalysis, PTSanGetID,
        PTSanLoadBase, PTSanLoadSpan, PTSanStaticBase, PTSanStaticSpan,
        PTSanStrip, PTSanStripI64, PTSanTagged, PTSanTag,
        PTSanFunctionExit, PTSanValidateArith, PTSanLibcCall,
        PTSanSpecialStrip}) {
    Value *V = Callee.getCallee();
    markAsInstr(V);
    AnchorCallees.insert(V);
  }

  auto *UnmaterializedRange = cast<GlobalVariable>(
      M.getOrInsertGlobal("__ptsan_unmaterialized_range", U8Ty));
  UnmaterializedRange->setInitializer(ConstantInt::get(U8Ty, 0));
  UnmaterializedRange->setLinkage(GlobalValue::PrivateLinkage);
  UnmaterializedRange->setConstant(true);
  PTSanUnmaterializedRange = UnmaterializedRange;
  markAsInstr(PTSanUnmaterializedRange);

  TypesArray = cast<GlobalValue>(
    M.getOrInsertGlobal("ptsan_obj_types", ArrayType::get(U8Ty, 0x10000)));
  TypesArray->setLinkage(GlobalValue::ExternalLinkage);
  markAsInstr(TypesArray);

  BoundsArray = cast<GlobalValue>(
    M.getOrInsertGlobal("ptsan_bounds", ArrayType::get(BoundsStructTy, 0x10000)));
  BoundsArray->setLinkage(GlobalValue::ExternalLinkage);
  markAsInstr(BoundsArray);

  OverflowIDBoundsPtr = cast<GlobalValue>(
    M.getOrInsertGlobal("ptsan_default_id_bounds", BoundsStructTy));
  OverflowIDBoundsPtr->setLinkage(GlobalValue::ExternalLinkage);
  markAsInstr(OverflowIDBoundsPtr);

  NextStackIDPtr = cast<GlobalValue>(
    M.getOrInsertGlobal("ptsan_next_stack_id", U16Ty));
  NextStackIDPtr->setLinkage(GlobalValue::ExternalLinkage);
  NextStackIDPtr->setThreadLocalMode(GlobalVariable::InitialExecTLSModel);
  markAsInstr(NextStackIDPtr);

  if (ClStackTemporalSafety) {
    PendingStackIdsPtr = cast<GlobalValue>(
      M.getOrInsertGlobal("ptsan_pending_stack_ids", U16Ty));
    PendingStackIdsPtr->setLinkage(GlobalValue::ExternalLinkage);
    PendingStackIdsPtr->setThreadLocalMode(GlobalVariable::InitialExecTLSModel);
    markAsInstr(PendingStackIdsPtr);
  }

  PTSanInLibc = cast<GlobalValue>(
    M.getOrInsertGlobal("ptsan_in_libc", U32Ty));
  PTSanInLibc->setLinkage(GlobalValue::ExternalLinkage);
  PTSanInLibc->setThreadLocal(true);
  PTSanInLibc->setThreadLocalMode(GlobalVariable::GeneralDynamicTLSModel);
  markAsInstr(PTSanInLibc);

  StackIDLimitPtr = cast<GlobalValue>(
    M.getOrInsertGlobal("ptsan_stack_id_limit", U16Ty));
  StackIDLimitPtr->setLinkage(GlobalValue::ExternalLinkage);
  StackIDLimitPtr->setThreadLocal(true);

  markAsInstr(StackIDLimitPtr);

  setRuntimeFlags();
}


#define SET_RUNTIME_FLAG(Name)                                                 \
  {                                                                            \
    GlobalVariable *GV =                                                       \
        cast<GlobalVariable>(M.getOrInsertGlobal(#Name, U32Ty));               \
    Constant *NewInit = ConstantInt::get(GV->getValueType(), Name ? 1 : 0);    \
    GV->setInitializer(NewInit);                                               \
    GV->setLinkage(GlobalValue::WeakODRLinkage);                               \
    GV->setConstant(true);                                                     \
  }

#define SET_RUNTIME_U32(Name)                                                  \
  {                                                                            \
    GlobalVariable *GV =                                                       \
        cast<GlobalVariable>(M.getOrInsertGlobal(#Name, U32Ty));               \
    Constant *NewInit = ConstantInt::get(GV->getValueType(), Name);            \
    GV->setInitializer(NewInit);                                               \
    GV->setLinkage(GlobalValue::WeakODRLinkage);                               \
    GV->setConstant(true);                                                     \
  }

void PointerTagSanitizer::setRuntimeFlags() {
  SET_RUNTIME_FLAG(ClNoIDs);
  SET_RUNTIME_FLAG(ClNoStrip);
  SET_RUNTIME_FLAG(ClLAM);
  SET_RUNTIME_FLAG(ClUseReturnedMallocSize);
  SET_RUNTIME_FLAG(ClValidate);
  SET_RUNTIME_FLAG(ClPrintStats);
  SET_RUNTIME_FLAG(ClQuietErrors);
  SET_RUNTIME_FLAG(ClAbortOnError);
  SET_RUNTIME_FLAG(ClStrictMode);
  SET_RUNTIME_U32(ClStackIDsPerBucket);
  SET_RUNTIME_U32(ClStackIDThreadBuckets);
  SET_RUNTIME_U32(ClPatchAllWith);
}
#undef SET_RUNTIME_U32
#undef SET_RUNTIME_FLAG

// XXX: Is this necessary under LAM?
static bool addNoBuiltinForPointerFns(Module &M) {
  bool Modified = false;
  for (Function &F : M) {
    if (F.isIntrinsic()) {
      continue;
    }

    bool HasPtrParam = false;
    for (Type *ParamTy : F.getFunctionType()->params()) {
      if (ParamTy->isPointerTy()) {
        HasPtrParam = true;
        break;
      }
    }

    if (HasPtrParam) {
      F.addFnAttr(Attribute::NoBuiltin);
      // Prevents codegen from expanding these
      // into instructions, which *explodes*
      // XXX: Maybe figure out which ones
      // would be expanded and strip them properly
      if (!F.hasFnAttribute(Attribute::OptimizeNone)) {
        F.addFnAttr(Attribute::MinSize);
      }
      Modified = true;
    }
  }
  return Modified;
}

Value *findPTSanBasePointer(Value *Start, PTSanUtils &PTU) {
  if (ClNoPTSanOpts) {
    return Start;
  }

  return PTU.findBasePointer(Start);
}

bool PointerTagSanitizer::sanitizeModule() {
  bool Modified = false;

  SmallPtrSet<GlobalVariable *, 16> ToTag;
  SmallVector<GlobalVariable *, 16> GlobalsToHandle;
  for (auto &GV : M.globals()) {
    if (!ClInstrumentGlobals) { break; }
    if (isInstr(&GV)) { continue; }
    // a bit crude, but it works -- we don't want to be instrumenting llvm internals
    if (GV.getName().starts_with("llvm.")) { continue; }
    if (!GV.getValueType()->isAggregateType()) {continue;}
    // Only enforce stdlib and constant globa bounds in strict mode
    if (!ClStrictMode) {
      if (GV.isConstant()) {continue;}
      if (isStdLibraryGlobal(GV)) {continue;}
    }
    if (GV.hasInitializer()) {
      // non-debug-info-declared-globals are autogenerated -- we don't instrument them
      if (GV.getMetadata(LLVMContext::MD_dbg) == nullptr) { continue; }
      GlobalsToHandle.push_back(&GV);
      ToTag.insert(&GV);
    } else {
      GlobalsToHandle.push_back(&GV);
    }
  }

  LLVMContext &C = M.getContext();

  for (GlobalVariable *GV : GlobalsToHandle) {
    // errs() << "Creating proxy for " << *GV << "\n";
    Modified = true;

    GlobalValue::LinkageTypes proxyLink =
    ToTag.contains(GV) ? GV->getLinkage()
                        : GlobalValue::WeakAnyLinkage;

    auto *Proxy = new GlobalVariable(
          M,
          PtrTy, // It's a ptr to GV
          /*constant=*/false,
          proxyLink,
          ConstantPointerNull::get(cast<PointerType>(GV->getType())),
          GV->getName() + ".ptsan.ptr");
    // errs() << "\t" << *Proxy << "\n";
    GlobalVariableProxies[GV] = Proxy;
    GlobalProxyTargets[Proxy] = GV;
  }

  for (GlobalAlias &GA : M.aliases()) {
    // errs() << "Considering alias " << GA.getName() << " …\n";

    GlobalObject *AliaseeObj = GA.getAliaseeObject();
    while (isa<GlobalAlias>(AliaseeObj)) {
      AliaseeObj = cast<GlobalAlias>(AliaseeObj)->getAliaseeObject();
    }

    GlobalVariable *AliasedVar = dyn_cast<GlobalVariable>(AliaseeObj);
    if (!(AliasedVar && GlobalVariableProxies.contains(AliasedVar))) {
      continue;
    }

    // We don't just directly redirect to the underlyng aliased thing
    // because linkages can differ and doing that might cause hard-to-
    // diagnose Problems
    auto *Proxy = GlobalAlias::create(
      GA.getName() + ".ptsan.ptr",
      GlobalVariableProxies.at(AliasedVar));

    GlobalAliasProxies[&GA] = Proxy;
    GlobalProxyTargets[Proxy] = AliasedVar;
  }

  // Create the global constructor
  Function *InitTags =
    Function::Create(FunctionType::get(Type::getVoidTy(C), false),
                    GlobalValue::InternalLinkage,
                    "__ptsan_init_tagged_globals", &M);
  Function *InitExterns =
    Function::Create(FunctionType::get(Type::getVoidTy(C), false),
                    GlobalValue::InternalLinkage,
                    "__ptsan_init_extern_globals", &M);
  markAsInstr(InitTags);
  markAsInstr(InitExterns);

  PTSanIRB TagsIRB(*this, BasicBlock::Create(C, "entry", InitTags));
  PTSanIRB ExternsIRB(*this, BasicBlock::Create(C, "entry", InitExterns));

  // Once we're running constructors, we're not longer in libc
  // This is idempotent (sets to 0), so it doesn't matter if it
  // happens once per module
  ExternsIRB.CreateCall(EnableLibcInstr);

  for (auto &GI : GlobalVariableProxies) {
    GlobalVariable *Real = GI.first;
    GlobalVariable *Proxy = GI.second;
    if (ToTag.contains(Real)) {
      // errs() << "Creating proxy initializer for " << *Real << "\n";
      Value *Raw = TagsIRB.CreatePtrToInt(Real, U64Ty);
      uint64_t SizeBytes   = DL.getTypeAllocSize(Real->getValueType());
      Constant *Sz   = ConstantInt::get(U64Ty, SizeBytes);
      Value *Id16  = TagsIRB.CreateCall(GetNextGlobalID, {Real, Sz});
      Value *Id64 = TagsIRB.CreateZExt(Id16, U64Ty);
      Value *Tag = TagsIRB.CreateShl(Id64, TagsIRB.getInt64(48));
      Value *Sum = TagsIRB.CreateAdd(Raw, Tag);
      Value *TaggedPtr = TagsIRB.CreateIntToPtr(Sum, Real->getType());
      TagsIRB.CreateStore(TaggedPtr, Proxy);
    } else {
      // errs() << "Creating extern initializer for " << *Real << "\n";
      ExternsIRB.CreateStore(Real, Proxy);
    }
  }

  TagsIRB.CreateRetVoid();
  ExternsIRB.CreateRetVoid();

  appendToGlobalCtors(M, InitTags, /*priority=*/0x1);
  appendToGlobalCtors(M, InitExterns, /*priority=*/0x0);

  addNoBuiltinForPointerFns(M);
  return Modified;
}

bool PointerTagSanitizer::isAnchor(Value *V) const {
  auto *CI = dyn_cast<CallInst>(V);
  return CI && AnchorCallees.contains(CI->getCalledOperand());
}

Instruction *
PointerTagSanitizer::getNextRealInstruction(Anchor Anchor) const {
  Instruction *I = Anchor.Call->getNextNode();
  while (I) {
    if (isAnchor(I)) {
      I = I->getNextNode();
      continue;
    }

    return I;
  }

  return nullptr;
}

Instruction *
PointerTagSanitizer::getFirstRealInsertionPt(BasicBlock &BB) const {
  for (Instruction &I : make_range(BB.getFirstInsertionPt(), BB.end())) {
    if (!isAnchor(&I)) {
      return &I;
    }
  }

  return BB.getTerminator();
}

void PointerTagSanitizer::verifyNoAnchors() const {
  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        if (!isAnchor(&I)) {
          continue;
        }

        errs() << "PTSan anchor survived lowering: " << I << "\n";
        report_fatal_error("PTSan lowering left an anchor behind");
      }
    }
  }
}


static void runOptimizations(Module &M) {
  // 1) Set up the various analysis managers
  LoopAnalysisManager     LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager    CGAM;
  ModuleAnalysisManager   MAM;

  // 2) Create a PassBuilder and register analyses
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // 3) Build the default O2 pipeline (or O3, O1, etc.)
  ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);

  // 4) Run it on our module
  MPM.run(M, MAM);
}

} // namespace llvm::ptsan

PreservedAnalyses
PointerTagSanitizerPass::run(Module &M, ModuleAnalysisManager &AM) {
  using namespace llvm::ptsan;
  INFO(errs() << "Running PointerTagSanitizer on " << M.getName() << ":\n");

  DBG(errs() << "#################################\n"<< M << "\n##########################\n");
  PointerTagSanitizer PTSan(M);

  if(ClDisable) {
    INFO(llvm::errs() << "Disabled. Bailing\n");
    return PreservedAnalyses::all();
  }

  if (ClLAM && Triple(M.getTargetTriple()).getArch() != Triple::x86_64)
    report_fatal_error("PTSan LAM mode requires an x86-64 target");

  bool Modified = PTSan.sanitizeModule();

  auto shouldStopAfter = [](StringRef Phase) {
    if (ClStopAfter.empty())
      return false;
    if (ClStopAfter != "prepare" && ClStopAfter != "discovery" &&
        ClStopAfter != "optimization" &&
        ClStopAfter != "metadata-lowering" &&
        ClStopAfter != "late-optimization" && ClStopAfter != "lowering") {
      report_fatal_error(Twine("unknown -ptsan-stop-after phase: ") +
                         ClStopAfter);
    }
    return ClStopAfter == Phase;
  };

  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  PTSanPreparationPass Preparation;
  PTSanDiscoveryPass Discovery;
  PTSanOptimizationPass Optimization;
  PTSanMetadataLoweringPass MetadataLowering;
  PTSanLateOptimizationPass LateOptimization;
  PTSanLoweringPass Lowering;

  for (Function &F : M) {
    if (F.empty())
      continue;
    if (F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation))
      continue;
    if (PTSan.isInstr(&F))
      continue;

    DBG(errs() << "\n\n-----------------------------------------\n");
    INFO(errs() << "Running PTSan phases on " << F.getName() << "\n");
    DBG(errs() << "-----------------------------------------\n");

    PTSanAnalysisState PTA(F, PTSan, FAM.getResult<TargetLibraryAnalysis>(F));
    Modified |= Preparation.run(PTA);
    if (shouldStopAfter("prepare"))
      continue;

    Modified |= Discovery.run(PTA);
    if (shouldStopAfter("discovery"))
      continue;

    Modified |= Optimization.run(PTA);
    if (shouldStopAfter("optimization"))
      continue;

    Modified |= MetadataLowering.run(PTA);
    if (shouldStopAfter("metadata-lowering"))
      continue;

    Modified |= LateOptimization.run(PTA);
    if (shouldStopAfter("late-optimization"))
      continue;

    Modified |= Lowering.run(PTA);
  }

  if (shouldStopAfter("prepare") ||
      shouldStopAfter("discovery") ||
      shouldStopAfter("optimization") ||
      shouldStopAfter("metadata-lowering") ||
      shouldStopAfter("late-optimization")) {
    PTSan.finalize();
    return PreservedAnalyses::none();
  }

  PTSan.verifyNoAnchors();
  if (shouldStopAfter("lowering")) {
    PTSan.finalize();
    return PreservedAnalyses::none();
  }

  PTSan.finalize();
  INFO(errs() << "Done with basic ptsan\n");

  if (!Modified) {
    return PreservedAnalyses::all();
  }


  if (ClOptimizeAfterward && Modified) {
    INFO(errs() << "Re-running optimizations post-ptsan\n");
    runOptimizations(M);
    INFO(errs() << "Re-ran optimizations after ptsan\n");
  }

  PreservedAnalyses PA = PreservedAnalyses::none();
  // GlobalsAA is considered stateless and does not get invalidated unless
  // explicitly invalidated; PreservedAnalyses::none() is not enough. Sanitizers
  // make changes that require GlobalsAA to be invalidated.
  PA.abandon<GlobalsAA>();
  return PA;
}
