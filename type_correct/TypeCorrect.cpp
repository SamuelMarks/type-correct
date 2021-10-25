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

#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendPluginRegistry.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Refactoring.h>
#include <llvm/Support/CommandLine.h>

#include "TypeCorrect.h"

//===----------------------------------------------------------------------===//
// Command line options
//===----------------------------------------------------------------------===//
static llvm::cl::OptionCategory TypeCorrectCategory("ct-code-refactor options");

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

void TypeCorrectMatcher::run(const clang::ast_matchers::MatchFinder::MatchResult &Result) {
    /* Temporary content to test workflow, will repeal and replace */
    // The matched 'if' statement was bound to 'ifStmt'.
    if (const clang::IfStmt *IfS = Result.Nodes.getNodeAs<clang::IfStmt>("ifStmt")) {
        const clang::Stmt *Then = IfS->getThen();
        TypeCorrectRewriter.InsertText(Then->getBeginLoc(), "// the 'if' part\n", true, true);

        if (const clang::Stmt *Else = IfS->getElse()) {
            TypeCorrectRewriter.InsertText(Else->getBeginLoc(), "// the 'else' part\n", true,
                               true);
        }
    }
}
