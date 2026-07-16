//===-- PTSanUtils.h - Shared PTSan utilities -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#pragma once

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"
#include <optional>

namespace llvm::ptsan {

#define die() \
  assert(false); \
  exit(EXIT_FAILURE);

class AccessRange;

enum class CmpResult { LTE, GTE, EQ, Unknown };
CmpResult swapOpsCmpResult(CmpResult C);
bool materializeConstantUsersOfGlobal(Function &F, GlobalValue *G);

enum class Polarity {
  Tagged,
  Stripped,
};

StringRef getPTSanPolarityName(Polarity P);
void setPTSanPolarityMetadata(Value *V, Polarity P,
                              StringRef Prefix = "ptsan.");
std::optional<Polarity>
getPTSanPolarityMetadata(Value *V, StringRef Prefix = "ptsan.");
void clearPTSanPolarityMetadata(Function &F, StringRef Prefix = "ptsan.");

// Returns the pointer operand use for operations whose result can be modeled as
// a rewriteable pointer dataflow boundary. Use this in transformations that may
// rewrite the operand use itself.
Use *getPTSanRewriteablePointerUse(Instruction &I);

// Returns the pointer value that V aliases/derives from for provenance queries.
// This includes cases such as returned-argument calls where the call result has
// the same underlying object, but the call operand use must not be rewritten.
Value *getPTSanPointerPeel(Value *V);

typedef std::shared_ptr<AccessRange> AccessRangePtr;

struct SerializeBoundsInfo {
  LLVMContext &Ctx;

  SerializeBoundsInfo(LLVMContext &Ctx): Ctx(Ctx) {}
  SerializeBoundsInfo(const Loop *L): Ctx(L->getHeader()->getContext()) {}

  MDNode *serializeSCEV(const SCEV *S);
  MDNode *serializeBaseRangeEntry(Value *Base, SmallVector<AccessRangePtr> &Ranges);
  MDNode *serializeSolvedRanges(bool Complete,
    DenseMap<Value *, SmallVector<AccessRangePtr>> &SolvedRanges);
  Metadata *serializeValue(Value *V);
  Metadata *serializeType(Type *Ty);
  void addLoopTag(Loop *L, MDNode *Tag);
  MDNode *getPTSanBounds(const MDNode *LoopID);
  MDNode *getOrCreateValueID(Value *V);
};

struct PTSanUtils;

struct DeserializeBoundsInfo {
  ScalarEvolution &SE;
  LoopInfo &LI;
  DominatorTree &DT;
  Instruction *IP; // Will insert before-or-at
  bool CanSynthesize;
  DenseMap<const MDNode *, Value *> IDToValue;
  DenseSet<const MDNode *> AmbiguousValueIDs;

  DeserializeBoundsInfo(ScalarEvolution &SE, LoopInfo &LI, DominatorTree &DT,
                        Instruction *IP, bool CanSynthesize = true):
    SE(SE), LI(LI), DT(DT), IP(IP), CanSynthesize(CanSynthesize) {
    for (BasicBlock &BB : *IP->getFunction()) {
      for (Instruction &I : BB) {
        MDNode *ID = I.getMetadata("ptsan.value.id");
        if (ID) {
          auto [It, Inserted] = IDToValue.try_emplace(ID, &I);
          if (!Inserted && It->second != &I) {
            AmbiguousValueIDs.insert(ID);
          }
        }
      }
    }
  }

  std::pair<bool, DenseMap<Value *, SmallVector<AccessRangePtr>>>
  deserializeSolvedRanges(MDNode *Root);

  std::optional<std::pair<Value *, SmallVector<AccessRangePtr>>>
  deserializeBaseRangeEntry(MDNode *Entry);

  Value *deserializeValue(Metadata *M);
  Type *deserializeType(Metadata* M);

  const SCEV *deserializeSCEV(Metadata *M);

  bool isValidToInsert(Value *V);
  void rememberDeserializedValue(MDNode *Node, Value *V);
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
  bool depsDominateLoc(const SCEV *S, Instruction *Loc);
  void indexInstructionSCEVs();
  Instruction *findLocalSCEVRepresentative(const SCEV *S, Instruction *Loc);
  const SCEV * solveAccessExtent(Loop *L, const SCEV *S, bool FindMin);
  std::shared_ptr<AccessRange> getLoopAccessExtent (Value *V, Value *TypeSize, Loop *L);
  SmallPtrSet<const BasicBlock *, 16> getEscapeBlocks(Loop *L);
  bool isValidCheckBlock(BasicBlock *BB, Instruction *Sink);
  bool hoistTargetDominates(HoistTarget Target, Instruction *Pos);
  HoistTarget getFirstCommonInsertionPoint(std::initializer_list<Value*> Points);
  HoistTarget getFirstIPAfter(Value *V);
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

static bool isPtrOrVecOfPtrType(Type *T) {
  if (T->isPointerTy()) {
    return true;
  }
  if (auto *VT = dyn_cast<VectorType>(T)) {
    return isPtrOrVecOfPtrType(VT->getElementType());
  }
  return false;
}


struct PtrOp {
  Use *U;
  CHK_TY CheckTy;
  Value *Size;
  PtrOp() = delete;

  void validate() {
    Value *V = U->get();

    if (isa<PtrToIntInst>(V)) {
      return;
    }
    if (isPtrOrVecOfPtrType(V->getType())) {
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

  PtrOp(Instruction *I, int Idx, CHK_TY CheckTy, Value *Size)
      : U(&I->getOperandUse(Idx)), CheckTy(CheckTy), Size(Size) {
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
  virtual MDNode *toMD(LLVMContext &Ctx) const = 0;
  static AccessRangePtr fromMD(DeserializeBoundsInfo &DBI , MDNode *MD);

  bool isSafeToExpandAt(SCEVExpander &Expander, Instruction *Pos) const;

  SmallPtrSet<Instruction *, 8> getDeps(PTSanUtils *PTU) const;

  virtual std::shared_ptr<AccessRange> clone() const = 0;

protected:
  ScalarEvolution &SE;

  explicit AccessRange(Kind K, ScalarEvolution &SE) : SE(SE), K(K) {}
private:
  Kind K;
};

class SimpleAccessRange : public AccessRange {
private:
  const SCEV *Min;
  const SCEV *Size;
public:
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
  MDNode *toMD(LLVMContext &Ctx) const override;
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

class LoopAccessRange : public AccessRange {
private:
  const SCEV *First;
  const SCEV *Last;
  const SCEV *Delta;
  const SCEV *TypeSize;
  const Loop *L;

public:
  LoopAccessRange(ScalarEvolution &SE, const SCEV *First, const SCEV *Last, const SCEV *Delta, const SCEV *TypeSize, const Loop *L):
    AccessRange(K_LoopRange, SE), First(First), Last(Last), Delta(Delta), TypeSize(TypeSize), L(L) {
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
  MDNode *toMD(LLVMContext &Ctx) const override;
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
