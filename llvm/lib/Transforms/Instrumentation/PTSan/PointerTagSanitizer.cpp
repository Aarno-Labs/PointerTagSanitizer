//===-- PointerTagSanitizer.cpp - Track bounds by tagging upper bits ------===//
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
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/MathExtras.h"
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

static cl::opt<bool> ClX86InlineStrip(
    "ptsan-x86-inline-strip", cl::Hidden, cl::init(true),
    cl::desc("Use inline AND for scalar PTSan strips on x86-64"));

cl::opt<bool> ClLAM(
  "ptsan-lam",
  cl::desc("Use LAM-compatible PTSan mode"),
  cl::ValueDisallowed,
  cl::init(false)
);

cl::opt<bool> Cl32BitIDs(
    "ptsan-32-bit-ids",
    cl::desc("Use 32-bit-ID pointer encoding with additive origins"),
    cl::init(false));

cl::opt<bool> ClSupportIndicatorBit(
    "ptsan-support-indicator-bit",
    cl::desc("Reserve the high ID bit for mixed-pointer decoding in strict ID32"),
    cl::init(false));
cl::opt<bool> ClMayDecodeRaw(
    "ptsan-may-decode-raw",
    cl::desc("Allow raw as well as encoded pointers in this file's ID32 conversions"),
    cl::init(false));

cl::opt<unsigned> ClRandomSeed(
    "ptsan-random-seed", cl::Hidden,
    cl::desc("Use a fixed PTSan random seed for testing"), cl::init(0));

cl::opt<bool> ClRandomizeIDs(
    "ptsan-randomize-ids", cl::Hidden,
    cl::desc("Randomize PTSan ID allocation"), cl::init(true));

cl::opt<unsigned> ClID32OriginQuantum(
    "ptsan-32-bit-ids-origin-quantum",
    cl::desc("Preserve address bits within this ID32 origin quantum in bytes"),
    cl::init(4096));

static unsigned getEffectiveIDBits() {
  if (Cl32BitIDs) {
    return 32;
  }
  return ClLAM ? 15 : 16;
}

cl::opt<bool> ClInstrumentGlobals(
  "ptsan-instrument-globals",
  cl::desc("Instrument Globals"),
  cl::init(true)
);

// XXX: This grants one ID access to the entire named section. If we use it for
// larger sections, consider randomizing global IDs to make forgery harder.
static cl::list<std::string> ClGlobalSections(
    "ptsan-global-section",
    cl::desc("Register an ELF section as one global through __start_/__stop_ "
             "symbols; specify only in its owning translation unit"),
    cl::ZeroOrMore);

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

cl::opt<bool> ClStripPtrToInt(
  "ptsan-strip-ptrtoint",
  cl::desc("Convert pointers to raw integer addresses (integer storage loses pointer IDs)"),
  cl::init(false)
);

cl::list<std::string> ClStripPtrToIntFunctions(
    "ptsan-strip-ptrtoint-function",
    cl::desc("Strip ptrtoint in the named function at PTSan discovery time "
             "(repeatable; does not follow previously inlined copies)"));

cl::list<std::string> ClClearIdsOnArithFunctions(
    "ptsan-clear-ids-on-arith-function",
    cl::desc("Strip pointer subtraction operands in the named function at "
             "PTSan discovery time (repeatable; does not follow previously "
             "inlined copies)"));

cl::list<std::string> ClClearIdsOnCmpFunctions(
    "ptsan-clear-ids-on-cmp-function",
    cl::desc("Strip pointer comparison operands in the named function at "
             "PTSan discovery time (repeatable; does not follow previously "
             "inlined copies)"));

cl::opt<int> ClStripPointerBitTwiddles(
  "ptsan-strip-pointer-bit-twiddles",
  cl::desc("Strip pointer IDs for address-bit operations, except within this "
           "many low bits (-1 disables)"),
  cl::init(-1)
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
  cl::desc("Disable PTSan check optimizations and the stack-ID fast path; "
           "pointer-polarity solving and conversion insertion still run"),
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
  cl::init(true)
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

cl::opt<unsigned> ClIDBucketSize(
  "ptsan-id-bucket-size",
  cl::desc("Power-of-two number of IDs per PTSan bucket (at least 64)")
);

static cl::opt<unsigned> ClStackIDLogCapacity(
  "ptsan-stack-id-log-capacity",
  cl::desc("Maximum outstanding runtime stack-ID log entries per thread")
);

cl::opt<unsigned> ClStackIDThreadBuckets(
  "ptsan-stack-id-thread-buckets",
  cl::desc("Number of threads that can hold a fast-path PTSan stack ID bucket (zero disables reservations)")
);

cl::opt<bool> ClAddOverflowChecks(
  "ptsan-add-overflow-checks",
  cl::desc("Use uadd.with.overflow to lower generated bounds checks"),
  cl::init(true)
);

cl::opt<unsigned> ClIDReuseDistance(
  "ptsan-id-reuse-distance",
  cl::desc("Target number of candidates for recycled-ID selection (default: up to 65536, limited by the ID namespace); capacity pressure may shorten it")
);

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



static void validateCLIArguments(unsigned IDBucketSize,
                                 unsigned StackIDThreadBuckets) {
  if (IDBucketSize < 64 || !isPowerOf2_64(IDBucketSize)) {
    report_fatal_error("PTSan ID bucket size must be a power of two and at least 64");
  }
  const uint64_t NumIDs = 1ULL << getEffectiveIDBits();
  const uint64_t IDSpaceBegin =
      Cl32BitIDs && (!ClStrictMode || ClSupportIndicatorBit) ? 0x80000000ULL : 0;
  if (StackIDThreadBuckets >
      (NumIDs - IDSpaceBegin) / IDBucketSize) {
    report_fatal_error("PTSan reserved stack ID buckets exceed ID space");
  }
  if (ClStackIDLogCapacity > (1ULL << getEffectiveIDBits())) {
    report_fatal_error("PTSan stack ID log capacity cannot exceed the ID space");
  }
  if (!isPowerOf2_64(ClID32OriginQuantum) ||
      ClID32OriginQuantum > (1ULL << 31)) {
    report_fatal_error(
        "PTSan ID32 origin quantum must be a power of two no larger than 2 GiB");
  }
  if (!ClGlobalSections.empty() && !ClInstrumentGlobals) {
    report_fatal_error("ptsan-global-section requires global instrumentation");
  }
  if (Cl32BitIDs && ClLAM) {
    report_fatal_error("PTSan 32-bit IDs and LAM are mutually exclusive");
  }
  if (ClSupportIndicatorBit && (!Cl32BitIDs || !ClStrictMode))
    report_fatal_error("ptsan-support-indicator-bit requires strict 32-bit IDs");
  if (ClMayDecodeRaw && !ClSupportIndicatorBit)
    report_fatal_error("ptsan-may-decode-raw requires ptsan-support-indicator-bit");
  if (ClStripPointerBitTwiddles < -1 || ClStripPointerBitTwiddles > 64) {
    report_fatal_error("PTSan pointer-bit threshold must be between -1 and 64");
  }
  if (!ClStopAfter.empty() && ClStopAfter != "prepare" &&
      ClStopAfter != "discovery" && ClStopAfter != "optimization" &&
      ClStopAfter != "metadata-lowering" && ClStopAfter != "late-optimization" &&
      ClStopAfter != "lowering") {
    report_fatal_error(Twine("unknown -ptsan-stop-after phase: ") + ClStopAfter);
  }
}

PointerTagSanitizer::PointerTagSanitizer(Module &M) :
  M(M), Ctx(M.getContext()), DL(M.getDataLayout()),  DIB(M, true) {
  StackIDLogCapacity = std::min(1ULL << 20, 1ULL << getEffectiveIDBits());
  if (ClStackIDLogCapacity.getNumOccurrences()) {
    StackIDLogCapacity = ClStackIDLogCapacity;
  }
  IDBucketSize = Cl32BitIDs ? 65536 : 512;
  if (ClIDBucketSize.getNumOccurrences()) {
    IDBucketSize = ClIDBucketSize;
  }
  StackIDThreadBuckets = Cl32BitIDs ? 128 : 12;
  if (ClStackIDThreadBuckets.getNumOccurrences()) {
    StackIDThreadBuckets = ClStackIDThreadBuckets;
  }
  validateCLIArguments(IDBucketSize, StackIDThreadBuckets);

  if (Cl32BitIDs) {
    ClAbortOnError = true;
  }

  auto *File = DIB.createFile("<ptsan>", "");
  CU = DIB.createCompileUnit(dwarf::DW_LANG_C, File,
                              "ptsan-instrumentation", /*Opt*/true, "", 0);

  PtrTy = PointerType::getUnqual(Ctx);
  UptrTy = DL.getIntPtrType(Ctx);
  U16Ty = IntegerType::get(Ctx, 16);
  U32Ty = IntegerType::get(Ctx, 32);
  IDTy = Cl32BitIDs ? U32Ty : U16Ty;
  U64Ty = IntegerType::get(Ctx, 64);
  U8Ty = IntegerType::get(Ctx, 8);
  BoolTy = IntegerType::get(Ctx, 1);

  AddrMask = ConstantInt::get(
      U64Ty, Cl32BitIDs ? 0x00000000FFFFFFFFULL : 0x0000FFFFFFFFFFFFULL);
  if (Cl32BitIDs) {
    IDMask = ConstantInt::get(U64Ty, 0xffffffff00000000ULL);
  } else {
    IDMask = ConstantInt::get(U64Ty, 0xffff000000000000ULL);
  }
  // Only non-strict ID16 allocation can fall back to a default ID.
  if (Cl32BitIDs || ClStrictMode) {
    DefaultIDNo = nullptr;
  } else {
    DefaultIDNo = ConstantInt::get(IDTy, (1U << getEffectiveIDBits()) - 1);
  }

  {
  SmallVector<Type*, 2> Fields = {
    IDTy, // Id
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

// Used by ID16 strip lowering; ID32 decoding does not use an address mask.
// X86 can select BEXTR/BZHI for our low-address mask. Inline AND has performed
// better for frequent PTSan tag strips, so prefer it independently of CPU
// features without changing instruction selection for application code.
bool PointerTagSanitizer::shouldUseInlineStrip() const {
  return ClX86InlineStrip &&
         Triple(M.getTargetTriple()).getArch() == Triple::x86_64;
}

void PointerTagSanitizer::markAsInstr(Value *V) {
  InstrVals[V] = true;
}

void PointerTagSanitizer::unmarkAsInstr(Value *V) {
  InstrVals.erase(V);
}

bool PointerTagSanitizer::isInstr(const Value *V) {
  return InstrVals.count(V);
}

void PointerTagSanitizer::initializeModule() {
  Type *VoidTy = Type::getVoidTy(Ctx);
  AttributeList AL;
  AL = AL.addFnAttribute(Ctx, Attribute::NoUnwind);
  AL = AL.addFnAttribute(Ctx, Attribute::WillReturn);

  InitVAListX86_64 = M.getOrInsertFunction(
      "ptsan_init_va_list_x86_64", AL, VoidTy, PtrTy, U64Ty, U64Ty, U64Ty);
  markAsInstr(InitVAListX86_64.getCallee());
  InitVAListAArch64 = M.getOrInsertFunction(
      "ptsan_init_va_list_aarch64", AL, VoidTy, PtrTy, U64Ty, U64Ty, U64Ty);
  markAsInstr(InitVAListAArch64.getCallee());

  ChooseStackOrigin = M.getOrInsertFunction(
      "ptsan_choose_stack_origin", AL, U32Ty, U64Ty);
  markAsInstr(ChooseStackOrigin.getCallee());

  RefillStackOrigins =
      M.getOrInsertFunction("ptsan_refill_stack_origins", AL, U32Ty);
  markAsInstr(RefillStackOrigins.getCallee());

  GetStackIDs = M.getOrInsertFunction("ptsan_get_stack_ids", AL,
    VoidTy, PtrTy, U64Ty);
  markAsInstr(GetStackIDs.getCallee()->stripPointerCasts());

  FreeStackIDs = M.getOrInsertFunction("ptsan_free_stack_ids", AL,
    VoidTy, U64Ty);
  markAsInstr(FreeStackIDs.getCallee()->stripPointerCasts());

  GetRepeatableAllocaID = M.getOrInsertFunction(
      "ptsan_get_repeatable_alloca_id", AL, IDTy, PtrTy, U64Ty, U64Ty);
  markAsInstr(GetRepeatableAllocaID.getCallee()->stripPointerCasts());

  RestoreStackIDs = M.getOrInsertFunction(
      "ptsan_restore_stack_ids", AL, VoidTy, U64Ty, U64Ty, IDTy, U8Ty);
  auto *RestoreStackIDsFn =
      cast<Function>(RestoreStackIDs.getCallee()->stripPointerCasts());
  RestoreStackIDsFn->addParamAttr(3, Attribute::ZExt);
  if (IDTy->isIntegerTy(16)) {
    RestoreStackIDsFn->addParamAttr(2, Attribute::ZExt);
  }
  markAsInstr(RestoreStackIDsFn);

  LogAllocaInfo = M.getOrInsertFunction("ptsan_log_alloca_info", AL,
    VoidTy, PtrTy, IDTy, BoolTy, BoolTy);
  // The C ABI zero-extends unsigned short IDs and bools, but not unsigned int IDs.
  auto *LogAllocaInfoFn =
      cast<Function>(LogAllocaInfo.getCallee()->stripPointerCasts());
  if (IDTy->isIntegerTy(16)) {
    LogAllocaInfoFn->addParamAttr(1, Attribute::ZExt);
  }
  for (unsigned Arg : {2, 3}) {
    LogAllocaInfoFn->addParamAttr(Arg, Attribute::ZExt);
  }
  markAsInstr(LogAllocaInfo.getCallee()->stripPointerCasts());

  LogCheckCalled = M.getOrInsertFunction("ptsan_log_check_called", AL, VoidTy, U32Ty);
  markAsInstr(LogCheckCalled.getCallee()->stripPointerCasts());

  GetNextGlobalID = M.getOrInsertFunction("ptsan_get_next_global_id", AL,
    IDTy, PtrTy, U64Ty, U64Ty);
  markAsInstr(GetNextGlobalID.getCallee()->stripPointerCasts());

  GetConstantGlobalID = M.getOrInsertFunction("ptsan_get_constant_global_id",
                                              AL, IDTy, PtrTy, U64Ty, U64Ty);
  markAsInstr(GetConstantGlobalID.getCallee()->stripPointerCasts());

  RegisterTLS = M.getOrInsertFunction("ptsan_register_tls", VoidTy,
                                     PtrTy, PtrTy, PtrTy);
  markAsInstr(RegisterTLS.getCallee());
  InitTLS = M.getOrInsertFunction("ptsan_initialize_tls", VoidTy,
                                 PtrTy, PtrTy, U64Ty, U64Ty);
  markAsInstr(InitTLS.getCallee());
  DestroyTLS = M.getOrInsertFunction("ptsan_destroy_tls", VoidTy, PtrTy, PtrTy);
  markAsInstr(DestroyTLS.getCallee());

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

  AttributeList ReportAttrs = AL.removeFnAttribute(Ctx, Attribute::WillReturn);
  if (ClAbortOnError) {
    ReportAttrs = ReportAttrs.addFnAttribute(Ctx, Attribute::NoReturn);
  }
  ReportGenericError = M.getOrInsertFunction("ptsan_complain", ReportAttrs,
    VoidTy, // return type: void
    U32Ty,  // check_id
    U64Ty,  // function_id
    U64Ty,  // ptr_id
    PtrTy,  // type (const char*)
    U64Ty,  // min
    U64Ty   // size
  );

  ReportVectorError = M.getOrInsertFunction("ptsan_complain_vector", ReportAttrs,
    VoidTy, // return type: void
    U32Ty,  // check_id
    U64Ty   // function_id
  );
  for (FunctionCallee Callee : {ReportGenericError, ReportVectorError}) {
    auto *Report = cast<Function>(Callee.getCallee()->stripPointerCasts());
    Report->setAttributes(ReportAttrs);
    markAsInstr(Report);
  }

  EnableLibcInstr = M.getOrInsertFunction("ptsan_enable_libc_instr", AL, VoidTy);
  markAsInstr(EnableLibcInstr.getCallee()->stripPointerCasts());

  ValidateFunctionStart = M.getOrInsertFunction("ptsan_validate_function_start", AL, VoidTy);
  markAsInstr(ValidateFunctionStart.getCallee()->stripPointerCasts());

  FunctionType *VoidOperationTy =
      FunctionType::get(VoidTy, /*isVarArg=*/true);
  PTSanSource =
      M.getOrInsertFunction("__ptsan_source", AL, PtrTy, PtrTy);
  PTSanAccess = M.getOrInsertFunction("__ptsan_access", VoidOperationTy, AL);
  PTSanLoopPreanalysis =
      M.getOrInsertFunction("__ptsan_loop_preanalysis", VoidOperationTy, AL);
  PTSanGetID =
      M.getOrInsertFunction("__ptsan_get_id", AL, U64Ty, PtrTy);
  PTSanLoadBase =
      M.getOrInsertFunction("__ptsan_load_base", AL, U64Ty, U64Ty);
  PTSanLoadSpan =
      M.getOrInsertFunction("__ptsan_load_span", AL, U64Ty, U64Ty);
  PTSanLoadOrigin =
      M.getOrInsertFunction("__ptsan_load_origin", AL, U32Ty, U64Ty);
  PTSanStaticBase =
      M.getOrInsertFunction("__ptsan_static_base", AL, U64Ty, PtrTy);
  PTSanStaticSpan =
      M.getOrInsertFunction("__ptsan_static_span", AL, U64Ty, U64Ty);
  PTSanID16Strip =
      M.getOrInsertFunction("__ptsan_strip", AL, PtrTy, PtrTy);
  PTSanID16StripI64 =
      M.getOrInsertFunction("__ptsan_strip_i64", AL, U64Ty, U64Ty);
  PTSanID32Strip =
      M.getOrInsertFunction("__ptsan_strip_id32", AL, PtrTy, PtrTy, U64Ty, U64Ty);
  PTSanID32StripI64 = M.getOrInsertFunction(
      "__ptsan_strip_i64_id32", AL, U64Ty, U64Ty, U64Ty, U64Ty);
  PTSanTagged =
      M.getOrInsertFunction("__ptsan_tagged", AL, PtrTy, PtrTy);
  PTSanID16Tag =
      M.getOrInsertFunction("__ptsan_tag", AL, PtrTy, PtrTy, U64Ty);
  PTSanID32Tag =
      M.getOrInsertFunction("__ptsan_tag_id32", AL, PtrTy, PtrTy, U64Ty, U64Ty);
  PTSanFunctionExit =
      M.getOrInsertFunction("__ptsan_function_exit", VoidOperationTy, AL);
  PTSanSetjmp =
      M.getOrInsertFunction("__ptsan_setjmp", VoidOperationTy, AL);
  PTSanVAStart =
      M.getOrInsertFunction("__ptsan_va_start", VoidOperationTy, AL);
  PTSanVarargCall =
      M.getOrInsertFunction("__ptsan_vararg_call", VoidOperationTy, AL);
  PTSanValidateArith =
      M.getOrInsertFunction("__ptsan_validate_arith", VoidOperationTy, AL);
  PTSanLibcCall =
      M.getOrInsertFunction("__ptsan_libc_call", VoidOperationTy, AL);
  PTSanSpecialStrip =
      M.getOrInsertFunction("__ptsan_special_strip", VoidOperationTy, AL);

  for (FunctionCallee Callee :
       {PTSanSource, PTSanAccess, PTSanLoopPreanalysis, PTSanGetID,
        PTSanLoadBase, PTSanLoadSpan, PTSanLoadOrigin, PTSanStaticBase, PTSanStaticSpan,
        PTSanID16Strip, PTSanID16StripI64, PTSanID32Strip, PTSanID32StripI64,
        PTSanTagged, PTSanID16Tag, PTSanID32Tag,
        PTSanFunctionExit, PTSanSetjmp, PTSanVAStart, PTSanVarargCall,
        PTSanValidateArith, PTSanLibcCall,
        PTSanSpecialStrip}) {
    Value *V = Callee.getCallee();
    // getOrInsertFunction leaves attributes on existing declarations alone.
    cast<Function>(V)->setDoesNotThrow();
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

  PTSanVarargSentinel = cast<GlobalValue>(
      M.getOrInsertGlobal("__ptsan_va_sentinel", ArrayType::get(U64Ty, 2)));
  markAsInstr(PTSanVarargSentinel);

  auto CreateMetadataArray = [&](StringRef Name, Type *ElementTy) {
    Type *TableTy;
    if (Cl32BitIDs) {
      TableTy = PtrTy;
    } else {
      TableTy = ArrayType::get(ElementTy,
                              1ULL << std::max(getEffectiveIDBits(), 16U));
    }
    auto *Array = cast<GlobalVariable>(M.getOrInsertGlobal(Name, TableTy));
    Array->setLinkage(GlobalValue::ExternalLinkage);
    markAsInstr(Array);
    return Array;
  };
  TypesArray = CreateMetadataArray("ptsan_obj_types", U8Ty);
  BoundsArray = CreateMetadataArray("ptsan_bounds", BoundsStructTy);
  OriginsArray = Cl32BitIDs ? CreateMetadataArray("ptsan_origins", U32Ty) : nullptr;

  if (Cl32BitIDs && ClStackTemporalSafety) {
    StackOriginNextPtr = cast<GlobalValue>(
        M.getOrInsertGlobal("ptsan_stack_origin_next", PtrTy));
    StackOriginNextPtr->setLinkage(GlobalValue::ExternalLinkage);
    StackOriginNextPtr->setThreadLocalMode(GlobalVariable::InitialExecTLSModel);
    markAsInstr(StackOriginNextPtr);
  }

  OverflowIDBoundsPtr = cast<GlobalValue>(
    M.getOrInsertGlobal("ptsan_default_id_bounds", BoundsStructTy));
  OverflowIDBoundsPtr->setLinkage(GlobalValue::ExternalLinkage);
  markAsInstr(OverflowIDBoundsPtr);

  NextStackIDPtr = cast<GlobalValue>(
    M.getOrInsertGlobal("ptsan_next_stack_id", U64Ty));
  NextStackIDPtr->setLinkage(GlobalValue::ExternalLinkage);
  NextStackIDPtr->setThreadLocalMode(GlobalVariable::InitialExecTLSModel);
  markAsInstr(NextStackIDPtr);

  // StackIDLog begins with its cursor, followed by the runtime's storage
  // pointer. Loading the first word keeps checkpoints a direct TLS load.
  StackIDCursorPtr = cast<GlobalValue>(M.getOrInsertGlobal(
      "ptsan_stack_id_log", StructType::get(U64Ty, PtrTy)));
  StackIDCursorPtr->setLinkage(GlobalValue::ExternalLinkage);
  StackIDCursorPtr->setThreadLocalMode(GlobalVariable::InitialExecTLSModel);
  markAsInstr(StackIDCursorPtr);

  // Longjmp checkpoints preserve this counter even when fast-path returns
  // are allowed to omit temporal cleanup.
  PendingStackIdsPtr = cast<GlobalValue>(
    M.getOrInsertGlobal("ptsan_pending_stack_ids", IDTy));
  PendingStackIdsPtr->setLinkage(GlobalValue::ExternalLinkage);
  PendingStackIdsPtr->setThreadLocalMode(GlobalVariable::InitialExecTLSModel);
  markAsInstr(PendingStackIdsPtr);

  InSignalContextPtr = cast<GlobalValue>(
      M.getOrInsertGlobal("ptsan_in_signal_context", U8Ty));
  InSignalContextPtr->setLinkage(GlobalValue::ExternalLinkage);
  InSignalContextPtr->setThreadLocalMode(GlobalVariable::InitialExecTLSModel);
  markAsInstr(InSignalContextPtr);

  PTSanInLibc = cast<GlobalValue>(
    M.getOrInsertGlobal("ptsan_in_libc", U32Ty));
  PTSanInLibc->setLinkage(GlobalValue::ExternalLinkage);
  PTSanInLibc->setThreadLocal(true);
  PTSanInLibc->setThreadLocalMode(GlobalVariable::GeneralDynamicTLSModel);
  markAsInstr(PTSanInLibc);

  StackIDLimitPtr = cast<GlobalValue>(
    M.getOrInsertGlobal("ptsan_stack_id_limit", U64Ty));
  StackIDLimitPtr->setLinkage(GlobalValue::ExternalLinkage);
  StackIDLimitPtr->setThreadLocal(true);

  markAsInstr(StackIDLimitPtr);

}

Value *PointerTagSanitizer::getBoundsPtr(PTSanIRB &IRB, Value *ID,
                                        optional<unsigned> Field) {
  if (Cl32BitIDs) {
    ID = IRB.CreateZExtOrTrunc(ID, ID->getType()->getWithNewType(U64Ty));
    Value *Table = IRB.CreateLoad(PtrTy, BoundsArray);
    SmallVector<Value *> Indices = {ID};
    if (Field) {
      Indices.push_back(IRB.getInt32(*Field));
    }
    return IRB.CreateInBoundsGEP(BoundsStructTy, Table, Indices);
  }
  SmallVector<Value *> Indices = {IRB.getInt64(0), ID};
  if (Field) {
    Indices.push_back(IRB.getInt32(*Field));
  }
  return IRB.CreateInBoundsGEP(BoundsArray->getValueType(), BoundsArray, Indices);
}

Value *PointerTagSanitizer::getOriginPtr(PTSanIRB &IRB, Value *ID) {
  ID = IRB.CreateZExtOrTrunc(ID, ID->getType()->getWithNewType(U64Ty));
  return IRB.CreateInBoundsGEP(U32Ty, IRB.CreateLoad(PtrTy, OriginsArray), ID);
}

#define SET_RUNTIME_FLAG(Name)                                                 \
  {                                                                            \
    GlobalVariable *GV =                                                       \
        cast<GlobalVariable>(M.getOrInsertGlobal(#Name, U32Ty));               \
    Constant *NewInit = ConstantInt::get(GV->getValueType(), Name ? 1 : 0);    \
    GV->setInitializer(NewInit);                                               \
    GV->setLinkage(GlobalValue::WeakODRLinkage);                               \
    GV->setConstant(true);                                                     \
    markAsInstr(GV);                                                           \
  }

#define SET_RUNTIME_U32(Name, Value)                                           \
  {                                                                            \
    GlobalVariable *GV =                                                       \
        cast<GlobalVariable>(M.getOrInsertGlobal(#Name, U32Ty));               \
    Constant *NewInit = ConstantInt::get(GV->getValueType(), Value);           \
    GV->setInitializer(NewInit);                                               \
    GV->setLinkage(GlobalValue::WeakODRLinkage);                               \
    GV->setConstant(true);                                                     \
    markAsInstr(GV);                                                           \
  }

void PointerTagSanitizer::setRuntimeFlags() {
  SET_RUNTIME_FLAG(ClLAM);
  SET_RUNTIME_FLAG(ClUseReturnedMallocSize);
  SET_RUNTIME_FLAG(ClValidate);
  SET_RUNTIME_FLAG(ClPrintStats);
  SET_RUNTIME_FLAG(ClQuietErrors);
  SET_RUNTIME_FLAG(ClAbortOnError);
  SET_RUNTIME_FLAG(ClStrictMode);
  SET_RUNTIME_FLAG(ClRandomizeIDs);
  SET_RUNTIME_U32(ClRandomSeed, ClRandomSeed);
  SET_RUNTIME_U32(ClIDBucketSize, IDBucketSize);
  SET_RUNTIME_U32(ClStackIDLogCapacity, StackIDLogCapacity);
  SET_RUNTIME_U32(ClStackIDThreadBuckets, StackIDThreadBuckets);
  SET_RUNTIME_U32(ClIDReuseDistance, ClIDReuseDistance.getNumOccurrences()
                                       ? (unsigned)ClIDReuseDistance
                                       : std::min<uint64_t>(
                                             65536, (1ULL << getEffectiveIDBits()) -
                                                        (Cl32BitIDs ? 1 : 2)));
  SET_RUNTIME_U32(ClPatchAllWith, ClPatchAllWith);
  if (Cl32BitIDs) {
    SET_RUNTIME_FLAG(ClSupportIndicatorBit);
    SET_RUNTIME_U32(ClID32OriginQuantum, ClID32OriginQuantum);
  }
}
#undef SET_RUNTIME_U32
#undef SET_RUNTIME_FLAG

Value *findPTSanBasePointer(Value *Start, PTSanUtils &PTU) {
  if (ClNoPTSanOpts) {
    return Start;
  }

  return PTU.findBasePointer(Start);
}

GlobalValue *PointerTagSanitizer::getGlobalProxy(Value *V) const {
  if (auto *GV = dyn_cast<GlobalVariable>(V)) {
    if (GlobalVariableProxies.contains(GV)) {
      return GlobalVariableProxies.at(GV);
    }
    return nullptr;
  }

  auto *GA = dyn_cast<GlobalAlias>(V);
  if (GA && GlobalAliasProxies.contains(GA)) {
    return GlobalAliasProxies.at(GA);
  }
  return nullptr;
}

static bool isLLVMInternalGlobal(const GlobalVariable &GV) {
  // Annotation strings can have ordinary names but are not emitted as storage.
  return GV.getName().starts_with("llvm.") || GV.getSection() == "llvm.metadata";
}

static SmallVector<std::pair<GlobalVariable *, GlobalVariable *>, 2>
createGlobalSectionBoundaries(
    Module &M, SmallPtrSetImpl<GlobalVariable *> &SectionBoundaries) {
  SmallVector<std::pair<GlobalVariable *, GlobalVariable *>, 2> GlobalSections;
  for (StringRef Section : ClGlobalSections) {
    SmallVector<GlobalVariable *, 2> Boundaries;
    for (StringRef Prefix : {"__start_", "__stop_"}) {
      std::string Name = (Prefix + Section).str();
      GlobalVariable *Boundary;
      if (GlobalValue *Existing = M.getNamedValue(Name)) {
        Boundary = dyn_cast<GlobalVariable>(Existing);
        if (!Boundary || !Boundary->isDeclaration() || Boundary->isThreadLocal() ||
            !Boundary->hasExternalLinkage()) {
          report_fatal_error("PTSan section boundary must be an external global");
        }
      } else {
        Boundary = new GlobalVariable(M, Type::getInt8Ty(M.getContext()), true,
                                      GlobalValue::ExternalLinkage, nullptr, Name);
      }
      SectionBoundaries.insert(Boundary);
      Boundaries.push_back(Boundary);
    }
    GlobalSections.emplace_back(Boundaries[0], Boundaries[1]);
  }
  return GlobalSections;
}

bool PointerTagSanitizer::sanitizeModule() {
  bool Modified = false;

  // References to actual memory functions (as opposed to intrinsics)
  // are redirected to checking (or stripping) versions. The 'default'
  // versions are raw, because intrinsic lowering can produce them and
  // intrinsics are already checked/stripped.
  if (Triple(M.getTargetTriple()).isMusl()) {
    StringRef Prefix = "__wrap_";
    if (ClNoChecks) {
      Prefix = ClMayDecodeRaw ? "ptsan_raw_strip_" : "ptsan_strip_";
    }
    for (StringRef Name : {"memcpy", "memmove", "memset"}) {
      Function *F = M.getFunction(Name);
      if (!F || !F->isDeclaration() || F->use_empty()) {
        continue;
      }
      FunctionCallee Wrapper = M.getOrInsertFunction(
          (Prefix + Name).str(), F->getFunctionType());
      F->replaceAllUsesWith(Wrapper.getCallee());
      Modified = true;
    }
    // Explicit C calls carry an encoded descriptor. Backend-generated TLS
    // calls instead use the native ABI and must keep the raw resolver symbol.
    if (Function *F = M.getFunction("__tls_get_addr");
        F && F->isDeclaration() && !F->use_empty()) {
      FunctionCallee Wrapper = M.getOrInsertFunction(
          "ptsan_musl_tls_get_addr", F->getFunctionType());
      F->replaceAllUsesWith(Wrapper.getCallee());
      Modified = true;
    }
  }

  SmallPtrSet<GlobalVariable *, 4> SectionBoundaries;
  auto GlobalSections = createGlobalSectionBoundaries(M, SectionBoundaries);

  SmallPtrSet<GlobalVariable *, 16> ToTag;
  SmallVector<GlobalVariable *, 16> GlobalsToHandle;
  for (auto &GV : M.globals()) {
    if (!ClInstrumentGlobals) { break; }
    if (isInstr(&GV)) { continue; }
    if (isLLVMInternalGlobal(GV)) { continue; }
    if (GV.hasAvailableExternallyLinkage()) {
      // This initializer is only an optimization copy. The definition is in
      // another module, so use an external proxy and do not replay or tag it.
      GV.setInitializer(nullptr);
      GV.setLinkage(GlobalValue::ExternalLinkage);
      Modified = true;
    }
    if (GV.hasInitializer()) {
      if (!GV.getValueType()->isSized()) {
        report_fatal_error("PTSan cannot instrument an unsized global");
      }
      GlobalsToHandle.push_back(&GV);
      ToTag.insert(&GV);
    } else {
      GlobalsToHandle.push_back(&GV);
    }
  }

  // A global initializer is constant data, so references to globals represented
  // by proxies cannot load those runtime-initialized proxies. Find the
  // containing globals now and replay their initializers from an ordinary
  // function after all proxies have been initialized. The normal PTSan function
  // pipeline will materialize the constant aggregates and redirect their global
  // references through the proxies.
  SmallSetVector<GlobalVariable *, 16> GlobalsToReinitialize;
  for (GlobalVariable *Target : GlobalsToHandle) {
    SmallVector<Constant *, 16> Worklist;
    SmallPtrSet<Constant *, 32> Visited;
    for (User *U : Target->users()) {
      if (auto *C = dyn_cast<Constant>(U)) {
        Worklist.push_back(C);
      }
    }

    while (!Worklist.empty()) {
      Constant *C = Worklist.pop_back_val();
      if (!Visited.insert(C).second) {
        continue;
      }

      if (auto *Owner = dyn_cast<GlobalVariable>(C)) {
        // ELF constants are made writable for replay and sealed afterward.
        if (Owner->hasInitializer() && !Owner->isThreadLocal() &&
            !isLLVMInternalGlobal(*Owner)) {
          GlobalsToReinitialize.insert(Owner);
        }
        continue;
      }

      for (User *U : C->users()) {
        if (auto *Next = dyn_cast<Constant>(U)) {
          Worklist.push_back(Next);
        }
      }
    }
  }

  bool HasReadOnlyInitializers = false;
  for (GlobalVariable *Owner : GlobalsToReinitialize) {
    if (Owner->isConstant()) {
      Owner->setConstant(false);
      Owner->setSection(".ptsan_init_ro");
      HasReadOnlyInitializers = true;
    }
  }

  LLVMContext &C = M.getContext();

  for (GlobalVariable *GV : GlobalsToHandle) {
    // errs() << "Creating proxy for " << *GV << "\n";
    Modified = true;

    GlobalValue::LinkageTypes proxyLink =
    ToTag.contains(GV) ? GV->getLinkage()
                        : GlobalValue::WeakAnyLinkage;
    if (SectionBoundaries.contains(GV)) {
      // One owner supplies the tagged proxies; other TUs retain weak extern
      // proxies. Multiple owners deliberately produce a duplicate-symbol error.
      proxyLink = GlobalValue::ExternalLinkage;
    }
    // Common linkage requires a zero initializer, but the proxy starts with
    // the global's address. Weak linkage still coalesces duplicate proxies;
    // the original global retains its common linkage.
    if (proxyLink == GlobalValue::CommonLinkage) {
      proxyLink = GlobalValue::WeakAnyLinkage;
    }

    if (GV->isThreadLocal()) {
      TLSIDs[GV] = new GlobalVariable(
          M, IDTy, false, proxyLink, ConstantInt::get(IDTy, 0),
          GV->getName() + ".ptsan.id", nullptr, GV->getThreadLocalMode());
      continue;
    }

    auto *Proxy = new GlobalVariable(
          M,
          PtrTy, // It's a ptr to GV
          /*constant=*/false,
          proxyLink,
          GV, // Bootstrap uses the raw address until the proxy constructor runs.
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
    if (AliasedVar && TLSIDs.contains(AliasedVar)) {
      auto *ID = GlobalAlias::create(GA.getLinkage(),
                                    GA.getName() + ".ptsan.id",
                                    TLSIDs.at(AliasedVar));
      ID->copyAttributesFrom(&GA);
      TLSIDs[&GA] = ID;
      continue;
    }
    if (!(AliasedVar && GlobalVariableProxies.contains(AliasedVar))) {
      continue;
    }

    // The proxy must follow the alias's linker resolution, not its aliasee's.
    auto *Proxy = GlobalAlias::create(
      GA.getLinkage(), GA.getName() + ".ptsan.ptr",
      GlobalVariableProxies.at(AliasedVar));
    Proxy->copyAttributesFrom(&GA);

    GlobalAliasProxies[&GA] = Proxy;
    if (!GA.isInterposable()) {
      GlobalProxyTargets[Proxy] = AliasedVar;
    }
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
  Function *InitInitializers =
    Function::Create(FunctionType::get(Type::getVoidTy(C), false),
                    GlobalValue::InternalLinkage,
                    "__ptsan_init_global_initializer_pointers", &M);
  markAsInstr(InitTags);
  markAsInstr(InitExterns);

  PTSanIRB TagsIRB(*this, BasicBlock::Create(C, "entry", InitTags));
  PTSanIRB ExternsIRB(*this, BasicBlock::Create(C, "entry", InitExterns));

  const bool EarlyGlobals = Triple(M.getTargetTriple()).isMusl();
  if (EarlyGlobals)
    InitInitializers->addFnAttr("ptsan-pre-tls");

  Function *TLSExterns = nullptr;
  Function *TLSTags = nullptr;
  Function *TLSCleanup = nullptr;
  PTSanIRB TLSExternsIRB(*this, C);
  PTSanIRB TLSTagsIRB(*this, C);
  PTSanIRB TLSCleanupIRB(*this, C);
  if (llvm::any_of(GlobalsToHandle, [](GlobalVariable *GV) {
        return GV->isThreadLocal();
      })) {
    auto CreateTLSFunction = [&](StringRef Name) {
      auto *F = Function::Create(FunctionType::get(Type::getVoidTy(C), false),
                                 GlobalValue::InternalLinkage, Name, M);
      markAsInstr(F);
      BasicBlock::Create(C, "entry", F);
      return F;
    };
    TLSExterns = CreateTLSFunction("__ptsan_init_extern_tls");
    TLSTags = CreateTLSFunction("__ptsan_init_tagged_tls");
    TLSCleanup = CreateTLSFunction("__ptsan_destroy_tls");
    TLSExternsIRB.SetInsertPoint(&TLSExterns->getEntryBlock());
    TLSTagsIRB.SetInsertPoint(&TLSTags->getEntryBlock());
    TLSCleanupIRB.SetInsertPoint(&TLSCleanup->getEntryBlock());
    ExternsIRB.CreateCall(RegisterTLS, {TLSExterns, TLSTags, TLSCleanup});
    if (!EarlyGlobals) {
      ExternsIRB.CreateCall(TLSExterns);
      TagsIRB.CreateCall(TLSTags);
    }
  }

  // Once we're running constructors, we're not longer in libc
  // This is idempotent (sets to 0), so it doesn't matter if it
  // happens once per module
  if (!EarlyGlobals)
    ExternsIRB.CreateCall(EnableLibcInstr);

  auto StoreTaggedGlobal = [&](GlobalVariable *Real, Value *ID) {
    Value *Raw = TagsIRB.CreatePtrToInt(Real, U64Ty);
    Value *ID64 = TagsIRB.CreateZExt(ID, U64Ty);
    Value *Tag = TagsIRB.CreateShl(ID64, TagsIRB.getInt64(Cl32BitIDs ? 32 : 48));
    Value *Sum;
    if (Cl32BitIDs) {
      // Shared globals and section endpoints need not be the registered base.
      Value *Bias = TagsIRB.CreateLoad(U64Ty, getBoundsPtr(TagsIRB, ID64, 0));
      Value *Payload = TagsIRB.CreateSub(Raw, Bias);
      Sum = TagsIRB.CreateOr(Tag, Payload);
      if (!ClStrictMode) {
        Sum = TagsIRB.CreateSelect(TagsIRB.CreateICmpEQ(ID64, TagsIRB.getInt64(0)),
                                  Raw, Sum);
      }
    } else {
      Sum = TagsIRB.CreateAdd(Raw, Tag);
    }
    Value *TaggedPtr = TagsIRB.CreateIntToPtr(Sum, Real->getType());
    TagsIRB.CreateStore(TaggedPtr, GlobalVariableProxies.at(Real));
  };

  for (auto [Start, Stop] : GlobalSections) {
    Value *Size = TagsIRB.CreateSub(TagsIRB.CreatePtrToInt(Stop, U64Ty),
                                  TagsIRB.CreatePtrToInt(Start, U64Ty));
    Value *ID = TagsIRB.CreateCall(GetNextGlobalID,
                                  {Start, Size, TagsIRB.getInt64(1)});
    StoreTaggedGlobal(Start, ID);
    StoreTaggedGlobal(Stop, ID);
  }

  for (GlobalVariable *Real : GlobalsToHandle) {
    if (SectionBoundaries.contains(Real)) {
      continue;
    }
    if (Real->isThreadLocal()) {
      GlobalValue *ID = TLSIDs.at(Real);
      // Match global constructor ordering: all zero-ID fallbacks precede IDs.
      TLSExternsIRB.CreateStore(ConstantInt::get(IDTy, 0), ID);
      if (ToTag.contains(Real)) {
        TLSTagsIRB.CreateCall(InitTLS,
            {ID, Real, TLSTagsIRB.getInt64(
                              DL.getTypeAllocSize(Real->getValueType())),
             TLSTagsIRB.getInt64(DL.getValueOrABITypeAlignment(
                 Real->getAlign(), Real->getValueType()).value())});
        TLSCleanupIRB.CreateCall(DestroyTLS, {ID, Real});
      }
      continue;
    }
    GlobalVariable *Proxy = GlobalVariableProxies.at(Real);
    if (ToTag.contains(Real)) {
      // errs() << "Creating proxy initializer for " << *Real << "\n";
      uint64_t SizeBytes   = DL.getTypeAllocSize(Real->getValueType());
      Constant *Sz   = ConstantInt::get(U64Ty, SizeBytes);
      FunctionCallee IDGetter =
          Real->isConstant() ? GetConstantGlobalID : GetNextGlobalID;
      Value *ID = TagsIRB.CreateCall(
          IDGetter, {Real, Sz, TagsIRB.getInt64(DL.getValueOrABITypeAlignment(
                                  Real->getAlign(), Real->getValueType()).value())});
      StoreTaggedGlobal(Real, ID);
    } else {
      // errs() << "Creating extern initializer for " << *Real << "\n";
      ExternsIRB.CreateStore(Real, Proxy);
    }
  }

  TagsIRB.CreateRetVoid();
  ExternsIRB.CreateRetVoid();
  if (TLSExterns) {
    TLSExternsIRB.CreateRetVoid();
    TLSTagsIRB.CreateRetVoid();
    TLSCleanupIRB.CreateRetVoid();
  }

  IRBuilder<> InitializersIRB(
      BasicBlock::Create(C, "entry", InitInitializers));
  for (GlobalVariable *Owner : GlobalsToReinitialize) {
    InitializersIRB.CreateStore(Owner->getInitializer(), Owner);
  }
  InitializersIRB.CreateRetVoid();

  if (EarlyGlobals) {
    // Musl's raw startup wrapper runs each phase across all modules before TLS.
    // TLS callbacks are registered here but run only after musl establishes TLS.
    auto *RecordTy = StructType::get(PtrTy, PtrTy, PtrTy);
    auto *Record = new GlobalVariable(
        M, RecordTy, true, GlobalValue::InternalLinkage,
        ConstantStruct::get(RecordTy, {InitExterns, InitTags, InitInitializers}),
        "__ptsan_global_init");
    Record->setSection("ptsan_global_init");
    Record->setAlignment(DL.getPointerABIAlignment(0));
    markAsInstr(Record);
    appendToUsed(M, {Record});
  } else {
    appendToGlobalCtors(M, InitTags, /*priority=*/0x1);
    appendToGlobalCtors(M, InitExterns, /*priority=*/0x0);
    appendToGlobalCtors(M, InitInitializers, /*priority=*/0x2);
  }

  if (HasReadOnlyInitializers) {
    // The linker isolates these globals on whole pages. Match ASan's ctor
    // COMDAT pattern so there is one sealing callback per linked image, after
    // every module has replayed its initializers.
    Type *VoidTy = Type::getVoidTy(C);
    auto *Seal = Function::Create(FunctionType::get(VoidTy, false),
                                 GlobalValue::LinkOnceODRLinkage,
                                 "__ptsan_seal_global_initializers", M);
    Seal->setVisibility(GlobalValue::HiddenVisibility);
    Seal->setComdat(M.getOrInsertComdat(Seal->getName()));
    markAsInstr(Seal);
    IRBuilder<> IRB(BasicBlock::Create(C, "entry", Seal));
    auto GetBoundary = [&](StringRef Name) {
      auto *GV = cast<GlobalVariable>(M.getOrInsertGlobal(Name, U8Ty));
      GV->setVisibility(GlobalValue::HiddenVisibility);
      markAsInstr(GV);
      return GV;
    };
    FunctionCallee Protect = M.getOrInsertFunction(
        "ptsan_protect_global_initializers", VoidTy, PtrTy, PtrTy);
    markAsInstr(Protect.getCallee()->stripPointerCasts());
    IRB.CreateCall(Protect,
                   {GetBoundary("__ptsan_init_ro_start"),
                    GetBoundary("__ptsan_init_ro_end")});
    IRB.CreateRetVoid();
    appendToGlobalCtors(M, Seal, /*priority=*/0x3, Seal);
  }

  addNoBuiltinForPointerFns(M);
  return Modified;
}

bool PointerTagSanitizer::isAnchor(Value *V) const {
  auto *CI = dyn_cast<CallInst>(V);
  // Loop preheader markers call llvm.sideeffect rather than a PTSan runtime
  // function, so AnchorCallees cannot recognize them.
  return CI && (AnchorCallees.contains(CI->getCalledOperand()) ||
                LoopPreheaderAnchor::isMarker(*CI));
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


static void runOptimizations(Module &M, IntrusiveRefCntPtr<vfs::FileSystem> FS) {
  // 1) Set up the various analysis managers
  LoopAnalysisManager     LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager    CGAM;
  ModuleAnalysisManager   MAM;

  // 2) Create a PassBuilder and register analyses
  PassBuilder PB(nullptr, PipelineTuningOptions(), std::nullopt, nullptr,
                 std::move(FS));
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

void validatePTSanTarget(Module &M) {
  Triple Target(M.getTargetTriple());
  StringRef Error;
  if (M.getDataLayout().getPointerSizeInBits() != 64 || Target.isX32()) {
    Error = "PTSan requires 64-bit pointers";
  } else if ((Target.getArch() != Triple::x86_64 && !Target.isAArch64()) ||
             !Target.isOSLinux() || !Target.isOSBinFormatELF()) {
    Error = "PTSan requires an x86-64 or AArch64 Linux ELF target";
  } else if (ClLAM && Target.getArch() != Triple::x86_64) {
    Error = "PTSan LAM mode requires an x86-64 target";
  } else {
    return;
  }

  M.getContext().emitError(Twine(Error) + "; target '" +
                           M.getTargetTriple().str() + "' is unsupported");
  exit(EXIT_FAILURE);
}

} // namespace llvm::ptsan

PreservedAnalyses
PointerTagSanitizerPass::run(Module &M, ModuleAnalysisManager &AM) {
  using namespace llvm::ptsan;
  INFO(errs() << "Running PointerTagSanitizer on " << M.getName() << ":\n");

  DBG(errs() << "#################################\n"<< M << "\n##########################\n");
  // Reject unsupported targets before the constructor modifies the module.
  if (!ClDisable)
    validatePTSanTarget(M);

  // Expose relative loads before discovering checks and strips.
  bool Modified = !ClDisable && lowerLoadRelativeIntrinsics(M);
  if (Modified) {
    AM.invalidate(M, PreservedAnalyses::none());
  }

  PointerTagSanitizer PTSan(M);
  PTSan.setRuntimeFlags();
  if(ClDisable) {
    INFO(llvm::errs() << "Disabled. Bailing\n");
    return PreservedAnalyses::all();
  }

  Modified |= PTSan.sanitizeModule();

  auto shouldStopAfter = [](StringRef Phase) {
    return ClStopAfter == Phase;
  };

  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  PTSanPreparationPass Preparation;
  PTSanDiscoveryPass Discovery;
  PTSanOptimizationPass Optimization;
  PTSanMetadataLoweringPass MetadataLowering;
  PTSanLateOptimizationPass LateOptimization;
  PTSanLoweringPass Lowering;

  // Prepare raw signal entries after global instrumentation and before the
  // function pipeline, which skips the generated uninstrumented wrappers.
  Modified |= Preparation.run(M);

  for (Function &F : M) {
    if (F.empty()) {
      continue;
    }
    if (F.hasFnAttribute(Attribute::DisableSanitizerInstrumentation)) {
      continue;
    }
    if (PTSan.isInstr(&F)) {
      continue;
    }

    DBG(errs() << "\n\n-----------------------------------------\n");
    INFO(errs() << "Running PTSan phases on " << F.getName() << "\n");
    DBG(errs() << "-----------------------------------------\n");

    TargetLibraryInfo &TLI = FAM.getResult<TargetLibraryAnalysis>(F);
    {
      PTSanAnalysisState PTA(F, PTSan, TLI);
      Modified |= Preparation.run(PTA);
      if (shouldStopAfter("prepare")) {
        continue;
      }

      Modified |= Discovery.run(PTA);
      if (shouldStopAfter("discovery")) {
        continue;
      }
    }

    // Discovery rewrites pointer sources through SourceAnchors. Rebuild the
    // function analyses so ScalarEvolution cannot later rematerialize an
    // expression rooted at the pre-anchor source.
    PTSanAnalysisState PTA(F, PTSan, TLI);
    Modified |= Optimization.run(PTA);
    if (shouldStopAfter("optimization")) {
      continue;
    }

    Modified |= MetadataLowering.run(PTA);
    if (shouldStopAfter("metadata-lowering")) {
      continue;
    }

    Modified |= LateOptimization.run(PTA);
    if (shouldStopAfter("late-optimization")) {
      continue;
    }

    // Generated musl initializer replay precedes TLS. Its memory accesses still
    // get normal checks, but the libc nesting counter does not exist yet.
    if (ClValidate && !F.hasFnAttribute("ptsan-pre-tls")) {
      PTSanIRB IRB(PTSan, &*F.getEntryBlock().getFirstInsertionPt());
      IRB.CreateCall(PTSan.ValidateFunctionStart);
    }
    Modified |= Lowering.run(PTA, FAM);
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
    runOptimizations(M, FS);
    INFO(errs() << "Re-ran optimizations after ptsan\n");
  }

  // Large initializer replays make SelectionDAG store merging prohibitively
  // expensive. Keep instrumentation and IR cleanup, but skip backend
  // optimization of this one-time startup function.
  if (Function *Init =
          M.getFunction("__ptsan_init_global_initializer_pointers")) {
    Init->addFnAttr(Attribute::NoInline);
    Init->addFnAttr(Attribute::OptimizeNone);
  }

  PreservedAnalyses PA = PreservedAnalyses::none();
  // GlobalsAA is considered stateless and does not get invalidated unless
  // explicitly invalidated; PreservedAnalyses::none() is not enough. Sanitizers
  // make changes that require GlobalsAA to be invalidated.
  PA.abandon<GlobalsAA>();
  return PA;
}
