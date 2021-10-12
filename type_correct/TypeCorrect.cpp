//==============================================================================
// FILE:
//    TypeCorrect.cpp
//
// DESCRIPTION:
//  Convert types to be correct (comparison type and return type take precedence)
//
//    * long f() { int a = 0; return a;} --> long f() { long a = 0; return a;}
//    * long g() { return 0L; }; int b = g(); --> long g() { return 0L; }; long b = g();
//    * for(int i=0; i<g(); i++) {} --> for(long i=0; i<g(); i++) {}
//
//  Resolves common UB and incorrect typings in C and C++ code.
//
// USAGE:
//    * clang -cc1 -load <BUILD_DIR>/lib/libTypeCorrect.dylib `\`
//        -plugin TypeCorrect test/MBA_add_int.cpp
//
// License: CC0
//==============================================================================

#include "TypeCorrect.h"

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace clang;

//===----------------------------------------------------------------------===//
// Command line options
//===----------------------------------------------------------------------===//
static cl::OptionCategory TypeCorrectCategory("ct-code-refactor options");

//===----------------------------------------------------------------------===//
// PluginASTAction
//===----------------------------------------------------------------------===//
class TypeCorrectPluginAction : public PluginASTAction {
public:
    explicit TypeCorrectPluginAction(){};
    // Not used
    bool ParseArgs(const CompilerInstance &CI,
                   const std::vector<std::string> &args) override {
        return true;
    }

    // Output the edit buffer for this translation unit
    // void EndSourceFileAction() override {
    //   RewriterForTypeCorrect
    //       .getEditBuffer(RewriterForTypeCorrect.getSourceMgr().getMainFileID())
    //       .write(llvm::outs());
    // }

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef file) override {
        RewriterForTypeCorrect.setSourceMgr(CI.getSourceManager(),
                                             CI.getLangOpts());
        return std::make_unique<TypeCorrectASTConsumer>(
                RewriterForTypeCorrect);
    }

private:
    Rewriter RewriterForTypeCorrect;
};

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//
int main(int Argc, const char **Argv) {
    Expected<tooling::CommonOptionsParser> eOptParser =
            clang::tooling::CommonOptionsParser::create(Argc, Argv,
                                                        TypeCorrectCategory);
    if (auto E = eOptParser.takeError()) {
        errs() << "Problem constructing CommonOptionsParser "
               << toString(std::move(E)) << '\n';
        return EXIT_FAILURE;
    }
    clang::tooling::RefactoringTool Tool(eOptParser->getCompilations(),
                                         eOptParser->getSourcePathList());

    return Tool.runAndSave(
            clang::tooling::newFrontendActionFactory<TypeCorrectPluginAction>()
                    .get());
}
