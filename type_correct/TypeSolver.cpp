/**
 * @file TypeSolver.cpp
 * @brief Implementation of Solver with Pointer Arithmetic Logic.
 *
 * Implements the resolution logic. Specifically upgraded to handle
 * `ptrdiff_t` constraints. Since pointer arithmetic defines memory
 * access reachability, these constraints are treated as high-priority
 * floors for type width.
 *
 * @author SamuelMarks
 * @license CC0
 */

#include "TypeSolver.h"
#include <algorithm>
#include <clang/AST/ASTContext.h>
#include <limits>
#include <queue>

namespace type_correct {

//-----------------------------------------------------------------------------
// ValueRange Logic
//-----------------------------------------------------------------------------

void ValueRange::Union(const ValueRange &Other) {
  if (!Other.HasMin && !Other.HasMax)
    return;
  if (Other.HasMin) {
    if (!HasMin) {
      Min = Other.Min;
      HasMin = true;
    } else {
      Min = std::min(Min, Other.Min);
    }
  }
  if (Other.HasMax) {
    if (!HasMax) {
      Max = Other.Max;
      HasMax = true;
    } else {
      Max = std::max(Max, Other.Max);
    }
  }
}

//-----------------------------------------------------------------------------
// TypeSolver Implementation
//-----------------------------------------------------------------------------

TypeSolver::TypeSolver() {}

void TypeSolver::AddNode(const clang::NamedDecl *Decl,
                         clang::QualType CurrentType, bool IsFixed) {
  if (!Decl) return;
  auto It = Nodes.find(Decl);
  if (It == Nodes.end()) {
    Nodes[Decl] = NodeState(Decl, CurrentType, IsFixed);
  } else {
    if (IsFixed)
      Nodes[Decl].IsFixed = true;
  }
}

void TypeSolver::AddGlobalConstraint(const clang::NamedDecl *Decl, 
                                     clang::QualType GlobalType, 
                                     clang::ASTContext *Ctx) {
    if (!Decl) return;
    auto It = Nodes.find(Decl);
    if (It == Nodes.end()) {
        Nodes[Decl] = NodeState(Decl, GlobalType, /*IsFixed=*/false);
        Nodes[Decl].HasGlobalConstraint = true;
    } else {
        NodeState &NS = It->second;
        NS.ConstraintType = GetWider(NS.ConstraintType, GlobalType, Ctx);
        NS.HasGlobalConstraint = true;
    }
}

void TypeSolver::AddPointerOffsetUsage(const clang::NamedDecl *Decl) {
    if (!Decl) return;
    auto It = Nodes.find(Decl);
    if (It != Nodes.end()) {
        It->second.IsPtrOffset = true;
    }
    // If node doesn't exist yet, it will be created via AddConstraint later, 
    // but usually AddNode calls precede usage flagging. 
    // To be safe, we don't create an innovative node here without a Type.
}

void TypeSolver::AddEdge(const clang::NamedDecl *User,
                         const clang::NamedDecl *Used) {
  if (!User || !Used || User == Used) return;
  if (Nodes.find(User) == Nodes.end()) return;
  if (Nodes.find(Used) == Nodes.end()) return;

  Adjacency[User].push_back(Used);
  Adjacency[Used].push_back(User);
}

void TypeSolver::AddConstraint(const clang::NamedDecl *Decl,
                               clang::QualType Candidate,
                               const clang::Expr *BaseExpr,
                               clang::ASTContext *Ctx) {
  if (!Decl) return;
  auto It = Nodes.find(Decl);
  if (It == Nodes.end()) return;

  NodeState &NS = It->second;
  NS.ConstraintType = GetWider(NS.ConstraintType, Candidate, Ctx);
  if (BaseExpr)
    NS.BaseExpr = BaseExpr;
}

void TypeSolver::AddRangeConstraint(const clang::NamedDecl *Decl,
                                    ValueRange Range) {
  if (!Decl) return;
  auto It = Nodes.find(Decl);
  if (It == Nodes.end()) return;

  It->second.ComputedRange.Union(Range);
}

void TypeSolver::AddSymbolicConstraint(const clang::NamedDecl *Result,
                                       OpKind Op,
                                       const clang::NamedDecl *LHS,
                                       const clang::NamedDecl *RHS) {
  if (!Result || !LHS || !RHS) return;
  SymbolicConstraints.push_back(SymbolicConstraint(Result, Op, LHS, RHS));
}

clang::QualType TypeSolver::GetWider(clang::QualType A, clang::QualType B,
                                     clang::ASTContext *Ctx) {
  if (A.isNull()) return B;
  if (B.isNull()) return A;
  if (Ctx->hasSameType(A, B)) return A;
  if (A->isIncompleteType() || B->isIncompleteType()) return B;
  if (!A->isScalarType() || !B->isScalarType()) return B;

  uint64_t SizeA = Ctx->getTypeSize(A);
  uint64_t SizeB = Ctx->getTypeSize(B);

  // Width priority
  if (SizeB > SizeA) return B;
  if (SizeA > SizeB) return A;
  
  // Tie-breaker: ptrdiff_t preference
  // If sizes are equal (e.g. long vs long long on 64-bit linux, or int vs long on 32-bit), 
  // identifying the typedef stack is hard without "sugar".
  // However, Standard Types like size_t/ptrdiff_t are usually preferred over raw ints.
  // We approximate this by preferring Unsigned if different signedness.
  if (B->isUnsignedIntegerType() && A->isSignedIntegerType()) return B;

  return A;
}

clang::QualType TypeSolver::GetOptimalTypeForRange(const ValueRange &R,
                                                   clang::QualType Original,
                                                   clang::ASTContext *Ctx) {
  if (!R.HasMin && !R.HasMax) return Original;

  bool NeedsSigned = (R.HasMin && R.Min < 0);

  auto GetUint = [&](unsigned Width) {
    if (Width == 8) return Ctx->UnsignedCharTy;
    if (Width == 16) return Ctx->UnsignedShortTy;
    if (Width == 32) return Ctx->UnsignedIntTy;
    return Ctx->UnsignedLongLongTy;
  };

  auto GetInt = [&](unsigned Width) {
    if (Width == 8) return Ctx->SignedCharTy;
    if (Width == 16) return Ctx->ShortTy;
    if (Width == 32) return Ctx->IntTy;
    return Ctx->LongLongTy;
  };

  if (!NeedsSigned) {
    if (R.HasMax) {
      if (R.Max <= std::numeric_limits<uint8_t>::max()) return GetUint(8);
      if (R.Max <= std::numeric_limits<uint16_t>::max()) return GetUint(16);
      if (R.Max <= std::numeric_limits<uint32_t>::max()) return GetUint(32);
      return Ctx->getSizeType();
    }
  } else {
    int64_t AbsMax = std::max(std::abs(R.Min), std::abs(R.Max));
    if (AbsMax <= std::numeric_limits<int8_t>::max()) return GetInt(8);
    if (AbsMax <= std::numeric_limits<int16_t>::max()) return GetInt(16);
    if (AbsMax <= std::numeric_limits<int32_t>::max()) return GetInt(32);
    return Ctx->LongLongTy;
  }
  return Original;
}

std::map<const clang::NamedDecl *, NodeState>
TypeSolver::Solve(clang::ASTContext *Ctx) {
  std::map<const clang::NamedDecl *, NodeState> Updates;
  std::set<const clang::NamedDecl *> Visited;

  // 1. Component Propagation
  for (auto &Pair : Nodes) {
    const clang::NamedDecl *StartNode = Pair.first;
    if (Visited.count(StartNode)) continue;

    std::vector<const clang::NamedDecl *> Component;
    std::queue<const clang::NamedDecl *> Q;
    Q.push(StartNode);
    Visited.insert(StartNode);

    ValueRange MergedRange; MergedRange.Min = 0; MergedRange.Max = 0; 
    clang::QualType MergedConstraint = Pair.second.ConstraintType;
    bool ComponentIsFixed = Pair.second.IsFixed;
    
    // Track if ANY node in the component is used as a pointer offset
    bool ComponentIsPtrOffset = Pair.second.IsPtrOffset;

    while (!Q.empty()) {
      const clang::NamedDecl *Curr = Q.front();
      Q.pop();
      Component.push_back(Curr);

      NodeState &State = Nodes[Curr];
      MergedRange.Union(State.ComputedRange);
      MergedConstraint = GetWider(MergedConstraint, State.ConstraintType, Ctx);
      
      if (State.IsFixed) ComponentIsFixed = true;
      if (State.IsPtrOffset) ComponentIsPtrOffset = true;

      for (const auto *Neighbor : Adjacency[Curr]) {
        if (!Visited.count(Neighbor)) {
          Visited.insert(Neighbor);
          Q.push(Neighbor);
        }
      }
    }

    // Apply strict requirements for Ptr Offsets
    if (ComponentIsPtrOffset) {
        // Must be at least ptrdiff_t wide
        clang::QualType PtrDiff = Ctx->getPointerDiffType();
        MergedConstraint = GetWider(MergedConstraint, PtrDiff, Ctx);
    }

    for (const auto *Node : Component) {
      Nodes[Node].ConstraintType = MergedConstraint;
      Nodes[Node].ComputedRange = MergedRange;
      // Propagate offset flag for symbolic stage
      if (ComponentIsPtrOffset) Nodes[Node].IsPtrOffset = true;
    }
  }

  // 2. Symbolic Propagation
  bool Changed = true;
  int Iterations = 0;
  // Increase iteration count slightly for complex propagations
  const int MAX_ITER = 25;

  while (Changed && Iterations < MAX_ITER) {
    Changed = false;
    Iterations++;

    for (const auto &SC : SymbolicConstraints) {
      NodeState &TargetState = Nodes[SC.Result];
      NodeState &LState = Nodes[SC.LHS];
      NodeState &RState = Nodes[SC.RHS];

      clang::QualType OpType = GetWider(LState.ConstraintType, RState.ConstraintType, Ctx);
      
      // If either operand is a pointer offset, the operation is likely pointer arithmetic related
      // or index arithmetic. The result must support that width.
      if (LState.IsPtrOffset || RState.IsPtrOffset) {
           OpType = GetWider(OpType, Ctx->getPointerDiffType(), Ctx);
      }

      // Forward
      clang::QualType NewTarget = GetWider(TargetState.ConstraintType, OpType, Ctx);
      if (NewTarget != TargetState.ConstraintType) {
        TargetState.ConstraintType = NewTarget;
        Changed = true;
      }

      // Backward
      uint64_t TargetSize = Ctx->getTypeSize(TargetState.ConstraintType);
      uint64_t OpSize = Ctx->getTypeSize(OpType);

      if (TargetSize > OpSize) {
         if (!LState.IsFixed) {
            clang::QualType NewL = GetWider(LState.ConstraintType, TargetState.ConstraintType, Ctx);
            if (NewL != LState.ConstraintType) { LState.ConstraintType = NewL; Changed = true; }
         }
         if (!RState.IsFixed) {
            clang::QualType NewR = GetWider(RState.ConstraintType, TargetState.ConstraintType, Ctx);
            if (NewR != RState.ConstraintType) { RState.ConstraintType = NewR; Changed = true; }
         }
      }
    }
  }

  // 3. Finalize
  for (auto &Pair : Nodes) {
    const clang::NamedDecl *Node = Pair.first;
    NodeState &State = Pair.second;

    if (State.IsFixed) continue;

    clang::QualType Optimal;

    // Pointer Arithmetic Semantics: 
    // If it is an offset, we IGNORE strict Range Analysis shrinking if the range is small.
    // Example: for(int i=0; i<10; i++) p[i];
    // i is 0..9. OptimalRange says 'char'. 
    // But p[i] implies pointer arithmetic. On 64-bit, we prefer standard types (ptrdiff_t/size_t) 
    // to avoid implicit cast warnings or partial register stalls.
    // So if IsPtrOffset, we floor at ptrdiff_t.
    
    if (State.IsPtrOffset) {
        Optimal = GetWider(State.ConstraintType, Ctx->getPointerDiffType(), Ctx);
    } 
    else if (State.ComputedRange.HasMax) {
        Optimal = GetOptimalTypeForRange(State.ComputedRange, State.OriginalType, Ctx);
    } else {
        Optimal = State.ConstraintType;
    }

    Optimal = GetWider(Optimal, State.ConstraintType, Ctx);

    if (Optimal != State.OriginalType) {
      State.ConstraintType = Optimal;
      Updates[Node] = State;
    }
  }

  return Updates;
}

} // namespace type_correct