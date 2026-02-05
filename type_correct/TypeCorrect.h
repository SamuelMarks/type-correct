/**
 * @file TypeCorrect.h
 * @brief Header for the TypeCorrect library.
 *
 * This file declares the AST Consumer and Matcher logic for correcting type
 * inconsistencies. It encompasses strategies for:
 * - Variable Initialization rewriting.
 * - Return type rewriting.
 * - Assignment Back-Tracking.
 * - Explicit Cast Injection (fallback).
 * - **Redundant Cast Removal**: Detection and cleaning of explicit casts.
 * - Widest Type Resolution.
 * - Argument Passing type correction (Standard Library compatibility).
 * - **Pointer Arithmetic Support**: Promotion of pointer subtraction results to
 * ptrdiff_t.
 * - **Sizeof/Alignof Support**: Detection of sizeof/alignof usages.
 * - **Arithmetic Propagation**: Recursive analysis of binary operators (+, -,
 * *) to propagate wide types (like size_t) from operands to results.
 * - **Ternary Operator Support**: Evaluation of conditional operators.
 * - **General Relational Comparisons**: Extends source-of-truth logic to
 *   control flow conditions.
 * - **Negative Literal Safeguard**: Detection of negative assignments.
 * - **Format String Updater**: Rewriting of printf AND scanf specifiers.
 * - **Multi-Variable Splitting**: Splitting of composite declarations.
 * - **Exclude Patterns**: Preventing rewrites in specific paths.
 * - **In-Place Editing**: Overwriting source files directly.
 * - **Namespace Qualification**: Ensuring types are fully qualified.
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
#include <set>
#include <string>
#include <vector>

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
  /** @brief The widest QualType encountered (e.g., size_t, ptrdiff_t). */
  clang::QualType Type;
  /** @brief The expression instance associated with this type (nullable). */
  const clang::Expr *BaseExpr;
};

/**
 * @struct FormatUsage
 * @brief Identifies a format specifier in a string literal linked to a
 * variable.
 *
 * Used to defer the rewriting of printf/scanf-style strings (e.g. "%d" ->
 * "%zu") until the final type of the variable is decided.
 */
struct FormatUsage {
  /** @brief Start location of the specifier/modifier to replace. */
  clang::SourceLocation SpecifierLoc;
  /** @brief Length of the text to replace. */
  unsigned Length;
};

/**
 * @class TypeCorrectMatcher
 * @brief Callback class executed when AST Matchers find a target pattern.
 *
 * This class handles the extraction of AST nodes and implements the logic for
 * checking file modifiability, registering type updates, and applying fixes
 * (rewrites, removals, or casts) at the end of the translation unit.
 */
class TYPE_CORRECT_EXPORT TypeCorrectMatcher
    : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  /**
   * @brief Construct a new Type Correct Matcher object.
   *
   * @param Rewriter Reference to the Clang Rewriter.
   * @param UseDecltype Flag for decltype syntax preference.
   * @param ExpandAuto Flag for auto expansion preference.
   * @param ProjectRoot Optional absolute path to project root.
   * @param ExcludePattern Optional regex string for exclusion.
   * @param InPlace Boolean flag for in-place editing.
   */
  explicit TypeCorrectMatcher(clang::Rewriter &Rewriter,
                              bool UseDecltype = false, bool ExpandAuto = false,
                              std::string ProjectRoot = "",
                              std::string ExcludePattern = "",
                              bool InPlace = false);

  /**
   * @brief Callback executed whenever a registered AST matcher succeeds.
   *
   * Analyzes the match results. It handles:
   * - Variable Init (Calls, Sizeof, Arithmetic, Casts)
   * - Function Returns
   * - Relational Comparisons
   * - Assignments
   * - Argument Passing
   * - Pointer Arithmetic
   * - Negative Literals
   * - Format Strings (Printf/Scanf)
   * - Cast Tracking
   *
   * @param Result Container for the AST nodes matching the pattern.
   */
  void
  run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

  /**
   * @brief Callback executed manually at the end of the translation unit.
   *
   * Iterates through the WidestTypeMap and applies the final rewrites
   * to the source buffer, including format string updates and removal of
   * obsolete casts.
   *
   * @param Ctx The ASTContext required to resolve types during processing.
   */
  void onEndOfTranslationUnit(clang::ASTContext &Ctx);

private:
  /** @brief Reference to the Clang Rewriter. */
  clang::Rewriter &Rewriter;

  /** @brief Optimization flag for decltype syntax. */
  bool UseDecltype;

  /** @brief Flag to force auto expansion. */
  bool ExpandAuto;

  /** @brief Path to the project root used to filtering rewriting targets. */
  std::string ProjectRoot;

  /** @brief Regex pattern for excluded file paths. */
  std::string ExcludePattern;

  /** @brief Flag for in-place editing. */
  bool InPlace;

  /**
   * @brief Map storing deferred rewrites.
   * Key: The definition Decl (VarDecl or FunctionDecl).
   * Value: The widest type state decided so far.
   */
  std::map<const clang::NamedDecl *, WidestTypeState> WidestTypeMap;

  /**
   * @brief Map storing explicit casts identified for potential removal.
   * Key: The variable declaration associated with the cast.
   * Value: List of ExplicitCastExpr nodes found in init/assign.
   */
  std::map<const clang::NamedDecl *,
           std::vector<const clang::ExplicitCastExpr *>>
      CastsToRemove;

  /**
   * @brief Set storing variables known to hold negative values.
   *
   * Used to safeguard against promoting signed variables to unsigned types.
   */
  std::set<const clang::NamedDecl *> VariablesWithNegativeValues;

  /**
   * @brief Map storing locations of format specifiers linked to variables.
   * Key: The variable declaration (VarDecl).
   * Value: List of locations in format strings that print/scan this variable.
   */
  std::map<const clang::NamedDecl *, std::vector<FormatUsage>> FormatUsageMap;

  /**
   * @brief Set tracking multi-variable DeclStmts that have already been split
   * and rewritten.
   */
  std::set<const clang::Stmt *> RewrittenStmts;

  /**
   * @brief Helper to resolve and rewrite a type location.
   *
   * Applied at the end of the TU based on the WidestTypeMap.
   *
   * @param OldLoc The location of the existing type in source.
   * @param NewType The qualified type to write.
   * @param Ctx The AST Context.
   * @param BoundVar Optional pointer to the variable (for auto checks).
   * @param BaseExpr Optional pointer to the expression (for decltype).
   */
  void ResolveType(const clang::TypeLoc &OldLoc, const clang::QualType &NewType,
                   clang::ASTContext *Ctx,
                   const clang::VarDecl *BoundVar = nullptr,
                   const clang::Expr *BaseExpr = nullptr);

  /**
   * @brief Handles the splitting of multi-variable declaration statements.
   *
   * Splits `int a=0, b=0;` into `size_t a=0; int b=0;` if types diverge.
   *
   * @param DS The declaration statement node.
   * @param Ctx The AST Context.
   */
  void HandleMultiDecl(const clang::DeclStmt *DS, clang::ASTContext *Ctx);

  /**
   * @brief Helper to inject an explicit cast immediately.
   *
   * Used as a fallback when the definition cannot be changed.
   *
   * @param ExprToCast The expression to wrap.
   * @param TargetType The target type.
   * @param Ctx The AST Context.
   */
  void InjectCast(const clang::Expr *ExprToCast,
                  const clang::QualType &TargetType, clang::ASTContext *Ctx);

  /**
   * @brief Helper to remove an explicit cast from the source code.
   *
   * Replaces `(int)foo()` with `foo()`.
   *
   * @param Cast The explicit cast AST node.
   * @param Ctx The AST Context.
   */
  void RemoveExplicitCast(const clang::ExplicitCastExpr *Cast,
                          clang::ASTContext *Ctx);

  /**
   * @brief Parses a printf-style format string to find specifiers for
   * arguments.
   *
   * @param FormatStr The string literal AST node.
   * @param Args The list of arguments passed to the function.
   * @param Ctx The AST context.
   */
  void ScanPrintfArgs(const clang::StringLiteral *FormatStr,
                      const std::vector<const clang::Expr *> &Args,
                      clang::ASTContext *Ctx);

  /**
   * @brief Parses a scanf-style format string to find specifiers for arguments.
   *
   * Handles assignment suppression (*) and pointer arguments (AddressOf).
   *
   * @param FormatStr The string literal AST node.
   * @param Args The list of arguments passed to the function.
   * @param Ctx The AST context.
   */
  void ScanScanfArgs(const clang::StringLiteral *FormatStr,
                     const std::vector<const clang::Expr *> &Args,
                     clang::ASTContext *Ctx);

  /**
   * @brief Updates format specifiers if the variable type changed.
   *
   * @param Decl The variable declaration.
   * @param NewType The new type of the variable.
   * @param Ctx The AST context.
   */
  void UpdateFormatSpecifiers(const clang::NamedDecl *Decl,
                              const clang::QualType &NewType,
                              clang::ASTContext *Ctx);

  /**
   * @brief Checks if the declaration is modifiable (Main file or Project path).
   *
   * @param DeclLoc Source location.
   * @param SM Source Manager.
   * @return true if safe to modify.
   */
  bool IsModifiable(clang::SourceLocation DeclLoc,
                    const clang::SourceManager &SM);

  /**
   * @brief Determines if the Candidate type is "wider" or preferred.
   *
   * @param Current The existing type.
   * @param Candidate The new type.
   * @param Ctx The AST Context.
   * @return true if Candidate is an upgrade.
   */
  bool IsWiderType(clang::QualType Current, clang::QualType Candidate,
                   clang::ASTContext *Ctx);

  /**
   * @brief Updates the deferred map if the new usage suggests a wider type.
   *
   * @param Decl The declaration definition.
   * @param CandidateType The type encountered.
   * @param BaseExpr The expression triggering the usage.
   * @param Ctx The AST Context.
   */
  void RegisterUpdate(const clang::NamedDecl *Decl,
                      clang::QualType CandidateType,
                      const clang::Expr *BaseExpr, clang::ASTContext *Ctx);

  /**
   * @brief Helper to deduce the resulting type from a type-generating
   * expression.
   *
   * Handles CallExpr, Sizeof/Alignof, Ternary, Casts, Variables (DeclRef),
   * and Arithmetic (BinaryOperator).
   *
   * @param E The expression to analyze.
   * @param Ctx The AST Context.
   * @return QualType The semantic type of the expression.
   */
  clang::QualType GetTypeFromExpression(const clang::Expr *E,
                                        clang::ASTContext *Ctx);
};

/**
 * @class TypeCorrectASTConsumer
 * @brief Consumes the AST and registers matchers.
 */
class TYPE_CORRECT_EXPORT TypeCorrectASTConsumer : public clang::ASTConsumer {
public:
  /**
   * @brief Construct a new Type Correct AST Consumer.
   * @param Rewriter Reference to the Clang Rewriter.
   * @param UseDecltype Flag for decltype syntax.
   * @param ExpandAuto Flag for auto expansion.
   * @param ProjectRoot Optional root directory for rewriting.
   * @param ExcludePattern Optional exclude regex for paths.
   * @param InPlace Flag for overwriting files.
   */
  explicit TypeCorrectASTConsumer(clang::Rewriter &Rewriter,
                                  bool UseDecltype = false,
                                  bool ExpandAuto = false,
                                  std::string ProjectRoot = "",
                                  std::string ExcludePattern = "",
                                  bool InPlace = false);

  /**
   * @brief Process the Translation Unit.
   * @param Ctx The AST Context.
   */
  void HandleTranslationUnit(clang::ASTContext &Ctx) override {
    Finder.matchAST(Ctx);
    Handler.onEndOfTranslationUnit(Ctx);
  }

private:
  clang::ast_matchers::MatchFinder Finder;
  TypeCorrectMatcher Handler;
};

#endif /* TYPE_CORRECT_H */