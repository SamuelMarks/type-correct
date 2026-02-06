/**
 * @file FactManager.cpp
 * @brief Implementation of CTU Fact serialization and merging.
 *
 * @author SamuelMarks
 * @license CC0
 */

#include "FactManager.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <llvm/Support/raw_ostream.h>

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
        Out << F.USR << DELIMITER 
            << F.TypeName << DELIMITER 
            << (F.IsField ? "1" : "0") << "\n";
    }
    
    Out.close();
    return true;
}

bool FactManager::ReadFacts(const std::string &FilePath, 
                            std::vector<SymbolFact> &OutFacts) {
    std::ifstream In(FilePath);
    if (!In.is_open()) {
        llvm::errs() << "Failed to open input fact file: " << FilePath << "\n";
        return false;
    }

    std::string Line;
    while (std::getline(In, Line)) {
        if (Line.empty() || Line[0] == '#') continue;

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
    if (T == "unsigned char" || T == "char") return 1;
    if (T == "short" || T == "unsigned short") return 2;
    if (T == "int" || T == "unsigned int" || T == "unsigned") return 3;
    if (T == "long" || T == "unsigned long") return 4;
    if (T == "size_t" || T == "std::size_t") return 5;
    if (T == "long long" || T == "unsigned long long") return 6;
    if (T == "ptrdiff_t" || T == "std::ptrdiff_t") return 5; // Treat equivalent to size_t
    return 0; // Unknown
}

std::map<std::string, SymbolFact> FactManager::MergeFacts(const std::vector<SymbolFact> &RawFacts) {
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
            // If strict equality, we might prefer explicit size_t over unsigned long
            // depending on style, but this simple rank handles basic widening.
        }
    }
    return Merged;
}

} // namespace ctu
} // namespace type_correct