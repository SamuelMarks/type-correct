/**
 * @file TypeSolver.h
 * @brief Header for the Data-Flow Sensitive Solver with Pointer Semantics.
 *
 * The TypeSolver is the core engine for local resolution.
 * To support "Pointer Arithmetic and ptrdiff_t Semantics", it now tracks
 * whether a variable is used as an offset in pointer arithmetic.
 *
 * If a variable is flagged as a Pointer Offset, the solver treats `ptrdiff_t`
 * as the minimal width requirement, preventing 32-bit truncation on 64-bit
 * systems.
 *
 * @author SamuelMarks
 * @license CC0
 */

#ifndef TYPE_CORRECT_TYPESOLVER_H
#define TYPE_CORRECT_TYPESOLVER_H

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Type.h>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "type_correct_export.h"

namespace type_correct {

/**
 * @struct ValueRange
 * @brief Represents a closed numerical interval [Min, Max].
 */
struct ValueRange {
  int64_t Min;
  int64_t Max;
  bool HasMin;
  bool HasMax;

  ValueRange() : Min(0), Max(0), HasMin(false), HasMax(false) {}
  ValueRange(int64_t Val) : Min(Val), Max(Val), HasMin(true), HasMax(true) {}
  ValueRange(int64_t Least, int64_t Most)
      : Min(Least), Max(Most), HasMin(true), HasMax(true) {}

  void Union(const ValueRange &Other);
};

/**
 * @enum OpKind
 * @brief Operations supported by the symbolic solver.
 */
enum class OpKind { None, Add, Sub, Mul, Div };

/**
 * @struct SymbolicConstraint
 * @brief Result = LHS <Op> RHS.
 */
struct SymbolicConstraint {
  const clang::NamedDecl *Result;
  OpKind Op;
  const clang::NamedDecl *LHS;
  const clang::NamedDecl *RHS;

  SymbolicConstraint(const clang::NamedDecl *Res, OpKind O,
                     const clang::NamedDecl *L, const clang::NamedDecl *R)
      : Result(Res), Op(O), LHS(L), RHS(R) {}
};

/**
 * @struct NodeState
 * @brief The solution state for a specific declaration.
 */
struct NodeState {
  const clang::NamedDecl *Decl;
  clang::QualType OriginalType;
  clang::QualType ConstraintType;
  ValueRange ComputedRange;

  /**
   * @brief If true, this node is structurally locked (e.g. system header).
   */
  bool IsFixed;

  /**
   * @brief If true, this node has an external constraint applied via Global
   * Facts.
   */
  bool HasGlobalConstraint;

  /**
   * @brief If true, this node is used as an offset in pointer arithmetic.
   * Forces the type to be at least as wide as `ptrdiff_t`.
   */
  bool IsPtrOffset;

  const clang::Expr *BaseExpr;

  NodeState()
      : Decl(nullptr), IsFixed(false), HasGlobalConstraint(false),
        IsPtrOffset(false), BaseExpr(nullptr) {}

  NodeState(const clang::NamedDecl *D, clang::QualType T, bool Locked = false)
      : Decl(D), OriginalType(T), ConstraintType(T), IsFixed(Locked),
        HasGlobalConstraint(false), IsPtrOffset(false), BaseExpr(nullptr) {}
};

/**
 * @class TypeSolver
 * @brief Solves the system of type constraints including pointer semantics.
 */
class TYPE_CORRECT_EXPORT TypeSolver {
public:
  TypeSolver();

  /**
   * @brief Registers a declaration within the solver scope.
   * @param Decl The AST node.
   * @param CurrentType The type as written in source.
   * @param IsFixed If true, we cannot rewrite this node.
   */
  void AddNode(const clang::NamedDecl *Decl, clang::QualType CurrentType,
               bool IsFixed);

  /**
   * @brief Injects a constraint derived from Global Facts (CTU).
   * @param Decl The decl.
   * @param GlobalType The type asserted by global consensus.
   * @param Ctx AST Context.
   */
  void AddGlobalConstraint(const clang::NamedDecl *Decl,
                           clang::QualType GlobalType, clang::ASTContext *Ctx);

  /**
   * @brief Flags a declaration as being used for pointer arithmetic.
   *
   * This updates the internal state to ensure the variable resolves to
   * `ptrdiff_t` (or a compatible wide unsigned type like `size_t`)
   * regardless of the specific values in `computedRange`, because
   * pointer offsets on 64-bit systems must hold 64-bit values.
   *
   * @param Decl The variable declaration used as an offset.
   */
  void AddPointerOffsetUsage(const clang::NamedDecl *Decl);

  /**
   * @brief Links two nodes as structurally equivalent (Assignment/Data Flow).
   */
  void AddEdge(const clang::NamedDecl *User, const clang::NamedDecl *Used);

  /**
   * @brief Applies a local type constraint.
   */
  void AddConstraint(const clang::NamedDecl *Decl, clang::QualType Candidate,
                     const clang::Expr *BaseExpr, clang::ASTContext *Ctx);

  /**
   * @brief Applies a data-value range constraint.
   */
  void AddRangeConstraint(const clang::NamedDecl *Decl, ValueRange Range);

  /**
   * @brief Adds a symbolic operation to the graph.
   */
  void AddSymbolicConstraint(const clang::NamedDecl *Result, OpKind Op,
                             const clang::NamedDecl *LHS,
                             const clang::NamedDecl *RHS);

  /**
   * @brief Computes the optimal type for all registered nodes.
   *
   * Integrates pointer semantics by forcing `IsPtrOffset` nodes to widen
   * to `ptrdiff_t` before final resolution.
   *
   * @param Ctx Clang AST Context.
   * @return A map of updates (Decl -> New State).
   */
  std::map<const clang::NamedDecl *, NodeState> Solve(clang::ASTContext *Ctx);

private:
  std::map<const clang::NamedDecl *, NodeState> Nodes;
  std::map<const clang::NamedDecl *, std::vector<const clang::NamedDecl *>>
      Adjacency;
  std::vector<SymbolicConstraint> SymbolicConstraints;

  clang::QualType GetWider(clang::QualType A, clang::QualType B,
                           clang::ASTContext *Ctx);
  clang::QualType GetOptimalTypeForRange(const ValueRange &R,
                                         clang::QualType Original,
                                         clang::ASTContext *Ctx);
};

} // namespace type_correct

#endif // TYPE_CORRECT_TYPESOLVER_H