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
  explicit TypeCorrectPluginAction() = default;
  // Not used
  bool ParseArgs(const clang::CompilerInstance &CI,
                 const std::vector<std::string> &args) override {
    return true;
  }

  // Output the edit buffer for this translation unit
  // void EndSourceFileAction() override {
  //   RewriterForTypeCorrect
  //       .getEditBuffer(RewriterForTypeCorrect.getSourceMgr().getMainFileID())
  //       .write(llvm::outs());
  // }

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI,
                    llvm::StringRef file) override {
    RewriterForTypeCorrect.setSourceMgr(CI.getSourceManager(),
                                        CI.getLangOpts());

    return std::make_unique<TypeCorrectASTConsumer>(RewriterForTypeCorrect);
  }

private:
  clang::Rewriter RewriterForTypeCorrect;
};

#endif /* TYPECORRECT_TYPECORRECTMAIN_H */
