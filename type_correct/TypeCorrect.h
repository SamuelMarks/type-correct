/**
 * @file TypeCorrect.h
 * @brief Header for the TypeCorrect library.
 *
 * Supports:
 * - Recursive Template Argument Rewriting.
 * - **Macro Body Analysis & Refactoring**.
 * - Standard Container Detection.
 * - Constraint-Based Solving.
 * - ABI Safety Toggles.
 * - Decltype Decoupling.
 *
 * @author SamuelMarks
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

#include "CTU/FactManager.h"
#include "StructAnalyzer.h"
#include "TypeSolver.h"
#include "type_correct_export.h"

namespace type_correct {
// Execution Phases for CTU
enum class Phase { Standalone, Map, Apply };
} // namespace type_correct

/**
 * @struct FormatUsage
 * @brief Identifies a format specifier in a string literal.
 */
struct FormatUsage {
  clang::SourceLocation SpecifierLoc;
  unsigned Length;
};

/**
 * @class TypeCorrectMatcher
 * @brief Callback class executed when AST Matchers find a target pattern.
 */
class TYPE_CORRECT_EXPORT TypeCorrectMatcher
    : public clang::ast_matchers::MatchFinder::MatchCallback {

public:
  /**
   * @brief Constructor.
   */
  explicit TypeCorrectMatcher(
      clang::Rewriter &Rewriter, bool UseDecltype = false,
      bool ExpandAuto = false, std::string ProjectRoot = "",
      std::string ExcludePattern = "", bool InPlace = false,
      bool EnableAbiBreakingChanges = false,
      type_correct::Phase CurrentPhase = type_correct::Phase::Standalone,
      std::string FactsOutputDir = "");

  /**
   * @brief Match callback.
   */
  void
  run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

  /**
   * @brief End of Translation Unit callback (Solver & Rewriter trigger).
   */
  void onEndOfTranslationUnit(clang::ASTContext &Ctx);

private:
  clang::Rewriter &Rewriter;
  bool UseDecltype;
  bool ExpandAuto;
  std::string ProjectRoot;
  std::string ExcludePattern;
  bool InPlace;
  StructAnalyzer StructEngine;

  // CTU State
  type_correct::Phase CurrentPhase;
  std::string FactsOutputDir;
  std::map<std::string, type_correct::ctu::SymbolFact> GlobalFacts;

  // Solver
  type_correct::TypeSolver Solver;

  // Cleanup & Formatting Maps
  std::map<const clang::NamedDecl *,
           std::vector<const clang::ExplicitCastExpr *>>
      CastsToRemove;
  std::set<const clang::NamedDecl *> VariablesWithNegativeValues;
  std::map<const clang::NamedDecl *, std::vector<FormatUsage>> FormatUsageMap;
  std::set<const clang::Stmt *> RewrittenStmts;

  //---------------------------------------------------------------------------
  // Core Logic Helpers
  //---------------------------------------------------------------------------

  /**
   * @brief Recursively traverses a TypeLoc to rewrite nested template args.
   */
  bool RecursivelyRewriteType(clang::TypeLoc TL, clang::QualType TargetType,
                              clang::ASTContext *Ctx,
                              const clang::Expr *BaseExpr);

  /**
   * @brief Wrapper for ResolveType that handles DeclStmts, Auto expansion,
   * AND Macro rewriting.
   */
  void ResolveType(const clang::TypeLoc &OldLoc, const clang::QualType &NewType,
                   clang::ASTContext *Ctx,
                   const clang::VarDecl *BoundVar = nullptr,
                   const clang::Expr *BaseExpr = nullptr);

  /**
   * @brief Logic to rewrite a type defined inside a Macro Body.
   *
   * Traces the expansion location back to the spelling location in the
   * macro definition, ensuring the file is modifiable, and applying the
   * replacement on the token stream.
   *
   * @param Loc The SourceLocation identifying the macro expansion.
   * @param NewType The target type to write.
   * @param Ctx AST Context.
   * @param BaseExpr Optional base expression for decltype logic.
   */
  void RewriteMacroType(clang::SourceLocation Loc,
                        const clang::QualType &NewType, clang::ASTContext *Ctx,
                        const clang::Expr *BaseExpr);

  void HandleMultiDecl(const clang::DeclStmt *DS, clang::ASTContext *Ctx);
  void InjectCast(const clang::Expr *ExprToCast,
                  const clang::QualType &TargetType, clang::ASTContext *Ctx);
  void RemoveExplicitCast(const clang::ExplicitCastExpr *Cast,
                          clang::ASTContext *Ctx);
  void ScanPrintfArgs(const clang::StringLiteral *FormatStr,
                      const std::vector<const clang::Expr *> &Args,
                      clang::ASTContext *Ctx);
  void ScanScanfArgs(const clang::StringLiteral *FormatStr,
                     const std::vector<const clang::Expr *> &Args,
                     clang::ASTContext *Ctx);
  void UpdateFormatSpecifiers(const clang::NamedDecl *Decl,
                              const clang::QualType &NewType,
                              clang::ASTContext *Ctx);

  /**
   * @brief Checks if a location is safe to modify.
   * Handles MacroIDs by checking their Spelling Location.
   */
  bool IsModifiable(clang::SourceLocation DeclLoc,
                    const clang::SourceManager &SM);

  /**
   * @brief Registers constraints into the solver.
   */
  void AddConstraint(const clang::NamedDecl *Decl,
                     clang::QualType CandidateType, const clang::Expr *BaseExpr,
                     clang::ASTContext *Ctx);
  void AddUsageEdge(const clang::NamedDecl *Target,
                    const clang::NamedDecl *Source, clang::ASTContext *Ctx);

  clang::QualType GetTypeFromExpression(const clang::Expr *E,
                                        clang::ASTContext *Ctx);
  void EnsureGlobalFactsLoaded();

  /**
   * @brief Helper to reconstruct a standard container type with a new argument.
   */
  clang::QualType SynthesizeContainerType(clang::QualType ContainerType,
                                          clang::QualType NewValueType,
                                          clang::ASTContext *Ctx);
};

/**
 * @class TypeCorrectASTConsumer
 * @brief Consumes the AST and registers matchers.
 */
class TYPE_CORRECT_EXPORT TypeCorrectASTConsumer : public clang::ASTConsumer {
public:
  explicit TypeCorrectASTConsumer(clang::Rewriter &Rewriter, bool UseDecltype,
                                  bool ExpandAuto, std::string ProjectRoot,
                                  std::string ExcludePattern, bool InPlace,
                                  bool EnableAbiBreakingChanges,
                                  type_correct::Phase CurrentPhase,
                                  std::string FactsOutputDir);

  void HandleTranslationUnit(clang::ASTContext &Ctx) override {
    Finder.matchAST(Ctx);
    Handler.onEndOfTranslationUnit(Ctx);
  }

private:
  clang::ast_matchers::MatchFinder Finder;
  TypeCorrectMatcher Handler;
};

#endif /* TYPE_CORRECT_H */