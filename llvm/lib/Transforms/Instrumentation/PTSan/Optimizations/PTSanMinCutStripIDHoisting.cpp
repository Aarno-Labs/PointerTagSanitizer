//===-- PTSanMinCutStripIDHoisting.cpp - Global strip-ID hoisting ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PTSan pointer values can be either tagged or stripped. Some operations
// require one polarity, transparent pointer operations propagate their input
// polarity, and changing polarity costs a strip or tag operation. This file
// chooses all value polarities and conversion points together by reducing that
// problem to an s-t min-cut:
//
//   source side = stripped
//   sink side   = tagged
//
// A directed edge contributes to the cut only when its tail is on the source
// side and its head is on the sink side.
//
// Infinite-capacity edges encode semantic requirements that a legal cut cannot
// violate. Finite edges encode loop-weighted conversion and check costs.
// Max-flow is only the mechanism used to find the minimum cut; the important
// part of this file is how PTSan's IR constraints become this graph.
//
// The graph has three kinds of nodes:
//
//   * Constraint nodes are graph artifacts. Source and sink define the two
//     polarities. Pointer comparisons also use one synthetic node when their
//     operands must match but the comparison has no pointer-valued result.
//     Check-use nodes choose between tagged and stripped check implementations.
//
//   * Value nodes represent the polarity of actual pointer-valued IR Values.
//     Transparent operations, PHIs, and selects reuse their result value node
//     as the required polarity of their pointer operands.
//
//   * Each value can have tag and strip conversion nodes. These represent a
//     shared possible conversion, so ten uses of one stripped value can share
//     one tag and pay its cost once.
//
// A small ID16 loop-hoisting example shows how those nodes fit together:
//
//   entry:
//     br label %loop
//   loop:
//     %q = getelementptr i8, ptr %p, i64 %i
//     %s = call ptr @__ptsan_strip(ptr %q)
//     %v = load i8, ptr %s
//
// %p is an opaque tagged root outside the loop. The strip of loop-local %q
// costs four times as much as a strip of %p. Let TC(V) and SC(V) denote V's tag
// and strip conversion nodes. Omitting only nodes that cannot affect this cut,
// the graph is below. Heavy `=` edges are infinite constraints; light `-` edges
// carry the shown finite costs.
//
//   SRC ===> SC(Q) --4--> Q <==== TC(P)
//                         ||        ^
//                         ||        2
//                         v         |
//                       SC(P) --1-> P ===> SNK
//
// The existing strip requires a stripped representative of Q, encoded by
// SRC ===> SC(Q). The opaque argument P is inherently tagged, encoded by
// P ===> SNK.
//
// SC(Q) is the shared strip conversion for Q. If Q remains tagged, SC(Q) and Q
// land on opposite sides and SC(Q) --4--> Q is cut: the existing strip remains
// and costs 4. If Q itself becomes stripped, both nodes land on the source
// side, the edge is free, and the existing strip is redundant.
//
// Keeping the local strip cuts SC(Q) -> Q for cost 4. That is not the minimum
// cut, however. Instead, the solver finds SC(P) -> P for cost 1. The rewriter
// can then take this assignment (specifically, that SC(P) and P disagree while
// SC(Q) and Q agree), and inserts strip(%p), rewrites the GEP to use it, and
// removes strip(%q).
//
// This example has omitted a few nodes and edges which don't factor into the
// solution for clarity.
//===----------------------------------------------------------------------===//

#include "PTSanStripIDHoisting.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/ConstantRange.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Support/KnownBits.h"
#include <limits>
#include <queue>

namespace llvm::ptsan {
// Small max-flow/min-cut Dinic solver.
class MinCutGraph {
private:
  struct Edge {
    unsigned To;
    unsigned Rev;
    uint64_t Cap;
  };

  // Hold the actual graph adjacencies.
  SmallVector<SmallVector<Edge, 4>, 64> Adj;

  // Holds the DAG-level graph created by BFS.
  SmallVector<int, 64> Level;

  // Per-node cursor for this BFS phase. When we advance NextEdge[N] past an
  // outgoing edge N -> M, that edge is exhausted, off-level, or leads to a dead
  // end for the current level graph.
  SmallVector<unsigned, 64> NextEdge;

  // Build Dinic's level graph from the current residual capacities. If Sink is
  // no longer reachable, there is no remaining augmenting path and max-flow is
  // done.
  bool buildLevels(unsigned Source, unsigned Sink) {
    Level.assign(Adj.size(), -1);
    std::queue<unsigned> Worklist;
    Level[Source] = 0;
    Worklist.push(Source);

    while (!Worklist.empty()) {
      unsigned V = Worklist.front();
      Worklist.pop();

      for (const Edge &E : Adj[V]) {
        if (E.Cap == 0 || Level[E.To] != -1) {
          continue;
        }

        Level[E.To] = Level[V] + 1;
        Worklist.push(E.To);
      }
    }

    return Level[Sink] != -1;
  }

  // Push along the current level graph. The recursion depth is bounded by the
  // number of graph nodes in a single function.
  uint64_t sendFlow(unsigned V, unsigned Sink, uint64_t Flow) {
    if (V == Sink) {
      return Flow;
    }

    while (NextEdge[V] < Adj[V].size()) {
      Edge &E = Adj[V][NextEdge[V]];
      if (E.Cap == 0 || Level[E.To] != Level[V] + 1) {
        ++NextEdge[V];
        continue;
      }

      uint64_t Pushed = sendFlow(E.To, Sink, std::min(Flow, E.Cap));
      if (Pushed == 0) {
        ++NextEdge[V];
        continue;
      }

      E.Cap -= Pushed;
      Adj[E.To][E.Rev].Cap += Pushed;
      return Pushed;
    }

    return 0;
  }

  uint64_t maxFlow(unsigned Source, unsigned Sink) {
    uint64_t Flow = 0;
    while (buildLevels(Source, Sink)) {
      NextEdge.assign(Adj.size(), 0);
      while (uint64_t Pushed =
                 sendFlow(Source, Sink, std::numeric_limits<uint64_t>::max())) {
        Flow += Pushed;
      }
    }
    return Flow;
  }

  // After max-flow, residual reachability from Source is the chosen stripped
  // side of the cut. Everything else is tagged.
  DenseSet<unsigned> getSourceSide(unsigned Source) const {
    DenseSet<unsigned> SourceSide;
    std::queue<unsigned> Worklist;
    SourceSide.insert(Source);
    Worklist.push(Source);

    while (!Worklist.empty()) {
      unsigned V = Worklist.front();
      Worklist.pop();

      for (const Edge &E : Adj[V]) {
        if (E.Cap == 0 || SourceSide.contains(E.To)) {
          continue;
        }

        SourceSide.insert(E.To);
        Worklist.push(E.To);
      }
    }

    return SourceSide;
  }

public:
  unsigned addNode() {
    Adj.emplace_back();
    return Adj.size() - 1;
  }

  void addEdge(unsigned From, unsigned To, uint64_t Cap) {
    Edge Fwd{To, (unsigned)Adj[To].size(), Cap};
    Edge Rev{From, (unsigned)Adj[From].size(), 0};
    Adj[From].push_back(Fwd);
    Adj[To].push_back(Rev);
  }

  // Solve the graph and return the stripped/source side of the minimum cut. A
  // cut at or above InfCost means hard constraints contradicted each other.
  DenseSet<unsigned> getSplit(unsigned Source, unsigned Sink,
                              uint64_t InfCost) {
    uint64_t Flow = maxFlow(Source, Sink);
    assert(Flow < InfCost && "unsatisfiable min-cut graph");
    return getSourceSide(Source);
  }
};

// Finite costs are loop-depth weighted and clamped well below this. Keeping
// "infinite" finite lets the solver use normal integer capacities without
// risking overflow when several hard constraints meet.
static constexpr uint64_t InfCost = UINT64_C(1) << 56;

struct GraphBoundary {
  Use *U;
  unsigned Node;
};

struct SolvedBoundary {
  Use *U;
  Polarity RequiredPolarity;
};

class PTSanMinCutGraphBuilder {
private:
  struct ConversionNodes {
    unsigned Tag;
    unsigned Strip;
  };

  PTSanAnalysisState &PTA;
  PointerTagSanitizer &DS;
  Function &F;
  LoopInfo &LI;

  uint64_t CheckWithTaggedCost;
  uint64_t StripPointerCost;
  uint64_t CheckWithStrippedCost;
  uint64_t TagPointerCost;

  MinCutGraph Graph;
  unsigned SourceNode;
  unsigned SinkNode;
  DenseMap<Value *, unsigned> ValueNodes;
  DenseMap<Value *, ConversionNodes> ConversionNodesForValue;
  SmallVector<GraphBoundary, 32> Boundaries;

  uint64_t getCostMultiplier(Value *V) const;
  bool canKeepID32GEPTagged(GetElementPtrInst &GEP);
  unsigned getValueNode(Value *V);
  ConversionNodes getConversionNodes(Value *V);
  void forceStripped(unsigned Node);
  void forceTagged(unsigned Node);
  void addUseBoundary(Use &U, unsigned Node);
  void buildGraph();

public:
  PTSanMinCutGraphBuilder(PTSanAnalysisState &PTA)
      : PTA(PTA), DS(PTA.DS), F(PTA.F), LI(PTA.LI), SourceNode(Graph.addNode()),
        SinkNode(Graph.addNode()) {
    if (Cl32BitIDs) {
      if (usesConditionalID32Decode()) {
        CheckWithTaggedCost = 6;
        StripPointerCost = 4;
        CheckWithStrippedCost = 2;
        TagPointerCost = 8;
      } else {
        CheckWithTaggedCost = 2;
        StripPointerCost = 2;
        CheckWithStrippedCost = 2;
        TagPointerCost = 6;
      }
    } else {
      CheckWithTaggedCost = 1;
      StripPointerCost = 1;
      CheckWithStrippedCost = 2;
      TagPointerCost = 2;
    }
    assert(DS.collectAnchors<TagAnchor>(F).empty() &&
           "tag anchors must be created by min-cut rewriting");
  }

  SmallVector<SolvedBoundary, 32> solveAndAnnotate();
};

// Hard constraints are represented as one-way infinite cuts. With our labeling,
// cutting Source->Node would mean choosing "tagged" for a hard-stripped node.
void PTSanMinCutGraphBuilder::forceStripped(unsigned Node) {
  Graph.addEdge(SourceNode, Node, InfCost);
}

// Cutting Node->Sink would mean choosing "stripped" for a hard-tagged node.
void PTSanMinCutGraphBuilder::forceTagged(unsigned Node) {
  Graph.addEdge(Node, SinkNode, InfCost);
}

// Runtime cost is dominated by loop frequency. This matches the local hoister's
// "each loop level is roughly 4x more expensive" model, but clamps earlier so
// finite costs stay comfortably below InfCost.
uint64_t PTSanMinCutGraphBuilder::getCostMultiplier(Value *V) const {
  auto *I = dyn_cast<Instruction>(V);
  if (!I) {
    return 1;
  }

  unsigned LoopDepth = std::min<unsigned>(LI.getLoopDepth(I->getParent()), 16);
  return UINT64_C(1) << (2 * LoopDepth);
}

// Bound every lane's total byte offset from Base. ScalarEvolution handles the
// scalar prefix; vector GEPs add lane-wise displacement ranges. Unknown vector
// producers (including PHIs and mixed-base vectors) stay on the stripped path.
static std::optional<ConstantRange>
getVectorGEPOffsetRange(Value *Ptr, Value *Base, PTSanAnalysisState &PTA) {
  if (!Ptr->getType()->isVectorTy()) {
    const SCEV *Offset =
        PTA.SE.getMinusSCEV(PTA.SE.getSCEV(Ptr), PTA.SE.getSCEV(Base));
    if (isa<SCEVCouldNotCompute>(Offset)) {
      return std::nullopt;
    }

    return PTA.SE.getSignedRange(Offset);
  }
  if (Value *Splat = getSplatValue(Ptr)) {
    return getVectorGEPOffsetRange(Splat, Base, PTA);
  }

  auto *GEP = dyn_cast<GEPOperator>(Ptr);
  if (!GEP) {
    return std::nullopt;
  }
  const DataLayout &DL = PTA.F.getParent()->getDataLayout();
  unsigned Bits = DL.getIndexTypeSizeInBits(Ptr->getType());
  MapVector<Value *, APInt> Indices;
  APInt ConstantOffset(Bits, 0);
  if (!GEP->collectOffset(DL, Bits, Indices, ConstantOffset)) {
    return std::nullopt;
  }

  auto Offset = getVectorGEPOffsetRange(GEP->getPointerOperand(), Base, PTA);
  if (!Offset) {
    return std::nullopt;
  }
  *Offset = Offset->add(ConstantRange(ConstantOffset));
  for (const auto &[Index, Scale] : Indices) {
    // KnownBits describes every lane. GEP sign-extends or truncates indices to
    // the pointer index width; ConstantRange accounts for wrapping arithmetic.
    ConstantRange IndexRange = ConstantRange::fromKnownBits(
        computeKnownBits(Index, DL), true).sextOrTrunc(Bits);
    *Offset = Offset->add(IndexRange.smul_fast(ConstantRange(Scale)));
  }
  return Offset;
}

bool PTSanMinCutGraphBuilder::canKeepID32GEPTagged(GetElementPtrInst &GEP) {
  Value *Base = PTA.PTU.findBasePointer(&GEP);
  // The base walk can stop at an interior pointer, such as an argument. Sources,
  // known allocators and individually registered globals name the object start.
  if (!DS.getAsAnchor<SourceAnchor>(Base) && !isAllocLikeFn(Base, &PTA.TLI)) {
    auto *GV = dyn_cast<GlobalVariable>(Base);
    if (!GV || !DS.getGlobalProxy(GV)) {
      return false;
    }
    // Constants can share a region's ID, but a linker section start names the
    // registered base even when its declaration is const.
    bool IsSectionStart = GV->isDeclaration() &&
                          GV->getName().starts_with("__start_");
    if ((GV->isConstant() && !IsSectionStart) ||
        GV->getName().starts_with("__stop_")) {
      return false;
    }
  }

  // Origins are at most 2^31, so any nonnegative total offset below 2^31
  // fits the payload. This does not require knowing the object's size.
  if (GEP.getType()->isVectorTy()) {
    auto Offset = getVectorGEPOffsetRange(&GEP, Base, PTA);
    if (!Offset) {
      return false;
    }
    unsigned Bits = Offset->getBitWidth();
    return Offset->getUnsignedMax().ult(APInt(Bits, 1ULL << 31));
  }
  ScalarEvolution &SE = PTA.SE;
  const SCEV *Offset = SE.getMinusSCEV(SE.getSCEV(&GEP), SE.getSCEV(Base));
  if (isa<SCEVCouldNotCompute>(Offset)) {
    return false;
  }
  return SE.isKnownPredicate(
      ICmpInst::ICMP_ULT, Offset,
      SE.getConstant(Offset->getType(), 1ULL << 31));
}

// Create each value node and apply its required polarity once.
unsigned PTSanMinCutGraphBuilder::getValueNode(Value *V) {
  if (ValueNodes.contains(V)) {
    return ValueNodes.at(V);
  }

  unsigned Node = Graph.addNode();
  ValueNodes[V] = Node;

  if (auto Required = getRequiredPolarity(DS, V)) {
    if (*Required == Polarity::Stripped) {
      forceStripped(Node);
    } else {
      forceTagged(Node);
    }
  }

  if (Cl32BitIDs) {
    if (auto *GEP = dyn_cast<GetElementPtrInst>(V);
        GEP && !canKeepID32GEPTagged(*GEP)) {
      forceStripped(Node);
    }
  }

  return Node;
}

PTSanMinCutGraphBuilder::ConversionNodes
PTSanMinCutGraphBuilder::getConversionNodes(Value *V) {
  if (!ConversionNodesForValue.contains(V)) {
    unsigned ValueNode = getValueNode(V);
    ConversionNodes Conversions{Graph.addNode(), Graph.addNode()};
    ConversionNodesForValue[V] = Conversions;
    uint64_t Multiplier = getCostMultiplier(V);
    Graph.addEdge(ValueNode, Conversions.Tag, TagPointerCost * Multiplier);
    Graph.addEdge(Conversions.Strip, ValueNode, StripPointerCost * Multiplier);
  }

  return ConversionNodesForValue.at(V);
}

// Associate an exact rewriteable operand with the polarity required by Node.
// Conversion nodes let several such boundaries share one concrete strip/tag.
void PTSanMinCutGraphBuilder::addUseBoundary(Use &U, unsigned Node) {
  Value *V = U.get();
  Boundaries.push_back({&U, Node});

  // A conversion emitted immediately after V is shared by every boundary use.
  // These nodes charge the conversion once if any use needs the other polarity.
  ConversionNodes Conversions = getConversionNodes(V);
  Graph.addEdge(Conversions.Tag, Node, InfCost);
  Graph.addEdge(Node, Conversions.Strip, InfCost);
}

// Encode the function's pointer-polarity problem. The important distinction is
// between values, which have one chosen polarity, and uses, which are exact
// conversion boundaries.
void PTSanMinCutGraphBuilder::buildGraph() {
  for (Instruction &I : instructions(F)) {
    if (auto Check = DS.getAsAnchor<CheckAnchor>(&I)) {
      unsigned CheckNode = Graph.addNode();
      addUseBoundary(Check->Call->getOperandUse(CheckAnchor::RangeMinIndex),
                     CheckNode);
      uint64_t Multiplier = getCostMultiplier(Check->Call);
      Graph.addEdge(SourceNode, CheckNode, CheckWithTaggedCost * Multiplier);
      Graph.addEdge(CheckNode, SinkNode, CheckWithStrippedCost * Multiplier);
      continue;
    }

    // With arithmetic stripping disabled, pointer subtraction retains IDs.
    // Neither operand requires a particular representation, but both must use
    // the same one so their ID bits cancel.
    if (!PTA.ClearIDsOnArith) {
      if (auto *Sub = dyn_cast<BinaryOperator>(&I)) {
        if (Sub->getOpcode() == Instruction::Sub) {
          auto *Left = dyn_cast<PtrToIntInst>(Sub->getOperand(0));
          auto *Right = dyn_cast<PtrToIntInst>(Sub->getOperand(1));
          if (Left && Right) {
            unsigned SubNode = Graph.addNode();
            addUseBoundary(Left->getOperandUse(0), SubNode);
            addUseBoundary(Right->getOperandUse(0), SubNode);
          }
        }
      }
    }

    if (auto *Cmp = dyn_cast<ICmpInst>(&I)) {
      if (Cmp->getOperand(0)->getType()->isPtrOrPtrVectorTy()) {
        // User-generated pointer compares should already have strip anchors
        // from discovery. The pointer compares still visible here are generated
        // while materializing comparisons used by PTSan range metadata. Their
        // operands are alternate endpoints of the same range, so either
        // polarity is fine as long as both operands match.
        unsigned CmpNode = Graph.addNode();
        addUseBoundary(Cmp->getOperandUse(0), CmpNode);
        addUseBoundary(Cmp->getOperandUse(1), CmpNode);
      }
      continue;
    }

    if (auto Source = DS.getAsAnchor<SourceAnchor>(&I)) {
      unsigned ResultNode = getValueNode(Source->Call);
      addUseBoundary(Source->Call->getOperandUse(0), ResultNode);
      continue;
    }

    if (!I.getType()->isPtrOrPtrVectorTy()) {
      continue;
    }

    if (auto Strip = DS.getAsAnchor<StripAnchor>(&I)) {
      getValueNode(Strip->Call);
      // Existing strips realize the same conversion as stripped boundary uses,
      // so they activate the shared conversion rather than paying a second
      // cost.
      forceStripped(getConversionNodes(Strip->getValue()).Strip);
      continue;
    }

    if (auto Tagged = DS.getAsAnchor<TaggedAnchor>(&I)) {
      unsigned TaggedNode = getValueNode(Tagged->Call);
      addUseBoundary(Tagged->Call->getOperandUse(0), TaggedNode);
      continue;
    }

    if (Use *U = getPTSanRewriteablePointerUse(I)) {
      // Transparent pointer ops only reshape the pointer expression. They do
      // not create a new tag state, so the input use and result must agree.
      addUseBoundary(*U, getValueNode(&I));
      continue;
    }

    if (auto *PHI = dyn_cast<PHINode>(&I)) {
      unsigned ResultNode = getValueNode(PHI);
      for (unsigned I = 0; I < PHI->getNumIncomingValues(); ++I) {
        addUseBoundary(PHI->getOperandUse(I), ResultNode);
        Value *Incoming = PHI->getIncomingValue(I);
        auto *Invoke = dyn_cast<InvokeInst>(Incoming);
        if (isa<CallBrInst>(Incoming) ||
            (Invoke && Invoke->getParent() == PHI->getIncomingBlock(I))) {
          // A callbr has no single insertion point for a result conversion.
          // An invoke also cannot supply a conversion on its normal edge
          // directly into a PHI. Keep these PHIs tagged and convert after them.
          forceTagged(ResultNode);
        }
      }
      continue;
    }

    if (auto *Select = dyn_cast<SelectInst>(&I)) {
      unsigned ResultNode = getValueNode(Select);
      addUseBoundary(Select->getOperandUse(1), ResultNode);
      addUseBoundary(Select->getOperandUse(2), ResultNode);
      continue;
    }

    if (auto *Shuffle = dyn_cast<ShuffleVectorInst>(&I)) {
      unsigned ResultNode = getValueNode(Shuffle);
      auto [UsesLHS, UsesRHS] = getPTSanUsedShuffleOperands(*Shuffle);
      if (UsesLHS) {
        addUseBoundary(Shuffle->getOperandUse(0), ResultNode);
      }
      if (UsesRHS) {
        addUseBoundary(Shuffle->getOperandUse(1), ResultNode);
      }
      continue;
    }

    assert(getRequiredPolarity(DS, &I).has_value());
    getValueNode(&I);
  }
}

SmallVector<SolvedBoundary, 32>
PTSanMinCutGraphBuilder::solveAndAnnotate() {
  buildGraph();
  DenseSet<unsigned> SourceSide =
      Graph.getSplit(SourceNode, SinkNode, InfCost);
  auto getPolarity = [&](unsigned Node) {
    return SourceSide.contains(Node) ? Polarity::Stripped : Polarity::Tagged;
  };

  // First, iterate over all graph nodes which represent values
  // and assign the solved polarity to each value (technically the value's
  // SSA value/output. At this point, inputs may not match outputs -- that is
  // what rewriting is for.
  for (auto &[V, Node] : ValueNodes) {
    if (auto *I = dyn_cast<Instruction>(V)) {
      setPTSanPolarityMetadata(*I, getPolarity(Node));
    }
  }

  // Now iterate over all solved *uses* and assign them a polarity. This is the
  // required polarity for that use, which may *differ* from that of the actual
  // values. Where they differ, we'll insert a conversion.
  SmallVector<SolvedBoundary, 32> SolvedBoundaries;
  for (GraphBoundary &Boundary : Boundaries) {
    SolvedBoundaries.push_back({Boundary.U, getPolarity(Boundary.Node)});
  }

  return SolvedBoundaries;
}

class PTSanMinCutRewriter {
private:
  PointerTagSanitizer &DS;
  PTSanUtils &PTU;
  Function &F;
  DominatorTree &DT;
  PTSanPolarityReader Polarities;

  // Rewriting can make several boundaries ask for the same concrete conversion.
  // A terminator result may need separate conversions on different paths.
  // Reuse only a representative that dominates the requested location.
  DenseMap<Value *, SmallVector<Value *, 1>> RetagIDs;
  // Value -> shared strip anchor for stripped boundary uses.
  DenseMap<Value *, SmallVector<CallInst *, 1>> StrippedValues;
  // Value -> shared tagged representative: a tag anchor or global proxy load.
  DenseMap<Value *, SmallVector<Value *, 1>> RetaggedValues;

  template <typename T>
  T *findDominating(const SmallVectorImpl<T *> &Candidates, Instruction *UseIP);
  Value *getRetagID(Value *V, Instruction *UseIP);
  Value *getInsertedStrip(Value *V, Use &U);
  Value *getRetaggedValue(Value *V, Instruction *UseIP);
  void rewriteBoundary(const SolvedBoundary &Boundary);

public:
  PTSanMinCutRewriter(PTSanAnalysisState &PTA)
      : DS(PTA.DS), PTU(PTA.PTU), F(PTA.F), DT(PTA.DT),
        Polarities(DS, PTA.F) {}

  void rewrite(ArrayRef<SolvedBoundary> Boundaries);
};

template <typename T>
T *PTSanMinCutRewriter::findDominating(const SmallVectorImpl<T *> &Candidates,
                                      Instruction *UseIP) {
  for (T *Candidate : Candidates) {
    if (DT.dominates(Candidate, UseIP)) {
      return Candidate;
    }
  }
  return nullptr;
}

// Recover V's allocation ID at a location where the consumer can use it.
Value *PTSanMinCutRewriter::getRetagID(Value *V, Instruction *UseIP) {
  if (Value *ID = findDominating(RetagIDs[V], UseIP)) {
    return ID;
  }

  auto cacheID = [&](Value *ID) {
    RetagIDs[V].push_back(ID);
    return ID;
  };

  // Instrumented stack allocations obtain IDs through SourceAnchor. Bare
  // allocas, including those in strip-only libraries, retain the raw ID zero.
  if (isa<ConstantPointerNull, AllocaInst>(V)) {
    return cacheID(ConstantInt::get(DS.U64Ty, 0));
  }

  if (auto Strip = DS.getAsAnchor<ID32StripAnchor>(V)) {
    return cacheID(Strip->getID());
  }

  if (auto Strip = DS.getAsAnchor<ID16StripAnchor>(V)) {
    return cacheID(getRetagID(Strip->getValue(), UseIP));
  }

  if (auto Tagged = DS.getAsAnchor<TaggedAnchor>(V)) {
    return cacheID(getRetagID(Tagged->getValue(), UseIP));
  }

  if (auto Tag = DS.getAsAnchor<TagAnchor>(V)) {
    return cacheID(Tag->getID());
  }

  if (isa<GlobalValue>(V)) {
    if (!DS.getGlobalProxy(V)) {
      return cacheID(ConstantInt::get(DS.U64Ty, 0));
    }

    Value *Tagged = getRetaggedValue(V, UseIP);
    Instruction *IP = PTU.getInsertionPointForUse(Tagged, UseIP).getIP();
    return cacheID(GetIDAnchor::create(DS, IP, Tagged).Call);
  }

  if (DS.getAsAnchor<SourceAnchor>(V)) {
    Instruction *IP = PTU.getInsertionPointForUse(V, UseIP).getIP();
    return cacheID(GetIDAnchor::create(DS, IP, V).Call);
  }

  // Values with a common base have a common allocation ID. Skip their
  // intervening pointer dataflow and recover the ID directly from that base.
  Value *Base = PTU.findBasePointer(V);
  if (Base != V) {
    return cacheID(getRetagID(Base, UseIP));
  }

  if (Polarities.getPolarity(V) == Polarity::Tagged) {
    Instruction *IP = PTU.getInsertionPointForUse(V, UseIP).getIP();
    return cacheID(GetIDAnchor::create(DS, IP, V).Call);
  }

  if (auto *PHI = dyn_cast<PHINode>(V)) {
    // A stripped pointer PHI needs a matching ID PHI so retags after the join
    // restore the correct incoming ID. Whole-vector incoming values with
    // scalar bases can keep scalar IDs even when those bases differ.
    Type *IDTy = DS.U64Ty;
    for (Value *Incoming : PHI->incoming_values()) {
      if (PTU.findBasePointer(Incoming)->getType()->isVectorTy()) {
        IDTy = V->getType()->getWithNewType(DS.U64Ty);
        break;
      }
    }

    auto *ID = PHINode::Create(IDTy, PHI->getNumIncomingValues(),
                               PHI->getName() + ".ptsan.id",
                               PHI->getIterator());
    RetagIDs[V].push_back(ID);
    for (unsigned I = 0; I < PHI->getNumIncomingValues(); ++I) {
      Instruction *IncomingIP = PHI->getIncomingBlock(I)->getTerminator();
      Value *IncomingID = getRetagID(PHI->getIncomingValue(I), IncomingIP);
      IncomingID = PTU.splatToMatchType(IncomingID, IDTy, IncomingIP);
      ID->addIncoming(IncomingID, PHI->getIncomingBlock(I));
    }

    return ID;
  }

  // Selects are the same idea but they can work per-row, so we have to handle
  // the vector cases a little more carefully
  if (auto *Select = dyn_cast<SelectInst>(V)) {
    Value *TrueID = getRetagID(Select->getTrueValue(), Select);
    Value *FalseID = getRetagID(Select->getFalseValue(), Select);
    bool IsVector = Select->getCondition()->getType()->isVectorTy() ||
                    TrueID->getType()->isVectorTy() ||
                    FalseID->getType()->isVectorTy();
    Type *IDTy = DS.U64Ty;
    if (IsVector) {
      IDTy = Select->getType()->getWithNewType(DS.U64Ty);
    }

    Instruction *IP = PTU.getInsertionPointForUse(Select, UseIP).getIP();
    PTSanIRB IRB(DS, IP);
    TrueID = PTU.splatToMatchType(TrueID, IDTy, IP);
    FalseID = PTU.splatToMatchType(FalseID, IDTy, IP);
    // Selects have ordinary insertion points, so an ID select is simpler than
    // forcing the pointer select to stay tagged.
    // Recursive ID recovery can reference PHIs whose incoming values are
    // still being populated. Insert the select without simplifying it: the
    // simplifier requires complete PHIs when inspecting their incoming edges.
    Value *ID = IRB.Insert(
        SelectInst::Create(Select->getCondition(), TrueID, FalseID),
        Select->getName() + ".ptsan.id");
    return cacheID(ID);
  }

  if (auto *Shuffle = dyn_cast<ShuffleVectorInst>(V)) {
    Type *IDTy = Shuffle->getOperand(0)->getType()->getWithNewType(DS.U64Ty);
    Value *LHS = getRetagID(Shuffle->getOperand(0), Shuffle);
    LHS = PTU.splatToMatchType(LHS, IDTy, Shuffle);
    Value *RHS = getRetagID(Shuffle->getOperand(1), Shuffle);
    RHS = PTU.splatToMatchType(RHS, IDTy, Shuffle);

    Instruction *IP = PTU.getInsertionPointForUse(Shuffle, UseIP).getIP();
    PTSanIRB IRB(DS, IP);
    Value *ID = IRB.CreateShuffleVector(LHS, RHS, Shuffle->getShuffleMask(),
                                           V->getName() + ".ptsan.id");
    return cacheID(ID);
  }

  llvm_unreachable("unexpected stripped pointer root");
}

Value *PTSanMinCutRewriter::getInsertedStrip(Value *V, Use &U) {
  Instruction *UseIP = PTU.getUseInsertionPoint(U);
  if (CallInst *Strip = findDominating(StrippedValues[V], UseIP)) {
    return Strip;
  }

  CallInst *Strip;
  if (Cl32BitIDs) {
    Value *ID = getRetagID(V, UseIP);
    Instruction *IP = PTU.getFirstCommonInsertionPoint(
                             {PTU.getInsertionPointForUse(V, U),
                              PTU.getInsertionPointForUse(ID, U)})
                          .getIP();
    auto Base = LoadBoundAnchor::createBase(DS, IP, ID);
    Strip = ID32StripAnchor::create(DS, IP, V, ID, Base).Call;
  } else {
    Strip = ID16StripAnchor::create(
                DS, PTU.getInsertionPointForUse(V, U).getIP(), V).Call;
  }
  StrippedValues[V].push_back(Strip);
  return Strip;
}

// Materialize the tagged representative for a stripped value. Retags use the
// structural ID for the stripped value's provenance, not whichever ID happens
// to be nearest in the rewritten IR.
Value *PTSanMinCutRewriter::getRetaggedValue(Value *V, Instruction *UseIP) {
  if (Value *Tagged = findDominating(RetaggedValues[V], UseIP)) {
    return Tagged;
  }

  if (isa<GlobalValue>(V)) {
    if (GlobalValue *Proxy = DS.getGlobalProxy(V)) {
      PTSanIRB IRB(DS, PTU.getInsertionPointForUse(V, UseIP).getIP());
      auto *Tagged = IRB.CreateLoad(DS.PtrTy, Proxy, V->getName() + ".tagged");
      setPTSanPolarityMetadata(*Tagged, Polarity::Tagged);
      RetaggedValues[V].push_back(Tagged);
      return Tagged;
    }
  }

  Value *ID = getRetagID(V, UseIP);
  Instruction *IP = PTU.getFirstCommonInsertionPoint(
                           {PTU.getInsertionPointForUse(V, UseIP),
                            PTU.getInsertionPointForUse(ID, UseIP)})
                        .getIP();
  CallInst *Tag;
  if (Cl32BitIDs) {
    auto Base = LoadBoundAnchor::createBase(DS, IP, ID);
    Tag = ID32TagAnchor::create(DS, IP, V, ID, Base).Call;
  } else {
    Tag = ID16TagAnchor::create(DS, IP, V, ID).Call;
  }
  RetaggedValues[V].push_back(Tag);
  return Tag;
}

// Rewrite one exact conversion boundary according to the solved labels. This is
// where the graph's abstract cut becomes concrete strip/tag anchors in IR.
void PTSanMinCutRewriter::rewriteBoundary(const SolvedBoundary &B) {
  Value *V = B.U->get();

  if (B.RequiredPolarity == Polarities.getPolarity(V)) {
    return;
  }

  if (B.RequiredPolarity == Polarity::Stripped) {
    B.U->set(getInsertedStrip(V, *B.U));
  } else {
    B.U->set(getRetaggedValue(V, PTU.getUseInsertionPoint(*B.U)));
  }
}

// Apply the solved graph to IR. General anchor cleanup runs afterward.
void PTSanMinCutRewriter::rewrite(ArrayRef<SolvedBoundary> Boundaries) {
  // Resolve raw-pointer ID requests through the same machinery as retags.
  // Tagged inputs and allocation sources can be lowered directly.
  for (GetIDAnchor ID : DS.collectAnchors<GetIDAnchor>(F)) {
    Value *Base = ID.getBase();
    if (!Base->getType()->isPtrOrPtrVectorTy() ||
        DS.getAsAnchor<SourceAnchor>(Base) ||
        Polarities.getPolarity(Base) == Polarity::Tagged) {
      continue;
    }

    Value *Resolved = PTU.splatToMatchType(
        getRetagID(Base, ID.Call), ID.Call->getType(), ID.Call);
    ID.Call->replaceAllUsesWith(Resolved);
    ID.erase();
  }

  // Resolve conversion IDs before rewriting operands can obscure their
  // original base-pointer structure. Default-mode strips do not consume IDs.
  for (const SolvedBoundary &B : Boundaries) {
    Value *V = B.U->get();
    if (B.RequiredPolarity != Polarities.getPolarity(V) &&
        (Cl32BitIDs || B.RequiredPolarity == Polarity::Tagged)) {
      getRetagID(V, PTU.getUseInsertionPoint(*B.U));
    }
  }

  for (const SolvedBoundary &B : Boundaries) {
    rewriteBoundary(B);
  }
}

void runMinCutStripIDHoisting(PTSanAnalysisState &PTA) {
  PTSanMinCutGraphBuilder Builder(PTA);
  SmallVector<SolvedBoundary, 32> Boundaries = Builder.solveAndAnnotate();
  PTSanMinCutRewriter Rewriter(PTA);
  Rewriter.rewrite(Boundaries);

  // Finalize surviving conversions after rewriting has connected all users.
  PTA.PTU.KnownBases.clear();
  PTSanAnchorOptimizer Anchors(PTA);
  Anchors.foldRedundantStripAnchors();

  Anchors.foldStripOfTagAnchors();
  Anchors.foldTagOfStripAnchors();
  Anchors.foldGetIDOfTagAnchors();
  Anchors.hoistGetIDAnchors();
  Anchors.mergeGetIDAnchors();
  if (Cl32BitIDs)
    optimizeLoadBoundAnchors(PTA);
  Anchors.hoistStripAnchors();
  Anchors.mergeStripAnchors();
  Anchors.eraseUnusedDependencyAnchors();
}

} // namespace llvm::ptsan
