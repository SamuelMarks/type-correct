/** 
 * @file TypeSolver.cpp
 * @brief Implementation of Solver with Tarjan's SCC Resolution. 
 * 
 * Replaces simple BFS propagation with Tarjan's Algorithm to detect and solve
 * strongly connected components atomically. 
 * 
 * @author SamuelMarks
 * @license CC0
 */ 

#include "TypeSolver.h" 
#include <algorithm>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <limits>
#include <queue>

using namespace clang; 

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

TypeSolver::TypeSolver() : TarjanIndexCounter(0) {} 

void TypeSolver::AddNode(const NamedDecl *Decl, QualType CurrentType, 
                         bool IsFixed, bool IsTypedef) { 
  if (!Decl) 
    return; 

  auto It = Nodes.find(Decl); 
  if (It == Nodes.end()) { 
    Nodes[Decl] = NodeState(Decl, CurrentType, IsFixed, IsTypedef); 
  } else { 
    if (IsFixed) 
      Nodes[Decl].IsFixed = true; 
    if (IsTypedef) 
      Nodes[Decl].IsTypedef = true; 
  } 
} 

void TypeSolver::AddGlobalConstraint(const NamedDecl *Decl, 
                                     QualType GlobalType, ASTContext *Ctx) { 
  if (!Decl) 
    return; 

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

void TypeSolver::AddPointerOffsetUsage(const NamedDecl *Decl) { 
  if (!Decl) 
    return; 

  auto It = Nodes.find(Decl); 
  if (It != Nodes.end()) { 
    It->second.IsPtrOffset = true; 
  } 
} 

void TypeSolver::AddEdge(const NamedDecl *Target, const NamedDecl *Source) { 
  if (!Target || !Source || Target == Source) 
    return; 

  // Ensure both nodes exist in the graph before linking so SCC processing
  // sees a connected graph.
  if (Nodes.count(Target) == 0 || Nodes.count(Source) == 0) 
    return; 

  // Directed edge: Target depends on Source (Source's constraint should flow
  // along this edge into Target).
  Adjacency[Target].push_back(Source); 
} 

void TypeSolver::AddConstraint(const NamedDecl *Decl, QualType Candidate, 
                               const Expr *BaseExpr, ASTContext *Ctx) { 
  if (!Decl) 
    return; 

  auto It = Nodes.find(Decl); 
  if (It == Nodes.end()) 
    return; 

  NodeState &NS = It->second; 
  NS.ConstraintType = GetWider(NS.ConstraintType, Candidate, Ctx); 
  if (BaseExpr) 
    NS.BaseExpr = BaseExpr; 
} 

void TypeSolver::AddLoopComparisonConstraint(const NamedDecl *InductionVar, 
                                             const Expr *BoundExpr, 
                                             ASTContext *Ctx) { 
  if (!InductionVar || !BoundExpr) 
    return; 

  QualType BoundType = HelperGetType(BoundExpr, Ctx); 
  if (BoundType.isNull()) 
    return; 

  // The bound expression must be at least as wide as the induction variable.
  // Let BoundVar depend on InductionVar so width flows into the bound.
  const Expr *Inner = BoundExpr->IgnoreParenImpCasts(); 
  if (const auto *DR = dyn_cast<DeclRefExpr>(Inner)) { 
    if (const auto *BoundVar = dyn_cast<NamedDecl>(DR->getDecl())) { 
      AddEdge(BoundVar, InductionVar); 
    } 
  } 

  AddConstraint(InductionVar, BoundType, BoundExpr, Ctx); 
} 

void TypeSolver::AddRangeConstraint(const NamedDecl *Decl, ValueRange Range) { 
  if (!Decl) 
    return; 

  auto It = Nodes.find(Decl); 
  if (It == Nodes.end()) 
    return; 

  It->second.ComputedRange.Union(Range); 
} 

void TypeSolver::AddSymbolicConstraint(const NamedDecl *Result, OpKind Op, 
                                       const NamedDecl *LHS, 
                                       const NamedDecl *RHS) { 
  if (!Result || !LHS || !RHS) 
    return; 

  SymbolicConstraints.push_back(SymbolicConstraint(Result, Op, LHS, RHS)); 
} 

QualType TypeSolver::HelperGetType(const Expr *E, ASTContext *Ctx) { 
  if (!E) 
    return {}; 

  const Expr *Clean = E->IgnoreParenImpCasts(); 

  if (const auto *DR = dyn_cast<DeclRefExpr>(Clean)) { 
    if (const auto *V = dyn_cast<ValueDecl>(DR->getDecl())) 
      return V->getType(); 
  } 

  if (const auto *CE = dyn_cast<CallExpr>(Clean)) { 
    return CE->getCallReturnType(*Ctx); 
  } 

  if (const auto *Cast = dyn_cast<ExplicitCastExpr>(Clean)) { 
    return Cast->getTypeAsWritten(); 
  } 

  return Clean->getType(); 
} 

QualType TypeSolver::GetResolvedType(const NamedDecl *Decl) const { 
  if (!Decl) 
    return {}; 

  auto It = Nodes.find(Decl); 
  if (It != Nodes.end()) { 
    return It->second.ConstraintType; 
  } 

  if (const auto *V = dyn_cast<ValueDecl>(Decl)) 
    return V->getType(); 

  return {}; 
} 

QualType TypeSolver::GetWider(QualType A, QualType B, ASTContext *Ctx) { 
  if (A.isNull()) 
    return B; 
  if (B.isNull()) 
    return A; 

  if (Ctx->hasSameType(A, B)) 
    return A; 

  if (A->isIncompleteType() || B->isIncompleteType()) 
    return B; 

  // If we are dealing with non-scalar types (like vectors or structs),
  // prefer the new candidate (B) assuming it is a substitution rewrite.
  if (!A->isScalarType() || !B->isScalarType()) 
    return B; 

  uint64_t SizeA = Ctx->getTypeSize(A); 
  uint64_t SizeB = Ctx->getTypeSize(B); 

  if (SizeB > SizeA) 
    return B; 
  if (SizeA > SizeB) 
    return A; 

  if (B->isUnsignedIntegerType() && A->isSignedIntegerType()) 
    return B; 

  return A; 
} 

QualType TypeSolver::GetOptimalTypeForRange(const ValueRange &R, 
                                            QualType Original, 
                                            ASTContext *Ctx) { 
  if (!R.HasMin && !R.HasMax) 
    return Original; 

  bool NeedsSigned = (R.HasMin && R.Min < 0); 

  auto GetUint = [&](unsigned Width) { 
    if (Width == 8) 
      return Ctx->UnsignedCharTy; 
    if (Width == 16) 
      return Ctx->UnsignedShortTy; 
    if (Width == 32) 
      return Ctx->UnsignedIntTy; 
    return Ctx->UnsignedLongLongTy; 
  }; 

  auto GetInt = [&](unsigned Width) { 
    if (Width == 8) 
      return Ctx->SignedCharTy; 
    if (Width == 16) 
      return Ctx->ShortTy; 
    if (Width == 32) 
      return Ctx->IntTy; 
    return Ctx->LongLongTy; 
  }; 

  if (!NeedsSigned) { 
    if (R.HasMax) { 
      if (R.Max <= std::numeric_limits<uint8_t>::max()) 
        return GetUint(8); 
      if (R.Max <= std::numeric_limits<uint16_t>::max()) 
        return GetUint(16); 
      if (R.Max <= std::numeric_limits<uint32_t>::max()) 
        return GetUint(32); 
      return Ctx->getSizeType(); 
    } 
  } else { 
    int64_t AbsMax = std::max(std::abs(R.Min), std::abs(R.Max)); 

    if (AbsMax <= std::numeric_limits<int8_t>::max()) 
      return GetInt(8); 
    if (AbsMax <= std::numeric_limits<int16_t>::max()) 
      return GetInt(16); 
    if (AbsMax <= std::numeric_limits<int32_t>::max()) 
      return GetInt(32); 
    return Ctx->LongLongTy; 
  } 

  return Original; 
} 

//----------------------------------------------------------------------------- 
// SCC Logic (Tarjan's Alg) 
//----------------------------------------------------------------------------- 

std::map<const NamedDecl *, NodeState> TypeSolver::Solve(ASTContext *Ctx) { 
  TarjanIndexCounter = 0; 
  while (!TarjanStack.empty()) 
    TarjanStack.pop(); 
  TarjanState.clear(); 

  for (const auto &Pair : Nodes) { 
    const NamedDecl *V = Pair.first; 
    if (TarjanState.find(V) == TarjanState.end()) { 
      StrongConnect(V, Ctx); 
    } 
  } 

  bool Changed = true; 
  int Iterations = 0; 
  while (Changed && Iterations < 25) { 
    Changed = false; 
    Iterations++; 

    for (const auto &SC : SymbolicConstraints) { 
      NodeState &TargetState = Nodes[SC.Result]; 
      NodeState &LState = Nodes[SC.LHS]; 
      NodeState &RState = Nodes[SC.RHS]; 

      QualType OpType =
          GetWider(LState.ConstraintType, RState.ConstraintType, Ctx); 

      if (LState.IsPtrOffset || RState.IsPtrOffset) { 
        OpType = GetWider(OpType, Ctx->getPointerDiffType(), Ctx); 
      } 

      QualType NewTarget = GetWider(TargetState.ConstraintType, OpType, Ctx); 
      if (NewTarget != TargetState.ConstraintType) { 
        TargetState.ConstraintType = NewTarget; 
        Changed = true; 
      } 
    } 
  } 

  std::map<const NamedDecl *, NodeState> Updates; 
  for (auto &Pair : Nodes) { 
    const NamedDecl *Node = Pair.first; 
    NodeState &State = Pair.second; 

    if (State.IsFixed) 
      continue; 

    QualType Optimal; 
    if (State.IsPtrOffset) { 
      Optimal = GetWider(State.ConstraintType, Ctx->getPointerDiffType(), Ctx); 
    } else if (State.ComputedRange.HasMax) { 
      Optimal =
          GetOptimalTypeForRange(State.ComputedRange, State.OriginalType, Ctx); 
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

void TypeSolver::StrongConnect(const NamedDecl *V, ASTContext *Ctx) { 
  TarjanData &VData = TarjanState[V]; 
  VData.Index = TarjanIndexCounter; 
  VData.LowLink = TarjanIndexCounter; 
  TarjanIndexCounter++; 
  TarjanStack.push(V); 
  VData.OnStack = true; 

  if (Adjacency.count(V)) { 
    for (const NamedDecl *W : Adjacency[V]) { 
      if (TarjanState.find(W) == TarjanState.end()) { 
        StrongConnect(W, Ctx); 
        TarjanState[V].LowLink =
            std::min(TarjanState[V].LowLink, TarjanState[W].LowLink); 
      } else if (TarjanState[W].OnStack) { 
        TarjanState[V].LowLink =
            std::min(TarjanState[V].LowLink, TarjanState[W].Index); 
      } 
    } 
  } 

  if (VData.LowLink == VData.Index) { 
    std::vector<const NamedDecl *> SCC; 
    const NamedDecl *W = nullptr; 
    do { 
      W = TarjanStack.top(); 
      TarjanStack.pop(); 
      TarjanState[W].OnStack = false; 
      SCC.push_back(W); 
    } while (W != V); 

    ProcessSCC(SCC, Ctx); 
  } 
} 

void TypeSolver::ProcessSCC(const std::vector<const NamedDecl *> &SCC, 
                            ASTContext *Ctx) { 
  QualType UnifiedType; 
  ValueRange UnifiedRange; 
  bool IsFixed = false; 
  bool IsPtrOffset = false; 

  if (!SCC.empty()) { 
    UnifiedType = Nodes[SCC[0]].ConstraintType; 
  } 

  for (const NamedDecl *Member : SCC) { 
    NodeState &St = Nodes[Member]; 
    UnifiedType = GetWider(UnifiedType, St.ConstraintType, Ctx); 
    UnifiedRange.Union(St.ComputedRange); 

    if (St.IsFixed) 
      IsFixed = true; 
    if (St.IsPtrOffset) 
      IsPtrOffset = true; 
  } 

  if (IsPtrOffset) { 
    UnifiedType = GetWider(UnifiedType, Ctx->getPointerDiffType(), Ctx); 
  } 

  for (const NamedDecl *Member : SCC) { 
    NodeState &St = Nodes[Member]; 
    St.ConstraintType = UnifiedType; 
    St.ComputedRange = UnifiedRange; 
    if (IsPtrOffset) 
      St.IsPtrOffset = true; 
  } 
} 

} // namespace type_correct
