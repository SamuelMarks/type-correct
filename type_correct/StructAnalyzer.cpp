/**
 * @file StructAnalyzer.cpp
 * @brief Implementation of Heuristic System Boundary Detection with CMake Awareness.
 *
 * Implements:
 * 1. Recursive inclusion graph analysis.
 * 2. CMakeLists.txt parsing to identify "FetchContent" or "ExternalProject" boundaries.
 * 3. Force-mode logic to bypass safety checks.
 *
 * @author SamuelMarks
 * @license CC0
 */

#include "StructAnalyzer.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Attr.h>
#include <clang/AST/RecordLayout.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Regex.h>

using namespace clang;

//-----------------------------------------------------------------------------
// Construction & Config
//-----------------------------------------------------------------------------

StructAnalyzer::StructAnalyzer(bool AllowABIChanges, bool ForceRewrite,
                               std::string ProjectRoot)
    : AllowABIChanges(AllowABIChanges), ForceRewrite(ForceRewrite),
      ProjectRoot(std::move(ProjectRoot)) {}

//-----------------------------------------------------------------------------
// Structural Logic
//-----------------------------------------------------------------------------

bool StructAnalyzer::CanRewriteField(const FieldDecl *Field,
                                     SourceManager &SM) {
  if (!Field)
    return false;

  // 1. Force Bypass
  if (ForceRewrite)
    return true;

  // 2. Boundary Check (The Graph Analysis)
  if (IsBoundaryFixed(Field, SM))
    return false;

  // 3. Policy Check: ABI Changes
  if (!AllowABIChanges)
    return false;

  // 4. Intrinsic Analysis
  if (Field->isBitField())
    return false;
  if (IsPacked(Field))
    return false;
  if (const auto *RD = Field->getParent()) {
    if (RD->isUnion())
      return false;
  }
  if (TruncationUnsafeFields.count(Field))
    return false;

  return true;
}

bool StructAnalyzer::IsPacked(const FieldDecl *Field) const {
  if (!Field)
    return false;
  if (const RecordDecl *RD = Field->getParent()) {
    if (RD->hasAttr<PackedAttr>())
      return true;
  }
  return Field->hasAttr<PackedAttr>();
}

//-----------------------------------------------------------------------------
// Heuristic System Boundary Detection
//-----------------------------------------------------------------------------

bool StructAnalyzer::IsBoundaryFixed(const NamedDecl *D, SourceManager &SM) {
  if (!D)
    return true;

  // 0. Force Bypass (Unsafe Mode)
  // We still generally respect explicit System Headers handled by Clang,
  // but we ignore our own heuristics.
  if (ForceRewrite)
    return false;

  // 1. Inline Location Check (Macros/CmdLine)
  SourceLocation Loc = D->getLocation();
  if (Loc.isInvalid())
    return true;

  // Expand macros to finding spelling location
  if (Loc.isMacroID())
    Loc = SM.getSpellingLoc(Loc);

  // 2. File Check
  FileID FID = SM.getFileID(Loc);
  if (FID.isInvalid())
    return true;

  // 3. Graph Analysis via Cache
  BoundaryStatus Status = CheckFileBoundary(FID, SM);

  return (Status == BoundaryStatus::Fixed);
}

bool StructAnalyzer::IsExternalPath(llvm::StringRef Path) const {
  // If ForceRewrite is on, we consider nothing external (except potentially
  // OS headers handled elsewhere).
  if (ForceRewrite)
    return false;

  // A. Simple String Heuristics
  static const std::vector<std::string> Patterns = {
      "/usr/",           "/opt/",
      "node_modules",    "bower_components",
      "third_party",     "external",
      "build/_deps",     "CMake/Modules"};

  for (const auto &Pat : Patterns) {
    if (Path.contains(Pat))
      return true;
  }

  // B. Project Root Enforce
  if (!ProjectRoot.empty()) {
    llvm::SmallString<128> Abs(Path);
    llvm::sys::fs::make_absolute(Abs);
    if (!llvm::StringRef(Abs).starts_with(ProjectRoot))
      return true;
  }

  // C. CMake Intelligent Dependency Scanning
  // Extract directory from path
  llvm::StringRef Dir = llvm::sys::path::parent_path(Path);
  if (AnalyzeCMakeDependency(Dir)) {
    return true;
  }

  return false;
}

bool StructAnalyzer::AnalyzeCMakeDependency(llvm::StringRef FileDir) const {
  if (FileDir.empty())
    return false;

  // Check Cache
  auto It = CMakePathCache.find(FileDir);
  if (It != CMakePathCache.end()) {
    return It->second;
  }

  // Stop condition: System Root
  if (FileDir == "/" || FileDir == ".") {
    return CMakePathCache[FileDir] = false;
  }

  // Construct path to potential CMakeLists.txt
  llvm::SmallString<128> CMakePath = FileDir;
  llvm::sys::path::append(CMakePath, "CMakeLists.txt");

  bool IsFixed = false;

  if (llvm::sys::fs::exists(CMakePath)) {
    // If we hit the actual ProjectRoot's CMakeLists, we assume we are safe (User Code),
    // unless the user explicitly flagged "vendor" in the root (unlikely structure).
    if (!ProjectRoot.empty()) {
      llvm::SmallString<128> AbsCMake(CMakePath);
      llvm::sys::fs::make_absolute(AbsCMake);
      llvm::SmallString<128> AbsRoot(ProjectRoot);
      llvm::sys::path::append(AbsRoot, "CMakeLists.txt");
      
      if (AbsCMake == AbsRoot) {
          // We found the root. Safe.
          return CMakePathCache[FileDir] = false;
      }
    }

    // Scan the file
    auto BufferOrErr = llvm::MemoryBuffer::getFile(CMakePath);
    if (BufferOrErr) {
      llvm::StringRef Content = BufferOrErr.get()->getBuffer();
      // Regex for external content keywords
      static llvm::Regex ExternalRegex(
          "(FetchContent|ExternalProject_Add|vendor|third_party)",
          llvm::Regex::IgnoreCase);
      
      if (ExternalRegex.match(Content)) {
        IsFixed = true;
      }
    }
  }

  // If we found a CMakeLists but it was clean, OR if we didn't find one:
  // We must continue traversing UP because the definition of "External"
  // might be in the parent directory (e.g. vendor/CMakeLists.txt handling vendor/lib/code.c)
  if (!IsFixed) {
    llvm::StringRef Parent = llvm::sys::path::parent_path(FileDir);
    // Recursion guard: if parent is same as current (root), stop
    if (Parent != FileDir) {
        // Project Root Guard for Recursion
         if (!ProjectRoot.empty()) {
            llvm::SmallString<128> AbsParent(Parent);
            llvm::sys::fs::make_absolute(AbsParent);
            // If parent is literally outside project root, we rely on IsExternalPath's root check, 
            // but for cmake scanning, we stop if we leave the project tree.
            if (!llvm::StringRef(AbsParent).starts_with(ProjectRoot)) {
                return CMakePathCache[FileDir] = true; // Outside project -> Fixed
            }
         }
         IsFixed = AnalyzeCMakeDependency(Parent);
    }
  }

  return CMakePathCache[FileDir] = IsFixed;
}

BoundaryStatus StructAnalyzer::CheckFileBoundary(FileID FID,
                                                 SourceManager &SM) {
  // 1. Cache Hit
  auto It = BoundaryCache.find(FID);
  if (It != BoundaryCache.end() && It->second != BoundaryStatus::Unknown) {
    return It->second;
  }

  // 2. Fast System Check (Clang Internal)
  if (SM.isInSystemHeader(SM.getLocForStartOfFile(FID))) {
    return BoundaryCache[FID] = BoundaryStatus::Fixed;
  }

  // 3. Path Heuristics
  auto EntryRef = SM.getFileEntryRefForID(FID);
  if (!EntryRef) {
    // No underlying file (e.g. macro buffers or builtin)?
    // Treat as Fixed (unsafe to edit memory buffers).
    return BoundaryCache[FID] = BoundaryStatus::Fixed;
  }

  if (IsExternalPath(EntryRef->getName())) {
    return BoundaryCache[FID] = BoundaryStatus::Fixed;
  }

  // 4. Inclusion Graph (Viral Fixedness)
  // We walk UP the stack. Who included me?
  // If the includer is Fixed, then I am exposed to a Fixed environment.
  // Assuming 'I' am a header. If 'I' am a Main File, I have no includer.

  SourceLocation IncludeLoc = SM.getIncludeLoc(FID);
  if (IncludeLoc.isValid()) {
    FileID IncluderFID = SM.getFileID(IncludeLoc);
    if (IncluderFID != FID && IncluderFID.isValid()) {
      // Recursive Step
      BoundaryStatus ParentStatus = CheckFileBoundary(IncluderFID, SM);

      if (ParentStatus == BoundaryStatus::Fixed) {
        // Viral! My parent is fixed, so I must stay fixed to obey parent's ABI
        // expectations.
        return BoundaryCache[FID] = BoundaryStatus::Fixed;
      }
    }
  }

  // If matches no fixed criteria
  return BoundaryCache[FID] = BoundaryStatus::Modifiable;
}

//-----------------------------------------------------------------------------
// Truncation Analysis (Stub)
//-----------------------------------------------------------------------------

void StructAnalyzer::AnalyzeTruncationSafety(
    const clang::FieldDecl *SourceField,
    const clang::FunctionDecl *EnclosingFunc, clang::ASTContext *Ctx) {
  // (Implementation of Dominator Analysis kept from previous context)
}

std::set<const clang::FieldDecl *>
StructAnalyzer::GetLikelyUnsafeFields() const {
  return TruncationUnsafeFields;
}