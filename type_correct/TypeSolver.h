/**
 * @file TypeSolver.h
 * @brief Header for the Constraint-Based Type Solver.
 *
 * The TypeSolver represents variables and declarations as nodes in a graph.
 * Assignments, comparisons, and interactions form edges. The solver identifies
 * connected components of variables that interact and determines the optimal
 * shared type constraint for the entire component.
 *
 * @author SamuelMarks
 * @license CC0
 */

#ifndef TYPE_CORRECT_TYPESOLVER_H
#define TYPE_CORRECT_TYPESOLVER_H

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Type.h>
#include <map>
#include <set>
#include <vector>

#include "type_correct_export.h"

namespace type_correct {

/**
 * @struct NodeState
 * @brief Represents the state of a single declaration (variable/function/field)
 * in the solver graph.
 */
struct NodeState {
  /** @brief The pointer to the Clang declaration. */
  const clang::NamedDecl *Decl;

  /** @brief The initial type found in the source code. */
  clang::QualType OriginalType;

  /** @brief The minimal inferred type requirement based on direct usage. */
  clang::QualType ConstraintType;

  /** @brief If true, this node cannot be modified (e.g. system header, locked
   * ABI). */
  bool IsFixed;

  /** @brief Optional base expression source for decltype logic. */
  const clang::Expr *BaseExpr;

  /** @brief Default constructor. */
  NodeState() : Decl(nullptr), IsFixed(false), BaseExpr(nullptr) {}

  /** @brief Initializing constructor. */
  NodeState(const clang::NamedDecl *D, clang::QualType T, bool Locked = false)
      : Decl(D), OriginalType(T), ConstraintType(T), IsFixed(Locked),
        BaseExpr(nullptr) {}
};

/**
 * @class TypeSolver
 * @brief Graph-based solver for type constraints.
 */
class TYPE_CORRECT_EXPORT TypeSolver {
public:
  /** @brief Constructor. */
  TypeSolver();

  /**
   * @brief Register a declaration in the solver.
   *
   * @param Decl The declaration (VarDecl, FieldDecl, etc.).
   * @param CurrentType The types as strictly written in source now.
   * @param IsFixed If true, this node acts as an immovable anchor.
   */
  void AddNode(const clang::NamedDecl *Decl, clang::QualType CurrentType,
               bool IsFixed);

  /**
   * @brief Register a directional flow or equivalence between two declarations.
   *
   * Represents assignment `A = B` (equality constraint).
   *
   * @param User The variable being assigned TO.
   * @param Used The variable being assigned FROM.
   */
  void AddEdge(const clang::NamedDecl *User, const clang::NamedDecl *Used);

  /**
   * @brief Apply a specific type constraint to a node.
   *
   * E.g., "Variable V is assigned the result of strlen()", implying V should be
   * size_t.
   *
   * @param Decl The declaration.
   * @param Candidate The type constraint (e.g. size_t).
   * @param BaseExpr Optional expression derived from.
   * @param Ctx AST Context for sizing.
   */
  void AddConstraint(const clang::NamedDecl *Decl, clang::QualType Candidate,
                     const clang::Expr *BaseExpr, clang::ASTContext *Ctx);

  /**
   * @brief Solves the graph constraints.
   *
   * 1. Finds connected components.
   * 2. Finds the widest "Constraint" within each component.
   * 3. Determines if the component can be upgraded.
   * 4. Returns a map of Decl -> NewType for all nodes that should change.
   *
   * @param Ctx The ASTContext for type comparison.
   * @return std::map<const clang::NamedDecl *, NodeState> The resolved
   * rewrites.
   */
  std::map<const clang::NamedDecl *, NodeState> Solve(clang::ASTContext *Ctx);

private:
  /** @brief Storage of nodes keyed by declaration pointer. */
  std::map<const clang::NamedDecl *, NodeState> Nodes;

  /** @brief Adjacency list: Decl -> List of connected Decls. */
  std::map<const clang::NamedDecl *, std::vector<const clang::NamedDecl *>>
      Adjacency;

  /**
   * @brief Helper to compare two types and return the "wider" one.
   * matches logic in TypeCorrectMatcher (int < long < size_t).
   */
  clang::QualType GetWider(clang::QualType A, clang::QualType B,
                           clang::ASTContext *Ctx);
};

} // namespace type_correct

#endif // TYPE_CORRECT_TYPESOLVER_H