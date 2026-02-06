//==============================================================================
// FILE: 
//    TypeCorrectMain.cpp
// 
// DESCRIPTION: 
//    A standalone tool that runs the TypeCorrect plugin. 
//    Now includes Map-Reduce orchestration for CTU mode.
// 
// USAGE: 
//    Standalone: ct-type-correct a.cpp
//    Map: ct-type-correct -phase=map -facts-dir=/tmp/facts a.cpp
//    Reduce: ct-type-correct -phase=reduce -facts-dir=/tmp/facts
// 
// License: CC0
//==============================================================================
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Refactoring.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include "TypeCorrectMain.h" 
#include "CTU/FactManager.h"

//===----------------------------------------------------------------------===// 
// Command line options
//===----------------------------------------------------------------------===// 
static llvm::cl::OptionCategory TypeCorrectCategory("ct-type-correct options"); 

static llvm::cl::opt<std::string> ProjectRootOpt( 
    "project-root", 
    llvm::cl::desc("Absolute path to project root. " 
                   "Enables rewriting of headers residing within this directory."), 
    llvm::cl::cat(TypeCorrectCategory)); 

static llvm::cl::opt<std::string> ExcludeOpt( 
    "exclude", 
    llvm::cl::desc("Regex pattern to exclude files from rewriting."), 
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

static llvm::cl::opt<bool> EnableAbiBreakingChangesOpt(
    "enable-abi-breaking-changes",
    llvm::cl::desc("Allow rewriting of struct/class member fields."),
    llvm::cl::cat(TypeCorrectCategory));

// CTU Options
static llvm::cl::opt<std::string> PhaseOpt(
    "phase",
    llvm::cl::desc("Execution phase using Map-Reduce for CTU. Values: standalone (default), map, reduce, apply."),
    llvm::cl::cat(TypeCorrectCategory));

static llvm::cl::opt<std::string> FactsDirOpt(
    "facts-dir",
    llvm::cl::desc("Directory to store/read intermediate facts for CTU mode."),
    llvm::cl::cat(TypeCorrectCategory));

//===----------------------------------------------------------------------===// 
// Custom Frontend Action Factory
//===----------------------------------------------------------------------===// 
class TypeCorrectActionFactory : public clang::tooling::FrontendActionFactory { 
public: 
  type_correct::Phase ResolvedPhase;

  TypeCorrectActionFactory(type_correct::Phase P) : ResolvedPhase(P) {}

  std::unique_ptr<clang::FrontendAction> create() override { 
    return std::make_unique<TypeCorrectPluginAction>(
        ProjectRootOpt, ExcludeOpt, InPlaceOpt, EnableAbiBreakingChangesOpt, ResolvedPhase, FactsDirOpt); 
  } 
}; 

//===----------------------------------------------------------------------===// 
// Reduce Phase Logic
//===----------------------------------------------------------------------===// 
int RunReduce(const std::string &Dir) {
    if (Dir.empty()) {
        llvm::errs() << "Error: -facts-dir is required for reduce phase.\n";
        return 1;
    }

    std::error_code EC;
    std::vector<type_correct::ctu::SymbolFact> AllFacts;

    for (llvm::sys::fs::directory_iterator It(Dir, EC), End; It != End && !EC; It.increment(EC)) {
        std::string Path = It->path();
        if (llvm::sys::path::extension(Path) == ".facts" && llvm::sys::path::filename(Path) != "global.facts") {
            if (!type_correct::ctu::FactManager::ReadFacts(Path, AllFacts)) {
                llvm::errs() << "Warning: Failed to read facts from " << Path << "\n";
            }
        }
    }

    if (EC) {
        llvm::errs() << "Directory iteration error: " << EC.message() << "\n";
        return 1;
    }

    llvm::outs() << "Reducing " << AllFacts.size() << " facts...\n";
    auto Map = type_correct::ctu::FactManager::MergeFacts(AllFacts);

    std::string OutPath = Dir + "/global.facts";
    if (type_correct::ctu::FactManager::WriteFacts(OutPath, Map)) {
        llvm::outs() << "Global facts written to " << OutPath << "\n";
        return 0;
    }
    return 1;
}

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

  // Determine Phase
  type_correct::Phase P = type_correct::Phase::Standalone;
  if (PhaseOpt == "map") P = type_correct::Phase::Map;
  else if (PhaseOpt == "reduce") P = type_correct::Phase::Standalone; // Handled separately
  else if (PhaseOpt == "apply") P = type_correct::Phase::Apply;

  // Handle Reduce (No ClangTool involved)
  if (PhaseOpt == "reduce") {
      return RunReduce(FactsDirOpt);
  }
  
  // Handle ClangTool Phases
  clang::tooling::RefactoringTool Tool(eOptParser->getCompilations(), 
                                       eOptParser->getSourcePathList()); 
  
  TypeCorrectActionFactory Factory(P); 
  return Tool.runAndSave(&Factory); 
}