//==============================================================================
// FILE: TypeCorrectMain.h
// LICENSE: CC0
//==============================================================================

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
class TYPE_CORRECT_EXPORT TypeCorrectPluginAction
    : public clang::PluginASTAction {
public:
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

  bool ParseArgs(const clang::CompilerInstance &CI,
                 const std::vector<std::string> &args) override {
    return true;
  }

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