//==============================================================================
// FILE: TypeCorrectMain.h
// LICENSE: CC0
//==============================================================================
/**
 * @file TypeCorrectMain.h
 * @brief Clang plugin action wrapper for TypeCorrect.
 */

#ifndef TYPECORRECT_TYPECORRECTMAIN_H
#define TYPECORRECT_TYPECORRECTMAIN_H

#include <string>
#include <vector>

#include "TypeCorrect.h"
#include "type_correct_export.h"
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Rewrite/Core/Rewriter.h>

//===----------------------------------------------------------------------===//
// PluginASTAction
//===----------------------------------------------------------------------===//
/**
 * @class TypeCorrectPluginAction
 * @brief Clang plugin action that wires TypeCorrect into the frontend.
 */
class TYPE_CORRECT_EXPORT TypeCorrectPluginAction
    : public clang::PluginASTAction {
public:
  /**
   * @brief Construct a plugin action with tool configuration.
   *
   * @param ProjectRoot Root path used for boundary checks.
   * @param ExcludePattern Regex for excluded paths.
   * @param InPlace Rewrite in place instead of emitting patches.
   * @param EnableAbiBreakingChanges Allow layout changes in structs.
   * @param AuditMode Emit audit report instead of rewriting.
   * @param CurrentPhase Processing phase in CTU pipeline.
   * @param FactsOutputDir Output directory for facts.
   * @param ReportFile Audit report file path.
   */
  explicit TypeCorrectPluginAction(
      std::string ProjectRoot = "", std::string ExcludePattern = "",
      bool InPlace = false, bool EnableAbiBreakingChanges = false,
      bool AuditMode = false,
      type_correct::Phase CurrentPhase = type_correct::Phase::Standalone,
      std::string FactsOutputDir = "", std::string ReportFile = "")
      : ProjectRoot(std::move(ProjectRoot)),
        ExcludePattern(std::move(ExcludePattern)), InPlace(InPlace),
        EnableAbiBreakingChanges(EnableAbiBreakingChanges),
        AuditMode(AuditMode), CurrentPhase(CurrentPhase),
        FactsOutputDir(std::move(FactsOutputDir)),
        ReportFile(std::move(ReportFile)) {}

  /**
   * @brief Parse plugin arguments.
   * @param CI Compiler instance.
   * @param args Argument vector.
   * @return true if arguments are accepted.
   */
  bool ParseArgs(const clang::CompilerInstance &CI,
                 const std::vector<std::string> &args) override {
    return true;
  }

  /**
   * @brief Create the AST consumer for the plugin.
   * @param CI Compiler instance.
   * @param file Input file name.
   * @return AST consumer instance.
   */
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI,
                    llvm::StringRef file) override {
    RewriterForTypeCorrect.setSourceMgr(CI.getSourceManager(),
                                        CI.getLangOpts());

    return std::make_unique<TypeCorrectASTConsumer>(
        RewriterForTypeCorrect,
        /*UseDecltype=*/false,
        /*ExpandAuto=*/false, ProjectRoot, ExcludePattern, InPlace,
        EnableAbiBreakingChanges, AuditMode, CurrentPhase, FactsOutputDir,
        ReportFile);
  }

private:
  clang::Rewriter RewriterForTypeCorrect;
  std::string ProjectRoot;
  std::string ExcludePattern;
  bool InPlace;
  bool EnableAbiBreakingChanges;
  bool AuditMode;
  type_correct::Phase CurrentPhase;
  std::string FactsOutputDir;
  std::string ReportFile;
};

#endif /* TYPECORRECT_TYPECORRECTMAIN_H */
