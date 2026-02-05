//==============================================================================
// FILE:
//    TypeCorrectMain.h
//
// DESCRIPTION: Header for TypeCorrectMain.cpp (prototypes and exports for
// shared lib)
//
// License: CC0
//==============================================================================

#ifndef TYPECORRECT_TYPECORRECTMAIN_H
#define TYPECORRECT_TYPECORRECTMAIN_H

#include <string>
#include <vector>

#include <clang/Frontend/CompilerInstance.h>
#include <clang/Rewrite/Core/Rewriter.h>

#include "TypeCorrect.h"

#include "type_correct_export.h"

//===----------------------------------------------------------------------===//
// PluginASTAction
//===----------------------------------------------------------------------===//
class TYPE_CORRECT_EXPORT TypeCorrectPluginAction
    : public clang::PluginASTAction {
public:
  explicit TypeCorrectPluginAction(std::string ProjectRoot = "",
                                   std::string ExcludePattern = "",
                                   bool InPlace = false)
      : ProjectRoot(std::move(ProjectRoot)),
        ExcludePattern(std::move(ExcludePattern)), InPlace(InPlace) {}

  // Not used via CLI tool directly usually, but implemented for completeness
  bool ParseArgs(const clang::CompilerInstance &CI,
                 const std::vector<std::string> &args) override {
    // CLI arguments are typically handled by CommonOptionsParser in main
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
        /*ExpandAuto=*/false, ProjectRoot, ExcludePattern, InPlace);
  }

private:
  clang::Rewriter RewriterForTypeCorrect;
  std::string ProjectRoot;
  std::string ExcludePattern;
  bool InPlace;
};

#endif /* TYPECORRECT_TYPECORRECTMAIN_H */