//===-- PTSanUtils.h - Shared PTSan utilities -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include <optional>

namespace llvm::ptsan {

// Parse the ptsan.loop.ids node for an instruction into a typed list
inline SmallVector<uint64_t, 2> getPTSanLoopIDs(const Instruction &I) {
  SmallVector<uint64_t, 2> IDs;
  if (MDNode *Node = I.getMetadata(LLVMContext::MD_ptsan_loop_ids)) {
    for (const MDOperand &Op : Node->operands()) {
      IDs.push_back(mdconst::extract<ConstantInt>(Op)->getZExtValue());
    }
  }
  return IDs;
}

inline void addPTSanLoopID(Instruction &I, uint64_t ID) {
  SmallVector<Metadata *, 4> IDs;
  if (MDNode *Node = I.getMetadata(LLVMContext::MD_ptsan_loop_ids)) {
    IDs.append(Node->op_begin(), Node->op_end());
  }
  IDs.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt64Ty(I.getContext()), ID)));
  I.setMetadata(LLVMContext::MD_ptsan_loop_ids,
                MDNode::get(I.getContext(), IDs));
}

#define die() \
  assert(false); \
  exit(EXIT_FAILURE);

class AccessRange;
class SimpleAccessRange;

enum class CmpResult { LTE, GTE, EQ, Unknown };
CmpResult swapOpsCmpResult(CmpResult C);
bool materializeConstantUsersOfGlobal(Function &F, GlobalValue *G);
GlobalVariable *getPTSanDataGlobal(Value *V);
bool addNoBuiltinForPointerFns(Module &M);
bool lowerLoadRelativeIntrinsics(Module &M);

enum class Polarity {
  Tagged,
  Stripped,
};

void setPTSanPolarityMetadata(Instruction &I, Polarity P);

class PointerTagSanitizer;

// Required value polarity, or nullopt when MinCut may choose either.
// Operand polarity constraints are checked separately.
std::optional<Polarity> getRequiredPolarity(PointerTagSanitizer &DS, Value *V);

class PTSanPolarityReader {
  PointerTagSanitizer &DS;

public:
  PTSanPolarityReader(PointerTagSanitizer &DS, Function &F);

  Polarity getPolarity(Value *V);
};

// Returns the pointer operand use for operations whose result can be modeled as
// a rewriteable pointer dataflow boundary. Use this in transformations that may
// rewrite the operand use itself.
Use *getPTSanRewriteablePointerUse(Instruction &I);

// Returns the pointer value that V aliases/derives from for provenance queries.
// This includes cases such as returned-argument calls where the call result has
// the same underlying object, but the call operand use must not be rewritten.
Value *getPTSanPointerPeel(Value *V);

// Return whether a shuffle's mask reads from its first and second operands.
// Only operands that contribute lanes are pointer-dataflow boundaries: an
// unused operand must not affect the min-cut or polarity validation. This is
// especially common for splats, whose second operand is usually poison.
std::pair<bool, bool>
getPTSanUsedShuffleOperands(const ShuffleVectorInst &Shuffle);

typedef std::shared_ptr<AccessRange> AccessRangePtr;

struct SerializeBoundsInfo {
  LLVMContext &Ctx;
  DenseMap<Value *, unsigned> CaptureIndices;
  SmallVector<Value *, 8> Captures;

  SerializeBoundsInfo(LLVMContext &Ctx): Ctx(Ctx) {}

  MDNode *serializeSCEV(const SCEV *S);
  MDNode *serializeBaseRangeEntry(Value *Base, SmallVector<AccessRangePtr> &Ranges);
  MDNode *serializeSolvedRanges(bool Complete,
    DenseMap<Value *, SmallVector<AccessRangePtr>> &SolvedRanges);
  Metadata *serializeValue(Value *V);
  ArrayRef<Value *> getCaptures() { return Captures; }
};

struct PTSanUtils;

struct DeserializeBoundsInfo {
  ScalarEvolution &SE;
  SmallVector<Value *, 8> Captures;

  DeserializeBoundsInfo(ScalarEvolution &SE, ArrayRef<Value *> Captures)
      : SE(SE), Captures(Captures) {}

  std::pair<bool, DenseMap<Value *, SmallVector<AccessRangePtr>>>
  deserializeSolvedRanges(MDNode *Root);

  std::pair<Value *, SmallVector<AccessRangePtr>>
  deserializeBaseRangeEntry(MDNode *Entry);

  Value *deserializeValue(Metadata *M);
  const SCEV *deserializeSCEV(Metadata *M);
};


enum InsertSide {
  BEFORE,
  AFTER
};


struct HoistTarget {
  Instruction *Instr;
  InsertSide Side;

  Instruction *getIP() const {
    assert(!(Side == AFTER && Instr->isTerminator()));
    return Side == BEFORE ? Instr : Instr->getNextNode();
  }
};

struct PTSanUtils {
  Function &F;
  LLVMContext &Ctx;
  const DataLayout &DL;
  DominatorTree &DT;
  PostDominatorTree &PDT;
  LoopInfo &LI;
  ScalarEvolution &SE;

  PTSanUtils(Function &F, LLVMContext &Ctx, DominatorTree &DT, PostDominatorTree &PDT, LoopInfo &LI, ScalarEvolution &SE):
    F(F), Ctx(Ctx), DL(F.getDataLayout()), DT(DT), PDT(PDT), LI(LI), SE(SE) {
    indexInstructionSCEVs();
  }

  ValueMap<Value *, SmallVector<Value *>> KnownBases;
  DenseMap<std::pair<const SCEV *, const SCEV *>, CmpResult> ScevCmpCache;
  DenseMap<const SCEV *, SmallPtrSet<Instruction*, 8>> ScevDepsCache;
  DenseMap<const SCEV *, SmallVector<Instruction *, 2>> SCEVToInstrs;

  SmallVector<Value *> findBasePointers(Value *Start);
  Value *findBasePointer(Value *Start);
  CmpResult compareSCEVs(const SCEV *A, const SCEV *B);
  SmallPtrSet<Instruction *, 8> getSCEVInstrs(const SCEV *Expr);
  bool depsDominateLoc(const SmallPtrSetImpl<Instruction *> &Deps,
                       Instruction *Loc);
  bool depsDominateLoc(const SCEV *S, Instruction *Loc);
  void indexInstructionSCEVs();
  Instruction *findLocalSCEVRepresentative(const SCEV *S, Instruction *Loc);
  const SCEV *getAddRecFreeRepresentation(const SCEV *S, Instruction *Loc);
  const SCEV * solveAccessExtent(Loop *L, const SCEV *S, bool FindMin);
  // Return the ordinary scalar range for an unmasked access, or the smallest
  // contiguous scalar range covering every active lane of a fully constant
  // fixed-vector mask. An empty mask has a zero-sized envelope; unknown masks
  // have no scalar envelope.
  std::shared_ptr<SimpleAccessRange>
  getScalarEnvelope(Value *Ptr, Value *ElementSize, Value *Mask,
                    Instruction *Access);
  std::shared_ptr<AccessRange>
  getLoopAccessExtent(const SimpleAccessRange &Range, Loop *L);
  SmallPtrSet<const BasicBlock *, 16> getEscapeBlocks(Loop *L);
  bool isValidCheckBlock(BasicBlock *BB, Instruction *Sink);
  bool hoistTargetDominates(HoistTarget Target, Instruction *Pos);
  HoistTarget
  getFirstCommonInsertionPoint(std::initializer_list<HoistTarget> Targets);
  HoistTarget getFirstIPAfter(Value *V);
  // Return V unchanged when it already has Ty; otherwise splat scalar V to
  // match vector Ty.
  Value *splatToMatchType(Value *V, Type *Ty);
  bool isKnownSpatiallyValid(Use *U);
  bool isUnfreeableMemory(Value *V);
};

// Keep this in sync with ptsan_stats.h
#define PTSAN_CHK_TY_LIST(X) \
  X(NO_CHECK,         0)     \
  X(POINT,            1)     \
  X(RANGE,            2)     \
  X(LOOP,             3)     \
  X(PREANALYZED_LOOP, 4)     \
  X(COMPOUND,         5)     \
  X(EXOTIC,           6)     \
  X(END_TYPE,         7)

enum CHK_TY {
#define X(name, value) name = value,
PTSAN_CHK_TY_LIST(X)
#undef X
};

#define X(name, value)                         \
  case name:                                   \
    return #name;

static inline const char *ChkTyToString(CHK_TY Ty) {
  switch (Ty) {
    PTSAN_CHK_TY_LIST(X)
  }
}

#undef X

struct PtrOp {
  Use *U;
  CHK_TY CheckTy;
  Value *Size;
  Value *Mask = nullptr;
  PtrOp() = delete;

  void validate() {
    Value *V = U->get();

    if (isa<PtrToIntInst>(V)) {
      return;
    }
    if (V->getType()->isPtrOrPtrVectorTy()) {
      return;
    }

    errs() << "Invalid PtrOp: " << *V << ": " << *V->getType()  << "\n";
    die();
  }

  PtrOp(Instruction *I, int Idx, CHK_TY CheckTy, uint64_t Sz)
      : U(&I->getOperandUse(Idx)), CheckTy(CheckTy) {
    Type *U64Ty = IntegerType::get(I->getContext(), 64);
    Size = ConstantInt::get(U64Ty, Sz);
    validate();
  }

  PtrOp(Instruction *I, int Idx, CHK_TY CheckTy, TypeSize TS)
      : U(&I->getOperandUse(Idx)), CheckTy(CheckTy) {
    Type *U64Ty = IntegerType::get(I->getContext(), 64);
    Size = ConstantInt::get(U64Ty, TS.getFixedValue());
    validate();
  }

  PtrOp(Instruction *I, int Idx, CHK_TY CheckTy, Value *Size,
        Value *Mask = nullptr)
      : U(&I->getOperandUse(Idx)), CheckTy(CheckTy), Size(Size), Mask(Mask) {
    if (this->Size == nullptr && CheckTy != NO_CHECK && CheckTy != PREANALYZED_LOOP) {
      errs() << "Null size. This will be an underestimate\n";
      Type *U64Ty = IntegerType::get(I->getContext(), 64);
      this->Size = ConstantInt::get(U64Ty, 1);
    }
    validate();
  }

  PtrOp(Instruction *I, int Idx): PtrOp(I, Idx, NO_CHECK, nullptr) {
    validate();
  }
};


struct FindMemAccesses: public InstVisitor<FindMemAccesses> {
  const DataLayout &DL;

  SmallVectorImpl<PtrOp> &PtrOps;

  FindMemAccesses(const DataLayout &DL, SmallVectorImpl<PtrOp> &PtrOps) : DL(DL), PtrOps(PtrOps) {}

  void visitIntrinsicInst(IntrinsicInst &I);
  void visitStoreInst(StoreInst &I);
  void visitLoadInst(LoadInst &I);
  void visitAtomicRMWInst(AtomicRMWInst &I);
  void visitAtomicCmpXchg(AtomicCmpXchgInst &I);
};

enum RangeCmpResult {
  RCR_UNKNOWN,
  RCR_SUBSET,
  RCR_SUPERSET,
  RCR_EQUAL,
  // These can include partial overlaps
  RCR_LTE,
  RCR_GTE
};

class AccessRange {
public:
  enum Kind {
    K_Base,         // rarely used directly
    K_SimpleRange,
    K_LoopRange,
    K_VectorRange,
  };

  Kind getKind() const { return K; }

  static bool classof(const AccessRange *AR) { return true; }

  virtual ~AccessRange();
  virtual RangeCmpResult cmp(PTSanUtils *PTU, const AccessRange *Other) const = 0;
  // Return true if merging changed anything
  virtual bool mergeIn(PTSanUtils *PTU, const AccessRange *Other) = 0;
  virtual bool isSizeZero() const = 0;
  virtual std::pair<Value *, Value *> materializeMinSize(
    PTSanUtils *PTU, SCEVExpander &Expander, Instruction *IP) const = 0;
  virtual void dump(int spaces) const = 0;
  virtual SmallVector<const SCEV *, 4> getAllSCEVs() const = 0;
  virtual MDNode *toMD(SerializeBoundsInfo &SBI) const = 0;
  static AccessRangePtr fromMD(DeserializeBoundsInfo &DBI , MDNode *MD);

  bool isSafeToExpandAt(SCEVExpander &Expander, Instruction *Pos) const;

  virtual SmallPtrSet<Instruction *, 8> getDeps(PTSanUtils *PTU) const;

  virtual std::shared_ptr<AccessRange> clone() const = 0;

protected:
  ScalarEvolution &SE;

  explicit AccessRange(Kind K, ScalarEvolution &SE) : SE(SE), K(K) {}
private:
  Kind K;
};

class SimpleAccessRange : public AccessRange {
public:
  const SCEV *Min;
  const SCEV *Size;

  SimpleAccessRange(ScalarEvolution&SE, const SCEV *Min, const SCEV *Size):
    AccessRange(K_SimpleRange, SE), Min(Min), Size(Size) {
      if (SE.isKnownNegative(Size)) {
        errs() << "Size should not be negative! " << *Size << "\n";
        die();
      }
    }

  RangeCmpResult cmp(PTSanUtils *PTU, const AccessRange *Other) const override;
  bool mergeIn(PTSanUtils *PTU, const AccessRange *Other) override;
  bool isSizeZero() const override;
  std::pair<Value *, Value *> materializeMinSize(
    PTSanUtils *PTU, SCEVExpander &Expander, Instruction *IP) const override;
  void dump(int spaces) const override;
  MDNode *toMD(SerializeBoundsInfo &SBI) const override;
  std::shared_ptr<AccessRange> clone() const override;

  SmallVector<const SCEV *, 4> getAllSCEVs() const override {
    SmallVector<const SCEV *, 4> V;
    V.push_back(Min);
    V.push_back(Size);
    return V;
  }

  static bool classof(const AccessRange *AR) {
    return AR->getKind() == K_SimpleRange;
  }
};

// A masked vector memory access whose active lanes all have the same size.
// Each lane covers LaneSize bytes. For an explicit pointer vector, lane I
// starts at Pointer[I] + Offset and Stride is unused. For a contiguous access,
// lane I starts at Pointer + Offset + I * Stride.
//
// Source accesses initially have equal LaneSize and Stride. Keeping them
// separate lets merging widen LaneSize without changing the original lane
// addresses.
class VectorAccessRange : public AccessRange {
private:
  Value *Pointer;
  int64_t Offset;
  Value *Mask;
  int64_t LaneSize;
  int64_t Stride;

  bool hasExplicitLanePointers() const {
    return Pointer->getType()->isVectorTy();
  }

public:
  VectorAccessRange(ScalarEvolution &SE, Value *Pointer, int64_t Offset,
                    Value *Mask, int64_t LaneSize, int64_t Stride)
      : AccessRange(K_VectorRange, SE), Pointer(Pointer), Offset(Offset),
        Mask(Mask), LaneSize(LaneSize), Stride(Stride) {
    assert(LaneSize >= 0);
  }

  RangeCmpResult cmp(PTSanUtils *PTU, const AccessRange *Other) const override;
  bool mergeIn(PTSanUtils *PTU, const AccessRange *Other) override;
  bool isSizeZero() const override;
  std::pair<Value *, Value *> materializeMinSize(
    PTSanUtils *PTU, SCEVExpander &Expander, Instruction *IP) const override;
  void dump(int spaces) const override;
  MDNode *toMD(SerializeBoundsInfo &SBI) const override;
  std::shared_ptr<AccessRange> clone() const override;
  SmallPtrSet<Instruction *, 8> getDeps(PTSanUtils *PTU) const override;

  Value *getMask() const { return Mask; }

  SmallVector<const SCEV *, 4> getAllSCEVs() const override {
    return {};
  }

  static bool classof(const AccessRange *AR) {
    return AR->getKind() == K_VectorRange;
  }
};

class LoopAccessRange : public AccessRange {
private:
  const SCEV *First;
  const SCEV *Last;
  const SCEV *Delta;
  const SCEV *TypeSize;

public:
  LoopAccessRange(ScalarEvolution &SE, const SCEV *First, const SCEV *Last, const SCEV *Delta, const SCEV *TypeSize):
    AccessRange(K_LoopRange, SE), First(First), Last(Last), Delta(Delta), TypeSize(TypeSize) {
      if (SE.isKnownNegative(Delta) || SE.isKnownPositive(Delta)) {
        errs() << "Delta should not be known! This should be a SimpleRange " << *Delta << "\n";
        //die();
      }
    }

  RangeCmpResult cmp(PTSanUtils *PTU, const AccessRange *Other) const override;
  bool mergeIn(PTSanUtils *PTU, const AccessRange *Other) override;
  bool isSizeZero() const override;
  std::pair<Value *, Value *> materializeMinSize(
    PTSanUtils *PTU, SCEVExpander &Expander, Instruction *IP) const override;
  void dump(int spaces) const override;
  MDNode *toMD(SerializeBoundsInfo &SBI) const override;
  std::shared_ptr<AccessRange> clone() const override;

  SmallVector<const SCEV *, 4> getAllSCEVs() const override {
    SmallVector<const SCEV *, 4> V;
    V.push_back(First);
    V.push_back(Last);
    V.push_back(Delta);
    V.push_back(TypeSize);
    return V;
  }

  static bool classof(const AccessRange *AR) {
    return AR->getKind() == K_LoopRange;
  }
};


} // namespace llvm::ptsan
