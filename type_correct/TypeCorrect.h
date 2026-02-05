/**
 * @file TypeCorrect.h
 * @brief Header for the TypeCorrect library.
 *
 * This file declares the AST Consumer and Matcher logic for correcting type
 * inconsistencies. It encompasses strategies for:
 * - Variable Initialization rewriting.
 * - Return type rewriting.
 * - Loop iterator correction.
 * - Assignment Back-Tracking.
 * - Explicit Cast Injection (fallback).
 * - Widest Type Resolution.
 * - Argument Passing type correction (Standard Library compatibility).
 *
 * @author Samuel Marks
 * @license CC0
 */

#ifndef TYPE_CORRECT_H
#define TYPE_CORRECT_H

#include <clang/AST/ASTConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <map>

#include "type_correct_export.h"

/**
 * @struct WidestTypeState
 * @brief Encapsulates the target type information for a deferred rewrite.
 *
 * Stores the "widest" type seen so far for a specific declaration, ensuring
 * that multiple conflicting usages (e.g. passing to functions expecting int vs
 * size_t) resolve to the type capable of representing all values (widening).
 */
struct WidestTypeState {
  /** @brief The widest QualType encountered (e.g., size_t). */
  clang::QualType Type;
  /** @brief The expression instance associated with this type (nullable). */
  const clang::Expr *BaseExpr;
};

/**
 * @class TypeCorrectMatcher
 * @brief Callback class executed when AST Matchers find a target pattern.
 *
 * This class handles the extraction of AST nodes and implements the logic for
 * checking file modifiability, registering type updates, and applying fixes
 * (rewrites or casts) at the end of the translation unit.
 */
class TYPE_CORRECT_EXPORT TypeCorrectMatcher
    : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  /**
   * @brief Construct a new Type Correct Matcher object.
   *
   * @param[in] Rewriter Reference to the Clang Rewriter used to modify the
   * source code.
   * @param[in] UseDecltype Boolean flag indicating if 'decltype(expr)' syntax
   * should be preferred.
   * @param[in] ExpandAuto Boolean flag indicating if 'auto' should be
   * aggressively expanded even if initialized by a call.
   */
  explicit TypeCorrectMatcher(clang::Rewriter &Rewriter,
                              bool UseDecltype = false,
                              bool ExpandAuto = false);

  /**
   * @brief Callback executed whenever a registered AST matcher succeeds.
   *
   * Analyzes the match results. It handles:
   * - Variable Init
   * - Function Returns
   * - For Loops
   * - Assignments
   * - **Argument Passing**: Matches variables passed to functions via implicit
   * casts.
   *
   * @param[in] Result Container for the AST nodes matching the pattern.
   */
  void
  run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

  /**
   * @brief Callback executed manually at the end of the translation unit.
   *
   * Iterates through the WidestTypeMap and applies the final rewrites
   * to the source buffer using the determined widest types.
   *
   * @param[in] Ctx The ASTContext required to resolve types during processing.
   */
  void onEndOfTranslationUnit(clang::ASTContext &Ctx);

private:
  /** @brief Reference to the Clang Rewriter. */
  clang::Rewriter &Rewriter;

  /** @brief Optimization flag for decltype syntax. */
  bool UseDecltype;

  /** @brief Flag to force auto expansion. */
  bool ExpandAuto;

  /**
   * @brief Map storing deferred rewrites.
   * Key: The definition Decl (VarDecl or FunctionDecl).
   * Value: The widest type state decided so far.
   */
  std::map<const clang::NamedDecl *, WidestTypeState> WidestTypeMap;

  /**
   * @brief Helper to resolve and rewrite a type location.
   *
   * Applied at the end of the TU based on the WidestTypeMap.
   *
   * @param[in] OldLoc The location of the existing type in source.
   * @param[in] NewType The qualified type to write.
   * @param[in] Ctx The AST Context.
   * @param[in] BoundVar Optional pointer to the variable (for auto checks).
   * @param[in] BaseExpr Optional pointer to the expression (for decltype).
   */
  void ResolveType(const clang::TypeLoc &OldLoc, const clang::QualType &NewType,
                   clang::ASTContext *Ctx,
                   const clang::VarDecl *BoundVar = nullptr,
                   const clang::Expr *BaseExpr = nullptr);

  /**
   * @brief Helper to inject an explicit cast immediately.
   *
   * Used as a fallback when the definition cannot be changed (e.g. system
   * headers).
   *
   * @param[in] ExprToCast The expression to wrap.
   * @param[in] TargetType The target type.
   * @param[in] Ctx The AST Context.
   */
  void InjectCast(const clang::Expr *ExprToCast,
                  const clang::QualType &TargetType, clang::ASTContext *Ctx);

  /**
   * @brief Checks if the declaration is in the main file and not a macro.
   * @param[in] DeclLoc Source location.
   * @param[in] SM Source Manager.
   * @return true if safe to modify.
   */
  bool IsModifiable(clang::SourceLocation DeclLoc,
                    const clang::SourceManager &SM);

  /**
   * @brief Determines if the Candidate type is "wider" or preferred over the
   * Current type.
   *
   * Logic: size_t (unsigned) > int (signed). Wider bit-width > Narrower.
   *
   * @param[in] Current The existing type of the variable (or current map
   * entry).
   * @param[in] Candidate The new type encountered in usage.
   * @param[in] Ctx The AST Context.
   * @return true if Candidate is an upgrade over Current.
   */
  bool IsWiderType(clang::QualType Current, clang::QualType Candidate,
                   clang::ASTContext *Ctx);

  /**
   * @brief Updates the deferred map if the new usage suggests a wider type.
   *
   * @param[in] Decl The declaration definition.
   * @param[in] CandidateType The type returned by the current usage (or
   * expected by param).
   * @param[in] BaseExpr The expression triggering the usage.
   * @param[in] Ctx The AST Context.
   */
  void RegisterUpdate(const clang::NamedDecl *Decl,
                      clang::QualType CandidateType,
                      const clang::Expr *BaseExpr, clang::ASTContext *Ctx);
};

/**
 * @class TypeCorrectASTConsumer
 * @brief Consumes the AST and registers matchers.
 */
class TYPE_CORRECT_EXPORT TypeCorrectASTConsumer : public clang::ASTConsumer {
public:
  /**
   * @brief Construct a new Type Correct AST Consumer.
   * @param[in] Rewriter Reference to the Clang Rewriter.
   * @param[in] UseDecltype Flag for decltype syntax.
   * @param[in] ExpandAuto Flag for auto expansion.
   */
  explicit TypeCorrectASTConsumer(clang::Rewriter &Rewriter,
                                  bool UseDecltype = false,
                                  bool ExpandAuto = false);

  /**
   * @brief Process the Translation Unit.
   * @param[in] Ctx The AST Context.
   */
  void HandleTranslationUnit(clang::ASTContext &Ctx) override {
    Finder.matchAST(Ctx);
    // Pass the context explicitly to flush results
    Handler.onEndOfTranslationUnit(Ctx);
  }

private:
  clang::ast_matchers::MatchFinder Finder;
  TypeCorrectMatcher Handler;
};

#endif /* TYPE_CORRECT_H */