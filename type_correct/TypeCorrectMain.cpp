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
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Refactoring.h>
#include <llvm/Support/CommandLine.h>

#include "TypeCorrectMain.h"

//===----------------------------------------------------------------------===//
// Command line options
//===----------------------------------------------------------------------===//
static llvm::cl::OptionCategory TypeCorrectCategory("ct-type-correct options");

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//
int main(int argc, const char **argv) {
    llvm::Expected<clang::tooling::CommonOptionsParser> eOptParser =
            clang::tooling::CommonOptionsParser::create(argc, argv,
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
