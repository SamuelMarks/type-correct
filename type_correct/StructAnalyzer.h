/**
 * @file StructAnalyzer.h
 * @brief Header for Safety Analysis including System Boundary Detection.
 *
 * This module performs safety checks on declarations to prevent refactoring
 * of code that must remain fixed due to ABI/API constraints.
 *
 * Features:
 * 1. **Structural Analysis**: Checks packed attributes, unions, and bitfields.
 * 2. **Truncation Safety**: Analyzes usage to determine if type widening is
 * safe.
 * 3. **System Boundary Detection**: A combined heuristic using inclusion graphs
 *    and CMake build system analysis to determine if a file is "Fixed"
 *    (System/Third-Party) or "Modifiable" (User).
 *
 * @author SamuelMarks
 * @copyright CC0
 */

#ifndef TYPE_CORRECT_STRUCT_ANALYZER_H
#define TYPE_CORRECT_STRUCT_ANALYZER_H

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/Analysis/CFG.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>
#include <map>
#include <set>
#include <string>

#include "type_correct_export.h"

/**
 * @enum BoundaryStatus
 * @brief Cache state for file safety analysis.
 */
enum class BoundaryStatus {
  Unknown,    ///< Not yet analyzed.
  Modifiable, ///< Safe to rewrite (User Code).
  Fixed       ///< Unsafe to rewrite (System/Third-Party/Locked).
};

/**
 * @class StructAnalyzer
 * @brief Comprehensive Safety Engine for Decls (Structs, Classes, Functions,
 * Globals, Typedefs).
 *
 * Handles boundary detection for named declarations, enduring we don't modify
 * system headers or libraries. It supports the Punt-to-Typedef strategy by
 * validating if a typedef root is writable.
 */
class TYPE_CORRECT_EXPORT StructAnalyzer {
public:
  /**
   * @brief Construct the Analyzer.
   *
   * @param AllowABIChanges If false, even "Modifiable" structs are locked
   * to prevent memory layout changes.
   * @param ForceRewrite If true, bypasses most safety checks (System Headers
   * are still respected where possible, but Project/vendor checks are ignored).
   * @param ProjectRoot The canonical root path of the project for path
   * checking.
   */
  explicit StructAnalyzer(bool AllowABIChanges, bool ForceRewrite = false,
                          std::string ProjectRoot = "");

  /**
   * @brief Determines if a specific field declaration can be safely rewritten.
   * Checks ABI policies and boundary status.
   *
   * @param Field The field to check.
   * @param SM The SourceManager (for boundary check).
   * @return true If rewriting is permitted.
   */
  bool CanRewriteField(const clang::FieldDecl *Field, clang::SourceManager &SM);

  /**
   * @brief Determines if a Typedef declaration can be safely rewritten.
   * This is critical for the "Punt-to-Typedef" strategy. If a typedef
   * is in a system header or vendor library, it cannot count as a modifiable
   * root.
   *
   * @param TD The typedef declaration.
   * @param SM The SourceManager.
   * @return true if the typedef is user-code and safe to update.
   */
  bool CanRewriteTypedef(const clang::TypedefNameDecl *TD,
                         clang::SourceManager &SM);

  /**
   * @brief Master Safety Check: Is this Symbol Fixed?
   *
   * This is the primary entry point for `TypeCorrectMatcher`. It consolidates:
   * 1. System Header checks.
   * 2. Heuristic Path Pattern Matching (includes CMake scanning).
   * 3. Inclusion Graph Analysis (Viral Fixedness).
   *
   * Applies to Variables, Functions, Typedefs, Records.
   *
   * @param D The declaration to check.
   * @param SM The Source Manager.
   * @return true If the symbol must NOT be changed.
   */
  bool IsBoundaryFixed(const clang::NamedDecl *D, clang::SourceManager &SM);

  /**
   * @brief Checks if a record is "packed" (attribute or pragma).
   * @param Field The field context.
   * @return true If packed.
   */
  bool IsPacked(const clang::FieldDecl *Field) const;

  /**
   * @brief Analyzes a usage for truncation risks (CFG/Dominator logic).
   * @param SourceField The field.
   * @param EnclosingFunc The function context.
   * @param Ctx AST Context.
   */
  void AnalyzeTruncationSafety(const clang::FieldDecl *SourceField,
                               const clang::FunctionDecl *EnclosingFunc,
                               clang::ASTContext *Ctx);

  /**
   * @brief Returns fields flagged as unsafe by Truncation Analysis.
   * @return Set of FieldDecls.
   */
  std::set<const clang::FieldDecl *> GetLikelyUnsafeFields() const;

private:
  bool AllowABIChanges;
  bool ForceRewrite;
  std::string ProjectRoot;

  std::set<const clang::FieldDecl *> TruncationUnsafeFields;
  std::set<std::pair<const clang::FieldDecl *, const clang::FunctionDecl *>>
      AnalyzedCache;

  // Caching mechanism for file boundaries
  llvm::DenseMap<clang::FileID, BoundaryStatus> BoundaryCache;
  mutable llvm::StringMap<bool> CMakePathCache;

  BoundaryStatus CheckFileBoundary(clang::FileID FID, clang::SourceManager &SM);
  bool IsExternalPath(llvm::StringRef Path) const;
  bool AnalyzeCMakeDependency(llvm::StringRef FileDir) const;
};

#endif // TYPE_CORRECT_STRUCT_ANALYZER_H
