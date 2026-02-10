/** 
 * @file StructAnalyzer.cpp
 * @brief Implementation of Heuristic System Boundary Detection with Typedef
 * Awareness. 
 * 
 * Implements logic to decide if code is "User Code" or "System Code". 
 * Supports specific checks for Typedef definitions to enable global refactoring. 
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

  // 2. Boundary Check
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

bool StructAnalyzer::CanRewriteTypedef(const TypedefNameDecl *TD, 
                                       SourceManager &SM) { 
  if (!TD) 
    return false; 

  // Typedefs are just another NamedDecl, but we wrap it to provide
  // specific extension points if we want to block specific typedef patterns
  // (e.g. standard library shims). 
  return !IsBoundaryFixed(TD, SM); 
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
  if (ForceRewrite) 
    return false; 

  // 1. Inline Location Check (Macros/CmdLine) 
  SourceLocation Loc = D->getLocation(); 
  if (Loc.isInvalid()) 
    return true; 

  // Handle Macro expansions by finding the spelling location
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
  if (ForceRewrite) 
    return false; 

  // A. Simple String Heuristics
  static const std::vector<std::string> Patterns = { 
      "/usr/", "/opt/", 
      "node_modules", "bower_components", 
      "third_party", "external", 
      "build/_deps", "CMake/Modules"}; 

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
    if (!ProjectRoot.empty()) { 
      llvm::SmallString<128> AbsCMake(CMakePath); 
      llvm::sys::fs::make_absolute(AbsCMake); 

      llvm::SmallString<128> AbsRoot(ProjectRoot); 
      llvm::sys::path::append(AbsRoot, "CMakeLists.txt"); 

      if (AbsCMake == AbsRoot) { 
        return CMakePathCache[FileDir] = false; // Root is safe
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

  // Recursive step up
  if (!IsFixed) { 
    llvm::StringRef Parent = llvm::sys::path::parent_path(FileDir); 
    if (Parent != FileDir) { 
      if (!ProjectRoot.empty()) { 
        llvm::SmallString<128> AbsParent(Parent); 
        llvm::sys::fs::make_absolute(AbsParent); 
        if (!llvm::StringRef(AbsParent).starts_with(ProjectRoot)) { 
          return CMakePathCache[FileDir] = true; 
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

  // FIX: Main File Check (Always User Code). 
  // This is critical for unit tests which run on virtual main files without FileEntry
  // Moved before FileEntry checks. 
  if (FID == SM.getMainFileID()) { 
    return BoundaryCache[FID] = BoundaryStatus::Modifiable; 
  } 

  // 2. Fast System Check (Clang Internal) 
  if (SM.isInSystemHeader(SM.getLocForStartOfFile(FID))) { 
    return BoundaryCache[FID] = BoundaryStatus::Fixed; 
  } 

  // 3. Path Heuristics
  auto EntryRef = SM.getFileEntryRefForID(FID); 
  if (!EntryRef) { 
    // If no FileEntry and not MainID, assume system/virtual/unsafe
    return BoundaryCache[FID] = BoundaryStatus::Fixed; 
  } 

  if (IsExternalPath(EntryRef->getName())) { 
    return BoundaryCache[FID] = BoundaryStatus::Fixed; 
  } 

  // 4. Inclusion Graph (Viral Fixedness) 
  SourceLocation IncludeLoc = SM.getIncludeLoc(FID); 
  if (IncludeLoc.isValid()) { 
    FileID IncluderFID = SM.getFileID(IncludeLoc); 
    if (IncluderFID != FID && IncluderFID.isValid()) { 
      BoundaryStatus ParentStatus = CheckFileBoundary(IncluderFID, SM); 
      if (ParentStatus == BoundaryStatus::Fixed) { 
        return BoundaryCache[FID] = BoundaryStatus::Fixed; 
      } 
    } 
  } 

  return BoundaryCache[FID] = BoundaryStatus::Modifiable; 
} 

//----------------------------------------------------------------------------- 
// Truncation Analysis (Stub) 
//----------------------------------------------------------------------------- 

void StructAnalyzer::AnalyzeTruncationSafety( 
    const clang::FieldDecl *SourceField, 
    const clang::FunctionDecl *EnclosingFunc, clang::ASTContext *Ctx) { 
  // Placeholder for CFG/Dominator analysis
} 

std::set<const clang::FieldDecl *>
StructAnalyzer::GetLikelyUnsafeFields() const { 
  return TruncationUnsafeFields; 
}