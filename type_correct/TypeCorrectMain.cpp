//==============================================================================
// FILE:
//    TypeCorrectMain.cpp
//
// DESCRIPTION:
//    A standalone tool that runs the TypeCorrect plugin. See
//    TypeCorrect.cpp for a complete description.
//
// USAGE:
//    * ct-type-correct a.cpp
//
//    (or any of b.cxx c.cc d.c d.h a.hpp b.hxx)
//
//
// License: CC0
//==============================================================================
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendPluginRegistry.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Refactoring.h>
#include <llvm/Support/CommandLine.h>

#include "TypeCorrect.h"

//===----------------------------------------------------------------------===//
// Command line options
//===----------------------------------------------------------------------===//
static llvm::cl::OptionCategory TypeCorrectCategory("ct-type-correct options");

//===----------------------------------------------------------------------===//
// PluginASTAction
//===----------------------------------------------------------------------===//
class TypeCorrectPluginAction : public clang::PluginASTAction {
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

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI,
                                                          llvm::StringRef file) override {
        RewriterForTypeCorrect.setSourceMgr(CI.getSourceManager(),
                                             CI.getLangOpts());
        return std::make_unique<TypeCorrectASTConsumer>(
                RewriterForTypeCorrect);
    }

private:
    clang::Rewriter RewriterForTypeCorrect;
};

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//
int main(int Argc, const char **Argv) {
    llvm::Expected<clang::tooling::CommonOptionsParser> eOptParser =
            clang::tooling::CommonOptionsParser::create(Argc, Argv,
                                                        TypeCorrectCategory);
    if (auto E = eOptParser.takeError()) {
        llvm::errs() << "Problem constructing CommonOptionsParser "
                     << toString(std::move(E)) << '\n';
        return EXIT_FAILURE;
    }
    clang::tooling::RefactoringTool Tool(eOptParser->getCompilations(),
                                         eOptParser->getSourcePathList());

    return Tool.runAndSave(
            clang::tooling::newFrontendActionFactory<TypeCorrectPluginAction>()
                    .get());
}
