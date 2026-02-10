/**
 * @file TypeSolver.h
 * @brief Header for the Data-Flow Solver with Tarjan's SCC Resolution.
 *
 * This solver uses graph theory to propagate value widths.
 * It now implements Tarjan's Algorithm to detect Strongly Connected Components
 * (cycles of dependencies) and solves them atomically to prevent iterative
 * oscillation.
 *
 * @author SamuelMarks
 * @copyright CC0
 */

#ifndef TYPE_CORRECT_TYPESOLVER_H
#define TYPE_CORRECT_TYPESOLVER_H

#include "type_correct_export.h"
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Type.h>
#include <cstdint>
#include <map>
#include <set>
#include <stack>
#include <vector>

namespace type_correct {

/**
 * @struct ValueRange
 * @brief Represents a closed numerical interval [Min, Max].
 */
struct ValueRange {
  int64_t Min; ///< Minimum observed value.
  int64_t Max; ///< Maximum observed value.
  bool HasMin; ///< Flag indicating Min is valid.
  bool HasMax; ///< Flag indicating Max is valid.

  /// @brief Construct an empty range with no bounds.
  ValueRange() : Min(0), Max(0), HasMin(false), HasMax(false) {}
  /// @brief Construct a fixed range [Val, Val].
  ValueRange(int64_t Val) : Min(Val), Max(Val), HasMin(true), HasMax(true) {}
  /// @brief Construct a fixed range [Least, Most].
  ValueRange(int64_t Least, int64_t Most)
      : Min(Least), Max(Most), HasMin(true), HasMax(true) {}

  /// @brief Expand this range by unioning with another range.
  void Union(const ValueRange &Other);
};

/**
 * @enum OpKind
 * @brief Operations supported by the symbolic solver.
 */
enum class OpKind {
  None, ///< No operation.
  Add,  ///< Addition.
  Sub,  ///< Subtraction.
  Mul,  ///< Multiplication.
  Div   ///< Division.
};

/**
 * @struct SymbolicConstraint
 * @brief Result = LHS Op RHS.
 */
struct SymbolicConstraint {
  const clang::NamedDecl *Result; ///< Result symbol.
  OpKind Op;                      ///< Operation applied.
  const clang::NamedDecl *LHS;    ///< Left-hand operand.
  const clang::NamedDecl *RHS;    ///< Right-hand operand.

  /// @brief Construct a symbolic constraint.
  SymbolicConstraint(const clang::NamedDecl *Res, OpKind O,
                     const clang::NamedDecl *L, const clang::NamedDecl *R)
      : Result(Res), Op(O), LHS(L), RHS(R) {}
};

/**
 * @struct NodeState
 * @brief The solution state for a specific declaration.
 */
struct NodeState {
  const clang::NamedDecl *Decl;   ///< Declaration being solved.
  clang::QualType OriginalType;   ///< Original type from the AST.
  clang::QualType ConstraintType; ///< Current type constraint.
  ValueRange ComputedRange;       ///< Computed value range.
  bool IsFixed;                   ///< Whether the type is locked.
  bool HasGlobalConstraint;       ///< Whether a global constraint exists.
  bool IsPtrOffset;               ///< Whether this is a pointer offset.
  bool IsTypedef;                 ///< Whether this is a typedef symbol.
  const clang::Expr *BaseExpr;    ///< Base expression for constraints.

  /// @brief Construct an empty node state.
  NodeState()
      : Decl(nullptr), IsFixed(false), HasGlobalConstraint(false),
        IsPtrOffset(false), IsTypedef(false), BaseExpr(nullptr) {}

  /// @brief Construct a node state for a declaration.
  NodeState(const clang::NamedDecl *D, clang::QualType T, bool Locked = false,
            bool Typedef = false)
      : Decl(D), OriginalType(T), ConstraintType(T), IsFixed(Locked),
        HasGlobalConstraint(false), IsPtrOffset(false), IsTypedef(Typedef),
        BaseExpr(nullptr) {}
};

/**
 * @struct TarjanData
 * @brief Helper structure for Tarjan's Algorithm state per node.
 */
struct TarjanData {
  int Index;    ///< Discovery index.
  int LowLink;  ///< Lowest reachable index.
  bool OnStack; ///< Whether the node is in the stack.

  /// @brief Construct a default Tarjan state.
  TarjanData() : Index(-1), LowLink(-1), OnStack(false) {}
};

/**
 * @class TypeSolver
 * @brief Solves type constraints using SCC Cycle Resolution.
 */
class TYPE_CORRECT_EXPORT TypeSolver {
public:
  /// @brief Construct an empty solver.
  TypeSolver();

  /**
   * @brief Register a declaration in the solver graph.
   * @param Decl Declaration to add.
   * @param CurrentType Original type.
   * @param IsFixed Whether the type is locked.
   * @param IsTypedef Whether the symbol is a typedef.
   */
  void AddNode(const clang::NamedDecl *Decl, clang::QualType CurrentType,
               bool IsFixed, bool IsTypedef = false);

  /**
   * @brief Add a global constraint derived from CTU facts.
   * @param Decl Declaration to constrain.
   * @param GlobalType Widened type from facts.
   * @param Ctx AST context for type manipulation.
   */
  void AddGlobalConstraint(const clang::NamedDecl *Decl,
                           clang::QualType GlobalType, clang::ASTContext *Ctx);

  /**
   * @brief Mark a declaration as used in pointer arithmetic.
   * @param Decl Declaration to mark.
   */
  void AddPointerOffsetUsage(const clang::NamedDecl *Decl);

  /**
   * AddEdge(Target, Source):
   *
   * "Target depends on Source" in the sense that Source's required width
   * should flow into Target. So, if usage constraints land on Source, they
   * should propagate along outgoing edges to Targets.
   */
  void AddEdge(const clang::NamedDecl *Target, const clang::NamedDecl *Source);

  /**
   * @brief Add a usage constraint for a declaration.
   * @param Decl Declaration to constrain.
   * @param Candidate Candidate type.
   * @param BaseExpr Optional source expression.
   * @param Ctx AST context for type manipulation.
   */
  void AddConstraint(const clang::NamedDecl *Decl, clang::QualType Candidate,
                     const clang::Expr *BaseExpr, clang::ASTContext *Ctx);

  /**
   * @brief Add a loop comparison constraint for an induction variable.
   * @param InductionVar Loop induction variable.
   * @param BoundExpr Loop bound expression.
   * @param Ctx AST context.
   */
  void AddLoopComparisonConstraint(const clang::NamedDecl *InductionVar,
                                   const clang::Expr *BoundExpr,
                                   clang::ASTContext *Ctx);

  /**
   * @brief Add a numeric range constraint.
   * @param Decl Declaration to constrain.
   * @param Range Observed range.
   */
  void AddRangeConstraint(const clang::NamedDecl *Decl, ValueRange Range);

  /**
   * @brief Add a symbolic relationship between declarations.
   * @param Result Result declaration.
   * @param Op Operation kind.
   * @param LHS Left operand declaration.
   * @param RHS Right operand declaration.
   */
  void AddSymbolicConstraint(const clang::NamedDecl *Result, OpKind Op,
                             const clang::NamedDecl *LHS,
                             const clang::NamedDecl *RHS);

  /**
   * @brief Solve all constraints and return updated states.
   * @param Ctx AST context for type resolution.
   * @return Map of declarations to solved node state.
   */
  std::map<const clang::NamedDecl *, NodeState> Solve(clang::ASTContext *Ctx);

  /**
   * @brief Get the resolved type for a declaration.
   * @param Decl Declaration to query.
   * @return Resolved type or null type if unknown.
   */
  clang::QualType GetResolvedType(const clang::NamedDecl *Decl) const;

private:
  std::map<const clang::NamedDecl *, NodeState> Nodes;
  std::map<const clang::NamedDecl *, std::vector<const clang::NamedDecl *>>
      Adjacency;
  std::vector<SymbolicConstraint> SymbolicConstraints;

  int TarjanIndexCounter;
  std::stack<const clang::NamedDecl *> TarjanStack;
  std::map<const clang::NamedDecl *, TarjanData> TarjanState;

  void StrongConnect(const clang::NamedDecl *V, clang::ASTContext *Ctx);
  void ProcessSCC(const std::vector<const clang::NamedDecl *> &SCC,
                  clang::ASTContext *Ctx);

  clang::QualType GetWider(clang::QualType A, clang::QualType B,
                           clang::ASTContext *Ctx);
  clang::QualType GetOptimalTypeForRange(const ValueRange &R,
                                         clang::QualType Original,
                                         clang::ASTContext *Ctx);
  clang::QualType HelperGetType(const clang::Expr *E, clang::ASTContext *Ctx);
};

} // namespace type_correct

#endif // TYPE_CORRECT_TYPESOLVER_H
