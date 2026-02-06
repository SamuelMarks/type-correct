//==============================================================================
// FILE:
//    TypeCorrectMain.cpp
//
// DESCRIPTION:
//    A standalone tool that runs the TypeCorrect plugin.
//    Supports Iterative Fixed-Point Convergence for CTU and Audit Reporting.
//
//    This file defines the Command Line Interface (CLI) experience, including
//    help text, categorization, and the main entry point logic.
//
// USAGE:
//    See the generated help via `--help` or `USAGE.md`.
//
// License: CC0
//==============================================================================

#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Refactoring.h>
#include <iostream>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include "CTU/FactManager.h"
#include "TypeCorrectMain.h"

using namespace clang::tooling;

//===----------------------------------------------------------------------===//
// Option Categories
//===----------------------------------------------------------------------===//
// Grouping options makes the help output (--help) much more readable by
// separating tool-specific flags from generic LLVM/Clang flags.

static llvm::cl::OptionCategory CoreCategory(
    "Core Options", "Basic configuration for file processing and rewriting.");

static llvm::cl::OptionCategory CTUCategory(
    "CTU Options",
    "Cross-Translation Unit analysis for global type consistency.");

static llvm::cl::OptionCategory SafetyCategory(
    "Safety Options",
    "Configuration for ABI stability and boundary detection.");

static llvm::cl::OptionCategory ReportingCategory(
    "Reporting Options", "Audit, dry-run, and JSON reporting configuration.");

//===----------------------------------------------------------------------===//
// Core Options
//===----------------------------------------------------------------------===//

static llvm::cl::opt<std::string> ProjectRootOpt(
    "project-root",
    llvm::cl::desc("Absolute path to the project root directory.\n"
                   "Required for correct header rewriting and boundary detection."
                   "Files outside this root are treated as external/system."),
    llvm::cl::cat(CoreCategory));

static llvm::cl::opt<std::string> ExcludeOpt(
    "exclude",
    llvm::cl::desc("Regex pattern to exclude specific files from rewriting.\n"
                   "E.g., \"(test|mock|legacy)\"."),
    llvm::cl::cat(CoreCategory));

static llvm::cl::opt<bool> InPlaceOpt(
    "in-place",
    llvm::cl::desc(
        "Apply changes directly to source files on disk.\n"
        "If omitted, modified code is printed to stdout."),
    llvm::cl::cat(CoreCategory));

static llvm::cl::alias InPlaceAlias("i",
                                    llvm::cl::desc("Alias for --in-place"),
                                    llvm::cl::aliasopt(InPlaceOpt),
                                    llvm::cl::cat(CoreCategory));

//===----------------------------------------------------------------------===//
// Safety Options
//===----------------------------------------------------------------------===//

static llvm::cl::opt<bool> EnableAbiBreakingChangesOpt(
    "enable-abi-breaking-changes",
    llvm::cl::desc("Allow rewriting of struct/class member fields.\n"
                   "WARNING: This changes memory layout. Ensure all translation "
                   "units seeing the struct are recompiled."),
    llvm::cl::cat(SafetyCategory));

//===----------------------------------------------------------------------===//
// Reporting Options
//===----------------------------------------------------------------------===//

static llvm::cl::opt<bool> AuditOpt(
    "audit",
    llvm::cl::desc("Run in Audit Mode.\n"
                   "Calculates changes and output a Markdown table to stdout "
                   "without modifying any files."),
    llvm::cl::cat(ReportingCategory));

static llvm::cl::opt<std::string> ReportFileOpt(
    "report-file",
    llvm::cl::desc("Path to a JSON file to append change records to.\n"
                   "Format: line-delimited JSON objects. Useful for CI/CD."),
    llvm::cl::cat(ReportingCategory));

//===----------------------------------------------------------------------===//
// CTU (Cross-Translation Unit) Options
//===----------------------------------------------------------------------===//

static llvm::cl::opt<std::string> PhaseOpt(
    "phase",
    llvm::cl::desc("Execution phase for global type resolution.\n"
                   "  standalone : Local analysis only (default).\n"
                   "  iterative  : Run multiple passes to converge on global types.\n"
                   "  map        : Generate local facts (intermediate step).\n"
                   "  reduce     : Merge local facts into global facts.\n"
                   "  apply      : Apply global facts to code."),
    llvm::cl::cat(CTUCategory));

static llvm::cl::opt<std::string> FactsDirOpt(
    "facts-dir",
    llvm::cl::desc("Directory to store/read intermediate fact files.\n"
                   "Required for 'iterative', 'map', 'reduce', and 'apply' phases."),
    llvm::cl::cat(CTUCategory));

static llvm::cl::opt<int> MaxIterationsOpt(
    "max-iterations",
    llvm::cl::desc("Maximum number of iterations for fixed-point convergence "
                   "in 'iterative' mode."),
    llvm::cl::init(10), llvm::cl::cat(CTUCategory));

//===----------------------------------------------------------------------===//
// Extra Help & Examples
//===----------------------------------------------------------------------===//

static const char *MoreHelp = R"(
EXAMPLES:

  1. Basic Usage (Dry Run / stdout):
     $ ct-type-correct file.cpp

  2. Apply changes in-place:
     $ ct-type-correct -i file.cpp

  3. Audit a project (See what would change):
     $ ct-type-correct --audit --project-root=$(pwd) src/*.cpp

  4. Iterative Global Analysis (Recommended for large projects):
     $ mkdir facts
     $ ct-type-correct --phase=iterative --facts-dir=facts --project-root=$(pwd) src/*.cpp

AUTHOR:
  SamuelMarks
)";

static llvm::cl::extrahelp CommonHelp(CommonOptionsParser::HelpMessage);
static llvm::cl::extrahelp MoreHelpText(MoreHelp);

//===----------------------------------------------------------------------===//
// Custom Frontend Action Factory
//===----------------------------------------------------------------------===//
/**
 * @class TypeCorrectActionFactory
 * @brief Factory to create the PluginAction with parsed CLI options.
 */
class TypeCorrectActionFactory : public clang::tooling::FrontendActionFactory {
public:
  type_correct::Phase ResolvedPhase;

  /**
   * @brief Constructor
   * @param P The execution phase.
   */
  explicit TypeCorrectActionFactory(type_correct::Phase P) : ResolvedPhase(P) {}

  std::unique_ptr<clang::FrontendAction> create() override {
    // Pass strictly strictly parsed options to the action.
    // Note: ForceRewrite is not exposed in this simplified factory as per current SDK header.
    return std::make_unique<TypeCorrectPluginAction>(
        ProjectRootOpt, ExcludeOpt, InPlaceOpt, EnableAbiBreakingChangesOpt,
        AuditOpt, ResolvedPhase, FactsDirOpt, ReportFileOpt);
  }
};

//===----------------------------------------------------------------------===//
// Reduce Phase Logic (Returns true if global state CHANGED)
//===----------------------------------------------------------------------===//
/**
 * @brief Merges partial fact files into a global fact file.
 * @param Dir The directory containing .facts files.
 * @return true if the global state changed (requires re-running map).
 */
bool RunReduce(const std::string &Dir) {
  if (Dir.empty()) {
    llvm::errs() << "Error: --facts-dir is required for reduce phase.\n";
    return false;
  }

  std::error_code EC;
  std::vector<type_correct::ctu::SymbolFact> AllFacts;

  // Gather all partial .facts files (excluding the global one loopback)
  for (llvm::sys::fs::directory_iterator It(Dir, EC), End; It != End && !EC;
       It.increment(EC)) {
    std::string Path = It->path();
    if (llvm::sys::path::extension(Path) == ".facts" &&
        llvm::sys::path::filename(Path) != "global.facts") {
      if (!type_correct::ctu::FactManager::ReadFacts(Path, AllFacts)) {
        llvm::errs() << "Warning: Failed to read facts from " << Path << "\n";
      }
    }
  }

  if (EC) {
    llvm::errs() << "Directory iteration error: " << EC.message() << "\n";
    return false;
  }

  auto MergedMap = type_correct::ctu::FactManager::MergeFacts(AllFacts);
  std::string OutPath = Dir + "/global.facts";

  // Check for Convergence: Does the new map equal existing file on disk?
  if (type_correct::ctu::FactManager::IsConvergenceReached(OutPath, MergedMap)) {
    return false; // No change
  }

  // Write new state
  if (type_correct::ctu::FactManager::WriteFacts(OutPath, MergedMap)) {
    llvm::outs() << "Global facts updated at " << OutPath << "\n";
    return true; // Changed
  }

  return false; // Error or no change
}

//===----------------------------------------------------------------------===//
// Main driver code.
//===----------------------------------------------------------------------===//
int main(int argc, const char **argv) {
  // Use the specific Category to hide unrelated LLVM options from --help
  llvm::Expected<clang::tooling::CommonOptionsParser> eOptParser =
      clang::tooling::CommonOptionsParser::create(argc, argv,
                                                  CoreCategory,
                                                  llvm::cl::OneOrMore);

  if (auto E = eOptParser.takeError()) {
    llvm::errs() << "Error parsing arguments: " << toString(std::move(E)) << '\n';
    return EXIT_FAILURE;
  }

  // Determine Phase
  type_correct::Phase P = type_correct::Phase::Standalone;
  if (PhaseOpt == "map")
    P = type_correct::Phase::Map;
  else if (PhaseOpt == "reduce")
    P = type_correct::Phase::Standalone; // Special case handled below
  else if (PhaseOpt == "apply")
    P = type_correct::Phase::Apply;
  else if (PhaseOpt == "iterative")
    P = type_correct::Phase::Iterative;

  // Handle Explicit Reduce
  if (PhaseOpt == "reduce") {
    bool Changed = RunReduce(FactsDirOpt);
    return Changed ? 1 : 0; // Return 1 if changes occurred (script friendliness)
  }

  // Clang Tool Setup
  clang::tooling::RefactoringTool Tool(eOptParser->getCompilations(),
                                       eOptParser->getSourcePathList());

  // === Iterative Mode Execution ===
  if (P == type_correct::Phase::Iterative) {
    if (FactsDirOpt.empty()) {
      llvm::errs() << "Error: Iterative mode requires --facts-dir.\n";
      return EXIT_FAILURE;
    }

    int Iteration = 0;
    bool ConvergenceReached = false;

    // Fixed-Point Loop
    while (Iteration < MaxIterationsOpt && !ConvergenceReached) {
      Iteration++;
      llvm::outs() << "=== Iteration " << Iteration << " ===\n";

      // 1. Run Map Phase (Analyzes headers/TUs, writes local facts)
      TypeCorrectActionFactory MapFactory(type_correct::Phase::Iterative);
      int Ret = Tool.run(&MapFactory);
      if (Ret != 0) {
        llvm::errs() << "Tool run failed in iteration " << Iteration << "\n";
        return Ret;
      }

      // 2. Run Reduce Check (Merges local facts, checks if global state differs)
      bool Changed = RunReduce(FactsDirOpt);

      if (!Changed) {
        llvm::outs() << "Convergence reached after " << Iteration
                     << " iterations.\n";
        ConvergenceReached = true;
      } else {
        llvm::outs() << "Facts changed, continuing...\n";
      }
    }

    if (!ConvergenceReached) {
      llvm::errs() << "Warning: Max iterations reached without convergence.\n";
    }

    // Optional: Final application pass could go here, or user runs 'apply' manually.
    // For now, iterative mode updates facts. Rewriting usually happens via side-effect
    // or a final pass. In the current Matcher design, Iterative mode writes facts
    // AND writes code if -in-place is set, but usually one wants to solve first, then write.
    return 0;
  }

  // === Standard Single-Pass Execution ===
  TypeCorrectActionFactory Factory(P);

  // If Audit or Reporting is enabled, we don't strictly need runAndSave
  // because we handle output manually (Audit prints to stdout, Report writes JSON).
  // However, runAndSave is required to flush the Rewriter buffers to disk if -in-place is set.
  if (AuditOpt) {
      // In Audit mode, we definitely do NOT want to save to disk. Use run().
      return Tool.run(&Factory);
  }

  return Tool.runAndSave(&Factory);
}