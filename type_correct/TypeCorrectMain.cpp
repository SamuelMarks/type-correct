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

static llvm::cl::opt<std::string> ProjectRootOpt( 
    "project-root", 
    llvm::cl::desc("Absolute path to project root. " 
                   "Enables rewriting of headers residing within this directory " 
                   "in addition to the main source file."), 
    llvm::cl::cat(TypeCorrectCategory)); 

static llvm::cl::opt<std::string> ExcludeOpt( 
    "exclude", 
    llvm::cl::desc("Regex pattern to exclude files from rewriting. " 
                   "Files matching this pattern will be ignored even if within parent root."), 
    llvm::cl::cat(TypeCorrectCategory)); 

static llvm::cl::opt<bool> InPlaceOpt(
    "in-place",
    llvm::cl::desc("Overwrite source files directly instead of outputting to stdout."),
    llvm::cl::cat(TypeCorrectCategory));

static llvm::cl::alias InPlaceAlias(
    "i", 
    llvm::cl::desc("Alias for -in-place"), 
    llvm::cl::aliasopt(InPlaceOpt),
    llvm::cl::cat(TypeCorrectCategory));

//===----------------------------------------------------------------------===// 
// Custom Frontend Action Factory
//===----------------------------------------------------------------------===// 
// Used to pass the command line option into the plugin action instance. 
class TypeCorrectActionFactory : public clang::tooling::FrontendActionFactory { 
public: 
  std::unique_ptr<clang::FrontendAction> create() override { 
    return std::make_unique<TypeCorrectPluginAction>(ProjectRootOpt, ExcludeOpt, InPlaceOpt); 
  } 
}; 

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
  
  // Use custom factory to inject CLI args into the Action
  TypeCorrectActionFactory Factory; 
  return Tool.runAndSave(&Factory); 
}