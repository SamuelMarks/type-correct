/**
 * @file TypeCorrect.h
 * @brief Header for the TypeCorrect library.
 *
 * Configures the AST Matcher engine, rewriting logic, and constraint solver.
 * Includes definitions for Audit and Reporting data structures.
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
/**
 * @enum Phase
 * @brief Defines the execution mode for Cross-Translation Unit analysis.
 */
enum class Phase { Standalone, Map, Apply, Iterative };

/**
 * @struct ChangeRecord
 * @brief Represents a single proposed or applied source code modification.
 */
struct ChangeRecord {
  std::string FilePath; ///< The file path of the change.
  unsigned Line;        ///< The line number of the change.
  std::string Symbol;   ///< The name of the symbol being modified.
  std::string OldType;  ///< The original type string.
  std::string NewType;  ///< The new type string.
};
} // namespace type_correct

/**
 * @struct FormatUsage
 * @brief Tracks usage of printf-style format specifiers.
 */
struct FormatUsage {
  clang::SourceLocation SpecifierLoc; ///< Location of the %d type specifier.
  unsigned Length;                    ///< Length of the specifier string.
};

/**
 * @class TypeCorrectMatcher
 * @brief The core AST Matching callback and rewriting engine.
 */
class TYPE_CORRECT_EXPORT TypeCorrectMatcher
    : public clang::ast_matchers::MatchFinder::MatchCallback {

public:
  /**
   * @brief Constructs the Matcher.
   *
   * @param Rewriter Reference to the Clang Rewriter.
   * @param UseDecltype Toggle dependent on CLI argument.
   * @param ExpandAuto Toggle dependent on CLI argument.
   * @param ProjectRoot Path to project root for boundary scanning.
   * @param ExcludePattern Regex for exclusion.
   * @param InPlace Toggle in-place file modification.
   * @param EnableAbiBreakingChanges Toggle struct refactoring.
   * @param AuditMode If true, changes are calculated but not applied to the
   * Rewriter buffer.
   * @param CurrentPhase CTU Phase.
   * @param FactsOutputDir Output directory for CTU facts.
   * @param ReportFile Optional path to write a JSON report of changes.
   */
  explicit TypeCorrectMatcher(
      clang::Rewriter &Rewriter, bool UseDecltype = false,
      bool ExpandAuto = false, std::string ProjectRoot = "",
      std::string ExcludePattern = "", bool InPlace = false,
      bool EnableAbiBreakingChanges = false, bool AuditMode = false,
      type_correct::Phase CurrentPhase = type_correct::Phase::Standalone,
      std::string FactsOutputDir = "", std::string ReportFile = "");

  /**
   * @brief Callback executed whenever an AST Matcher finds a node.
   * @param Result The match result containing AST nodes.
   */
  void
  run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

  /**
   * @brief Called when the entire Translation Unit has been parsed.
   * Triggers the Solver and the Rewriting/Reporting phase.
   * @param Ctx The AST Context.
   */
  void onEndOfTranslationUnit(clang::ASTContext &Ctx);

  /**
   * @brief API to retrieve the list of changes calculated during this run.
   * @return A vector of ChangeRecord objects.
   */
  std::vector<type_correct::ChangeRecord> GetChanges() const;

private:
  clang::Rewriter &Rewriter;
  bool UseDecltype;
  bool ExpandAuto;
  std::string ProjectRoot;
  std::string ExcludePattern;
  bool InPlace;
  bool AuditMode;
  StructAnalyzer StructEngine;

  type_correct::Phase CurrentPhase;
  std::string FactsOutputDir;
  std::string ReportFile;

  std::map<std::string, type_correct::ctu::SymbolFact> GlobalFacts;
  type_correct::TypeSolver Solver;

  // Change Tracking
  std::vector<type_correct::ChangeRecord> Changes;
  const clang::NamedDecl *CurrentProcessingDecl = nullptr;

  std::map<const clang::NamedDecl *,
           std::vector<const clang::ExplicitCastExpr *>>
      CastsToRemove;

  std::set<const clang::NamedDecl *> VariablesWithNegativeValues;
  std::map<const clang::NamedDecl *, std::vector<FormatUsage>> FormatUsageMap;
  std::map<const clang::VarDecl *, const clang::CXXMethodDecl *>
      StdFunctionToLambdaMap;
  std::set<const clang::Stmt *> RewrittenStmts;

  // -- Template Safety --
  /**
   * @brief Checks if specializing a template with NewType is compatible with
   * its definition.
   * @param TST The template specialization type.
   * @param OldType The original type argument.
   * @param NewType The new type argument.
   * @param Ctx AST Context.
   * @return true if safe to rewrite.
   */
  bool IsTemplateInstantiationSafe(const clang::TemplateSpecializationType *TST,
                                   clang::QualType OldType,
                                   clang::QualType NewType,
                                   clang::ASTContext *Ctx);

  /**
   * @brief Helper to find the active partial specialization.
   * @param Template The template decl.
   * @param Args The template arguments.
   * @return The specialization decl or nullptr.
   */
  const clang::ClassTemplatePartialSpecializationDecl *
  DetermineActiveSpecialization(
      clang::ClassTemplateDecl *Template,
      const llvm::ArrayRef<clang::TemplateArgument> &Args);

  // -- Recursion --
  /**
   * @brief Recursively traverses a TypeLoc to rewrite specific nested types.
   *
   * In Audit mode, this records the change without applying it.
   *
   * @param TL The TypeLoc to traverse.
   * @param TargetType The desired target type.
   * @param Ctx AST Context.
   * @param BaseExpr The expression triggering the type deduction (for
   * decltype).
   * @return true if any change was recorded/made.
   */
  bool RecursivelyRewriteType(clang::TypeLoc TL, clang::QualType TargetType,
                              clang::ASTContext *Ctx,
                              const clang::Expr *BaseExpr);

  /**
   * @brief High-level entry point to rewrite a variable/field type.
   * @param OldLoc The TypeLoc of the existing type.
   * @param NewType The calculated optimal type.
   * @param Ctx AST Context.
   * @param BoundVar Optional pointer to variable being rewritten.
   * @param BaseExpr Optional base expression.
   */
  void ResolveType(const clang::TypeLoc &OldLoc, const clang::QualType &NewType,
                   clang::ASTContext *Ctx,
                   const clang::VarDecl *BoundVar = nullptr,
                   const clang::Expr *BaseExpr = nullptr);

  /**
   * @brief Handles type rewriting within macro expansions (carefully).
   * @param Loc Source location.
   * @param NewType New type.
   * @param Ctx Context.
   * @param BaseExpr Expression.
   */
  void RewriteMacroType(clang::SourceLocation Loc,
                        const clang::QualType &NewType, clang::ASTContext *Ctx,
                        const clang::Expr *BaseExpr);

  // -- Helpers --
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
   * @brief Checks if a location is user-code (Modifiable) vs system-code.
   * @param DeclLoc The location to check.
   * @param SM Source Manager.
   * @return true if safe to modify.
   */
  bool IsModifiable(clang::SourceLocation DeclLoc,
                    const clang::SourceManager &SM);

  void AddConstraint(const clang::NamedDecl *Decl,
                     clang::QualType CandidateType, const clang::Expr *BaseExpr,
                     clang::ASTContext *Ctx);
  void AddUsageEdge(const clang::NamedDecl *Target,
                    const clang::NamedDecl *Source, clang::ASTContext *Ctx);

  clang::QualType GetTypeFromExpression(const clang::Expr *E,
                                        clang::ASTContext *Ctx);

  void EnsureGlobalFactsLoaded();
  void ApplyGlobalFactIfExists(const clang::NamedDecl *Decl,
                               clang::ASTContext *Ctx);

  clang::QualType ParseTypeString(const std::string &TypeName,
                                  clang::ASTContext *Ctx);

  clang::QualType SynthesizeContainerType(clang::QualType ContainerType,
                                          clang::QualType OldType,
                                          clang::QualType NewValueType,
                                          clang::ASTContext *Ctx);

  clang::QualType SynthesizeStdFunctionType(
      clang::QualType CurrentFuncType, const clang::CXXMethodDecl *LambdaCallOp,
      const std::map<const clang::NamedDecl *, type_correct::NodeState>
          &Solutions,
      clang::ASTContext *Ctx);
};

/**
 * @class TypeCorrectASTConsumer
 * @brief Wrapper for the Matcher required by Clang Tooling.
 */
class TYPE_CORRECT_EXPORT TypeCorrectASTConsumer : public clang::ASTConsumer {
public:
  /**
   * @brief Constructor passing all configuration down to the Matcher.
   *
   * @param Rewriter Clang Rewriter.
   * @param UseDecltype Config flag.
   * @param ExpandAuto Config flag.
   * @param ProjectRoot Project path.
   * @param ExcludePattern Exclusion regex.
   * @param InPlace Edit mode.
   * @param EnableAbiBreakingChanges Struct safety.
   * @param AuditMode Audit/Dry-run mode.
   * @param CurrentPhase CTU phase.
   * @param FactsOutputDir Output dir.
   * @param ReportFile Report output file.
   */
  explicit TypeCorrectASTConsumer(clang::Rewriter &Rewriter, bool UseDecltype,
                                  bool ExpandAuto, std::string ProjectRoot,
                                  std::string ExcludePattern, bool InPlace,
                                  bool EnableAbiBreakingChanges, bool AuditMode,
                                  type_correct::Phase CurrentPhase,
                                  std::string FactsOutputDir,
                                  std::string ReportFile);

  /**
   * @brief Runs the matcher and triggers the final solver logic.
   * @param Ctx AST Context.
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