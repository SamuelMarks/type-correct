/**
 * @file FactManager.cpp
 * @brief Implementation of CTU Fact serialization, merging, and convergence
 * logic.
 *
 * @author SamuelMarks
 * @license CC0
 */

#include "FactManager.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <llvm/Support/raw_ostream.h>
#include <sstream>

namespace type_correct {
namespace ctu {

// Format: USR <TAB> TypeName <TAB> IsField (1/0)
static const char DELIMITER = '\t';

bool FactManager::WriteFacts(const std::string &FilePath,
                             const std::map<std::string, SymbolFact> &Facts) {
  std::ofstream Out(FilePath);
  if (!Out.is_open()) {
    llvm::errs() << "Failed to open output fact file: " << FilePath << "\n";
    return false;
  }

  for (const auto &Pair : Facts) {
    const SymbolFact &F = Pair.second;
    Out << F.USR << DELIMITER << F.TypeName << DELIMITER
        << (F.IsField ? "1" : "0") << "\n";
  }

  Out.close();
  return true;
}

bool FactManager::ReadFacts(const std::string &FilePath,
                            std::vector<SymbolFact> &OutFacts) {
  std::ifstream In(FilePath);
  if (!In.is_open()) {
    // It is not necessarily an error if a file is missing in some contexts
    // (e.g. first run of global), but usually we expect files to exist.
    return false;
  }

  std::string Line;
  while (std::getline(In, Line)) {
    if (Line.empty() || Line[0] == '#')
      continue;

    std::stringstream SS(Line);
    std::string Seg;
    std::vector<std::string> Parts;

    while (std::getline(SS, Seg, DELIMITER)) {
      Parts.push_back(Seg);
    }

    if (Parts.size() >= 3) {
      SymbolFact Fact;
      Fact.USR = Parts[0];
      Fact.TypeName = Parts[1];
      Fact.IsField = (Parts[2] == "1");
      OutFacts.push_back(Fact);
    }
  }
  return true;
}

// Rudimentary ranking for type width
static int GetTypeRank(const std::string &T) {
  if (T == "unsigned char" || T == "char")
    return 1;
  if (T == "short" || T == "unsigned short")
    return 2;
  if (T == "int" || T == "unsigned int" || T == "unsigned")
    return 3;
  if (T == "long" || T == "unsigned long")
    return 4;
  if (T == "size_t" || T == "std::size_t")
    return 5;
  if (T == "long long" || T == "unsigned long long")
    return 6;
  if (T == "ptrdiff_t" || T == "std::ptrdiff_t")
    return 5; // Treat equivalent to size_t
  return 0;   // Unknown
}

std::map<std::string, SymbolFact>
FactManager::MergeFacts(const std::vector<SymbolFact> &RawFacts) {
  std::map<std::string, SymbolFact> Merged;

  for (const auto &Raw : RawFacts) {
    auto It = Merged.find(Raw.USR);
    if (It == Merged.end()) {
      Merged[Raw.USR] = Raw;
    } else {
      // Conflict Resolution: Pick wider type
      int CurrentRank = GetTypeRank(It->second.TypeName);
      int NewRank = GetTypeRank(Raw.TypeName);

      if (NewRank > CurrentRank) {
        It->second.TypeName = Raw.TypeName;
      }
      // If ranks are equal, we effectively keep the existing one (first writer
      // wins or existing global wins).
    }
  }
  return Merged;
}

bool FactManager::IsConvergenceReached(
    const std::string &GlobalFilePath,
    const std::map<std::string, SymbolFact> &NewFacts) {
  // 1. Read existing facts
  std::vector<SymbolFact> ExistingVector;
  if (!ReadFacts(GlobalFilePath, ExistingVector)) {
    // If we can't read the old file, we definitely haven't converged (new state
    // created).
    return false;
  }

  // 2. Convert vector to map for comparison (handling duplicates if file is
  // malformed)
  std::map<std::string, SymbolFact> ExistingMap;
  for (const auto &F : ExistingVector) {
    // We assume file is clean; if duplicates exist, last write wins in logic,
    // but MergeFacts logic is preferred. Here we are just checking state
    // equality.
    ExistingMap[F.USR] = F;
  }

  // 3. Compare sizes
  if (ExistingMap.size() != NewFacts.size()) {
    return false;
  }

  // 4. Compare elements
  // operator== for map compares element-by-element using ValueType's
  // operator==.
  return ExistingMap == NewFacts;
}

} // namespace ctu
} // namespace type_correct