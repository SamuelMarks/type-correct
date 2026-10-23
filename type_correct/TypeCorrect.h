/**
 * @file TypeCorrect.h
 * @brief Public matcher and AST consumer for the type-correct tool.
 *
 * Declares the core matcher used to collect constraints and a consumer
 * wrapper that plugs into Clang tooling.
 */
#ifndef TYPE_CORRECT_H
#define TYPE_CORRECT_H

#include "CTU/FactManager.h"
#include "StructAnalyzer.h"
#include "TypeSolver.h"
#include "type_correct_export.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Rewrite/Core/Rewriter.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace type_correct {

/**
 * @enum Phase
 * @brief Execution phase for the tool pipeline.
 */
enum class Phase {
  Standalone, ///< Single-pass mode (no CTU facts).
  Map,        ///< Emit per-TU facts.
  Apply,      ///< Apply merged facts.
  Iterative   ///< Iterate Map/Apply until convergence.
};

/**
 * @struct ChangeRecord
 * @brief Records a single type change applied by the tool.
 */
struct ChangeRecord {
  std::string FilePath; ///< Source file path for the change.
  unsigned Line;        ///< 1-based line number.
  std::string Symbol;   ///< Symbol name or USR.
  std::string OldType;  ///< Original type spelling.
  std::string NewType;  ///< Updated type spelling.
};

} // namespace type_correct

/**
 * @struct AssignmentSite
 * @brief Captures an assignment edge for solver constraints.
 */
struct AssignmentSite {
  const clang::NamedDecl *Target; ///< Destination symbol being assigned.
  const clang::Expr *SourceExpr;  ///< Source expression of the assignment.
};

/**
 * @class TypeCorrectMatcher
 * @brief AST matcher callback that collects type constraints and applies rewrites.
 */
class TYPE_CORRECT_EXPORT TypeCorrectMatcher
    : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  /**
   * @brief Construct a matcher with tool configuration.
   *
   * @param Rewriter Output rewriter for edits.
   * @param UseDecltype Prefer decltype() when rewriting.
   * @param ExpandAuto Expand auto to concrete types.
   * @param ProjectRoot Root path used for boundary checks.
   * @param ExcludePattern Regex for excluded paths.
   * @param InPlace Rewrite in place instead of emitting patches.
   * @param EnableAbiBreakingChanges Allow layout changes in structs.
   * @param AuditMode Emit audit report instead of rewriting.
   * @param CurrentPhase Processing phase in CTU pipeline.
   * @param FactsOutputDir Output directory for facts.
   * @param ReportFile Audit report file path.
   */
  explicit TypeCorrectMatcher(
      clang::Rewriter &Rewriter, bool UseDecltype = false,
      bool ExpandAuto = false, std::string ProjectRoot = "",
      std::string ExcludePattern = "", bool InPlace = false,
      bool EnableAbiBreakingChanges = false, bool AuditMode = false,
      type_correct::Phase CurrentPhase = type_correct::Phase::Standalone,
      std::string FactsOutputDir = "", std::string ReportFile = "") noexcept;

  /**
   * @brief Destructor.
   */
  virtual ~TypeCorrectMatcher() noexcept;

  /**
   * @brief Handle a match result from the AST matcher.
   * @param Result Matched AST nodes.
   */
  void run(const clang::ast_matchers::MatchFinder::MatchResult &Result)
      noexcept override;

  /**
   * @brief Finalize processing after the translation unit is parsed.
   * @param Ctx AST context.
   */
  void onEndOfTranslationUnit(clang::ASTContext &Ctx) noexcept;

  /**
   * @brief Retrieve the list of recorded changes.
   * @return Vector of changes.
   */
  std::vector<type_correct::ChangeRecord> GetChanges() const noexcept;

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
  std::vector<type_correct::ChangeRecord> Changes;

  const clang::NamedDecl *CurrentProcessingDecl = nullptr;
  std::vector<const clang::ExplicitCastExpr *> ExplicitCasts;
  std::vector<AssignmentSite> Assignments;

  void EnsureGlobalFactsLoaded() noexcept;
  void ApplyGlobalFactIfExists(const clang::NamedDecl *,
                               clang::ASTContext *) noexcept;
  void RegisterTypedefDependency(const clang::NamedDecl *, clang::QualType,
                                 clang::ASTContext *) noexcept;

  void AddConstraint(const clang::NamedDecl *, clang::QualType,
                     const clang::Expr *, clang::ASTContext *) noexcept;

  void AddUsageEdge(const clang::NamedDecl *, const clang::NamedDecl *,
                    clang::ASTContext *) noexcept;

  void ProcessRedundantCasts(clang::ASTContext &Ctx) noexcept;
  void ProcessNarrowingSafety(clang::ASTContext &Ctx) noexcept;
};

/**
 * @class TypeCorrectASTConsumer
 * @brief Clang ASTConsumer that dispatches AST matching and end-of-TU rewriting.
 */
class TYPE_CORRECT_EXPORT TypeCorrectASTConsumer : public clang::ASTConsumer {
public:
  /**
   * @brief Construct the AST consumer and its matcher.
   *
   * @param Rewriter Output rewriter for edits.
   * @param UseDecltype Prefer decltype() when rewriting.
   * @param ExpandAuto Expand auto to concrete types.
   * @param ProjectRoot Root path used for boundary checks.
   * @param ExcludePattern Regex for excluded paths.
   * @param InPlace Rewrite in place instead of emitting patches.
   * @param EnableAbiBreakingChanges Allow layout changes in structs.
   * @param AuditMode Emit audit report instead of rewriting.
   * @param CurrentPhase Processing phase in CTU pipeline.
   * @param FactsOutputDir Output directory for facts.
   * @param ReportFile Audit report file path.
   */
  explicit TypeCorrectASTConsumer(
      clang::Rewriter &Rewriter, bool UseDecltype, bool ExpandAuto,
      const std::string &ProjectRoot, const std::string &ExcludePattern,
      bool InPlace, bool EnableAbiBreakingChanges, bool AuditMode,
      type_correct::Phase CurrentPhase, const std::string &FactsOutputDir,
      const std::string &ReportFile) noexcept;

  /**
   * @brief Entry point for AST traversal on a translation unit.
   * @param Ctx AST context.
   */
  void HandleTranslationUnit(clang::ASTContext &Ctx) noexcept override;

private:
  clang::ast_matchers::MatchFinder Finder;
  TypeCorrectMatcher Handler;
};

#endif // TYPE_CORRECT_H
