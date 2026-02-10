/**
 * @file FactManager.cpp
 * @brief Implementation of CTU Fact serialization, merging, and convergence
 * logic.
 *
 * Implements the text-based protocol for exchanging type facts between
 * tool invocations.
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

// Format: USR <TAB> TypeName <TAB> IsField(int) <TAB> IsTypedef(int)
static const char DELIMITER = '\t';

/**
 * @brief Ranking function for standard integer width hierarchy.
 * Used during the Merge phase to determine the "winner".
 *
 * @param T The type string.
 * @return int Rank (higher is wider).
 */
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
    return 5; // Treat equivalent to size_t magnitude
  return 0;   // Unknown
}

bool FactManager::WriteFacts(const std::string &FilePath,
                             const std::map<std::string, SymbolFact> &Facts) {
  std::ofstream Out(FilePath);
  if (!Out.is_open()) {
    llvm::errs() << "Failed to open output fact file: " << FilePath << "\n";
    return false;
  }

  for (const auto &Pair : Facts) {
    const SymbolFact &F = Pair.second;
    // Format: USR | Type | IsField | IsTypedef
    Out << F.USR << DELIMITER << F.TypeName << DELIMITER
        << (F.IsField ? "1" : "0") << DELIMITER << (F.IsTypedef ? "1" : "0")
        << "\n";
  }

  Out.close();
  return true;
}

bool FactManager::ReadFacts(const std::string &FilePath,
                            std::vector<SymbolFact> &OutFacts) {
  std::ifstream In(FilePath);
  if (!In.is_open()) {
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
      // Optional 4th column for backward compatibility with older fact files
      if (Parts.size() >= 4) {
        Fact.IsTypedef = (Parts[3] == "1");
      }
      OutFacts.push_back(Fact);
    }
  }
  return true;
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
      // Preserve "IsTypedef" metadata if either source has it
      if (Raw.IsTypedef)
        It->second.IsTypedef = true;
    }
  }
  return Merged;
}

bool FactManager::IsConvergenceReached(
    const std::string &GlobalFilePath,
    const std::map<std::string, SymbolFact> &NewFacts) {
  // 1. Read existing facts from the previous global state
  std::vector<SymbolFact> ExistingVector;
  if (!ReadFacts(GlobalFilePath, ExistingVector)) {
    // If the file doesn't exist, we haven't converged (initial state)
    return false;
  }

  // 2. Convert vector to map for O(1) comparison
  std::map<std::string, SymbolFact> ExistingMap;
  for (const auto &F : ExistingVector) {
    ExistingMap[F.USR] = F;
  }

  // 3. Compare sizes
  if (ExistingMap.size() != NewFacts.size()) {
    return false;
  }

  // 4. Deep Compare elements (TypeName, Flags)
  return ExistingMap == NewFacts;
}

} // namespace ctu
} // namespace type_correct