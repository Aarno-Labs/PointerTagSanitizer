//===-- PTSanInternal.h - Private PTSan implementation state ----*- C++ -*-===//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/Loads.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/ReplaceConstant.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
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

#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>
#include <stdlib.h>

using namespace llvm;
using namespace llvm::PatternMatch;
using std::nullopt;
using std::optional;
using NW = llvm::SCEV::NoWrapFlags;

#define DEBUG_TYPE "ptsan-dbg"
#define INFO_TYPE "ptsan-info"
#define DBG(X) DEBUG_WITH_TYPE(DEBUG_TYPE, X)
#define INFO(X) DEBUG_WITH_TYPE(INFO_TYPE, X)

namespace llvm::ptsan {

extern cl::opt<bool> ClDisable;
extern cl::opt<bool> ClOptimizeAfterward;
extern cl::opt<bool> ClNoBoundsLoadHoist;
extern cl::opt<bool> ClNoLoopCheckHoist;
extern cl::opt<bool> ClNoCheckMerge;
extern cl::opt<bool> ClNoFilterRemovableChecks;
extern cl::opt<bool> ClNoStackIDFastPath;
extern cl::opt<bool> ClNoChecks;
extern cl::opt<bool> ClNoIDs;
extern cl::opt<bool> ClNoStrip;
extern cl::opt<bool> ClLAM;
extern cl::opt<bool> ClInstrumentGlobals;
extern cl::opt<bool> ClValidate;
extern cl::opt<bool> ClValidateArith;
extern cl::opt<bool> ClPrintStats;
extern cl::opt<bool> ClClearIdsOnArith;
extern cl::opt<bool> ClClearIdsOnCmp;
extern cl::opt<bool> ClQuietErrors;
extern cl::opt<bool> ClStrictMode;
extern cl::opt<bool> ClNoPTSanOpts;
extern cl::opt<bool> ClNoStripIDHoist;
extern cl::opt<bool> ClUseLocalStripIDHoist;
extern cl::opt<bool> ClUseReturnedMallocSize;
extern cl::opt<bool> ClAbortOnError;
extern cl::opt<bool> ClPTSanPreanalyzeLoops;
extern cl::opt<bool> ClStackTemporalSafety;
extern cl::opt<unsigned> ClStackIDsPerBucket;
extern cl::opt<unsigned> ClStackIDThreadBuckets;
extern cl::opt<bool> ClAddOverflowChecks;
extern cl::opt<int> ClPatchAllWith;
extern cl::opt<bool> ClCreateIRDbg;
extern cl::opt<std::string> ClStopAfter;

bool isLoopCheckHoistDisabled();
bool isCheckMergeDisabled();
bool isRemovableCheckFilteringDisabled();
bool isStackIDFastPathDisabled();
bool isKnownZeroAccessSize(Value *Size);
Value *findPTSanBasePointer(Value *Start, PTSanUtils &PTU);
bool isUntaggedConstantPtr(Value *V);
optional<uint64_t> getStaticGlobalSize(const GlobalVariable *GV,
                                       const DataLayout &DL);
bool isStdLibraryGlobal(const GlobalVariable &GV);

class PointerTagSanitizer;

// XXX: figure out a way to not-copy-paste this into compiler-rt
enum FREE_TY : uint32_t {
  STACK,
  MALLOC,
  MMAP,
};

struct RangeCheckInfo {
  HoistTarget IP;
  CHK_TY Type;
  uint32_t CheckID;
  Value *PtrID;
  Value *OutOfRange;
  Value *Min;
  Value *Size;
};

struct StaticObjectBounds {
  Value *Base;
  const SCEVConstant *Size;
};

class PTSanAnalysisState;
class PTSanDiscoverer;
class PTSanOptimizer;
class PTSanAnchorOptimizer;
class PTSanMetadataLowerer;
class PTSanStripIDHoister;
class PTSanMinCutStripIDHoister;
class PTSanMinCutGraphBuilder;
class PTSanMinCutRewriter;
class PTSanLateOptimizer;
class PTSanLowerer;
struct PTSanIRB;
class PTSanPreparationPass;
class PTSanDiscoveryPass;
class PTSanOptimizationPass;
class PTSanMetadataLoweringPass;
class PTSanLateOptimizationPass;
class PTSanLoweringPass;
class PointerTagSanitizer;
class Anchor;
class SourceAnchor;
class AccessAnchor;
class LoopPreanalysisAnchor;
class CheckAnchor;
class GetIDAnchor;
class BoundAnchor;
class LoadBoundAnchor;
class StaticBoundAnchor;
class StripAnchor;
class TaggedAnchor;
class TagAnchor;
class ValidateArithAnchor;
class PositionedAnchor;
class FunctionExitAnchor;
class LibcCallAnchor;
class SpecialStripAnchor;

class PointerTagSanitizer {
private:
  friend class ::llvm::PointerTagSanitizerPass;
  friend class PTSanAnalysisState;
  friend class Anchor;
  friend class SourceAnchor;
  friend class AccessAnchor;
  friend class LoopPreanalysisAnchor;
  friend class CheckAnchor;
  friend class GetIDAnchor;
  friend class BoundAnchor;
  friend class LoadBoundAnchor;
  friend class StaticBoundAnchor;
  friend class StripAnchor;
  friend class TaggedAnchor;
  friend class TagAnchor;
  friend class ValidateArithAnchor;
  friend class PositionedAnchor;
  friend class FunctionExitAnchor;
  friend class LibcCallAnchor;
  friend class SpecialStripAnchor;
  friend class PTSanDiscoverer;
  friend class PTSanOptimizer;
  friend class PTSanAnchorOptimizer;
  friend class PTSanMetadataLowerer;
  friend class PTSanStripIDHoister;
  friend class PTSanMinCutStripIDHoister;
  friend class PTSanMinCutGraphBuilder;
  friend class PTSanMinCutRewriter;
  friend class PTSanLateOptimizer;
  friend class PTSanLowerer;
  friend class PTSanPreparationPass;
  friend class PTSanDiscoveryPass;
  friend class PTSanOptimizationPass;
  friend class PTSanMetadataLoweringPass;
  friend class PTSanLateOptimizationPass;
  friend class PTSanLoweringPass;
  friend class SCEVStripIDs;
  friend struct PTSanIRB;
  Module &M;
  LLVMContext &Ctx;
  const DataLayout &DL;
  DIBuilder DIB;
  DICompileUnit *CU;

  // Real -> Proxy
  DenseMap<GlobalVariable *, GlobalVariable *> GlobalVariableProxies;
  DenseMap<GlobalAlias *, GlobalAlias *>GlobalAliasProxies;
  // Proxy -> Real
  DenseMap<GlobalValue *, GlobalVariable *> GlobalProxyTargets;

  // Types
  IntegerType *U32Ty;
  IntegerType *U64Ty;
  IntegerType *U16Ty;
  IntegerType *U8Ty;
  IntegerType *UptrTy;
  IntegerType *BoolTy;
  StructType *IDNodeStructTy;
  StructType *BoundsStructTy;
  StructType *NewRangeStructTy;
  StructType *TwoSpansTy;
  Value *Nullptr;
  PointerType *PtrTy;

  ConstantInt *AddrMask;
  ConstantInt *IDMask;
  ConstantInt *DefaultIDNo;

  // Instrumentation-inserted values
  SmallPtrSet<Value *, 16> InstrVals;

  SmallPtrSet<Value *, 8> AnchorCallees;

  // Globals that we actually get/use/reference
  FunctionCallee GetStackIDs1;
  FunctionCallee GetStackIDs2;
  FunctionCallee GetStackIDs3;
  FunctionCallee GetStackIDs4;
  FunctionCallee GetStackIDsN;

  FunctionCallee FreeStackIDs1;
  FunctionCallee FreeStackIDs2;
  FunctionCallee FreeStackIDs3;
  FunctionCallee FreeStackIDs4;
  FunctionCallee FreeStackIDsN;

  FunctionCallee LogAllocaInfo;
  FunctionCallee LogCheckCalled;
  FunctionCallee GetNextGlobalID;
  FunctionCallee ReportGenericError;
  FunctionCallee EnableLibcInstr;
  FunctionCallee ValidateFunctionStart;
  FunctionCallee ValidateArithmetic;
  FunctionCallee MakecontextStripBefore;
  FunctionCallee MakecontextStripAfter;
  FunctionCallee PTSanSource;
  FunctionCallee PTSanAccess;
  FunctionCallee PTSanLoopPreanalysis;
  FunctionCallee PTSanGetID;
  FunctionCallee PTSanLoadBase;
  FunctionCallee PTSanLoadSpan;
  FunctionCallee PTSanStaticBase;
  FunctionCallee PTSanStaticSpan;
  FunctionCallee PTSanStrip;
  FunctionCallee PTSanStripI64;
  FunctionCallee PTSanTagged;
  FunctionCallee PTSanTag;
  FunctionCallee PTSanFunctionExit;
  FunctionCallee PTSanValidateArith;
  FunctionCallee PTSanLibcCall;
  FunctionCallee PTSanSpecialStrip;
  GlobalValue *PTSanUnmaterializedRange;
  GlobalValue *BoundsArray;
  GlobalValue *TypesArray;
  GlobalValue *OverflowIDBoundsPtr;
  GlobalValue *NextStackIDPtr;
  GlobalValue *PendingStackIdsPtr = nullptr;
  GlobalValue *StackIDLimitPtr;
  GlobalValue *PTSanInLibc;

  void initializeModule();
  void markAsInstr(Value *V);
  void unmarkAsInstr(Value *V);
  bool isInstr(Value *V);
  bool isAnchor(Value *V) const;
  template <typename AnchorT>
  SmallVector<AnchorT, 16> collectAnchors(Function &F);
  // Find the following program instruction, skipping adjacent PTSan anchors.
  Instruction *getNextRealInstruction(Anchor Anchor) const;
  // Find the first valid insertion point after any leading PTSan anchors.
  Instruction *getFirstRealInsertionPt(BasicBlock &BB) const;
  void verifyNoAnchors() const;
  void setRuntimeFlags();

public:
  template <typename AnchorT>
    optional<AnchorT> getAsAnchor(Value *V);
  PointerTagSanitizer(Module &M);
  bool sanitizeModule();
  void finalize() { DIB.finalize(); }
};

// IRBuilder which marks all created instructions as instrumentation
struct PTSanIRB
    : IRBuilder<InstSimplifyFolder, IRBuilderCallbackInserter> {
  explicit PTSanIRB(PointerTagSanitizer &DS, BasicBlock *BB):
    PTSanIRB(DS, BB->getContext()) {
      this->SetInsertPoint(BB);
    }
  explicit PTSanIRB(PointerTagSanitizer &DS, Instruction *I)
      : PTSanIRB(DS, I->getContext()) {
    this->SetInsertPoint(I);
  }
  explicit PTSanIRB(PointerTagSanitizer &DS, LLVMContext &Ctx)
      : IRBuilder(Ctx, DS.DL,
                  IRBuilderCallbackInserter(
                      [&DS](Instruction *I) {
                        DS.markAsInstr(I);
                       })) {}
};

} // namespace llvm::ptsan
