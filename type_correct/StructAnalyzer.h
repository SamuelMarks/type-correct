/**
 * @file StructAnalyzer.h
 * @brief Header for the StructAnalyzer module.
 *
 * This file declares the StructAnalyzer class, which is responsible for
 * determining the safety and validity of refactoring fields within structs and
 * classes. It handles ABI-breaking change policies and field-specific
 * AST checks.
 *
 * @author SamuelMarks
 * @license CC0
 */

#ifndef TYPE_CORRECT_STRUCT_ANALYZER_H
#define TYPE_CORRECT_STRUCT_ANALYZER_H

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Type.h>
#include <clang/Rewrite/Core/Rewriter.h>

#include "type_correct_export.h"

/**
 * @class StructAnalyzer
 * @brief Analysis engine for struct and class member refactoring.
 *
 * This class serves as a gatekeeper for changes to record types (structs,
 * classes, unions). It enforces policies regarding ABI breakage and checks
 * simple validity constraints (e.g., ignoring bitfields or packed structs
 * where type promotion might be physically impossible or dangerous).
 */
class TYPE_CORRECT_EXPORT StructAnalyzer {
public:
  /**
   * @brief Construct a new Struct Analyzer.
   *
   * @param AllowABIChanges Boolean flag indicating if ABI-breaking changes
   * (modifying headers of structs) are permitted.
   */
  explicit StructAnalyzer(bool AllowABIChanges);

  /**
   * @brief Determines if a specific field declaration can be safely rewritten.
   *
   * Performs the following checks:
   * 1. Is the ABI Change toggle enabled?
   * 2. Is the field a bitfield? (Currently unsupported).
   * 3. Is the field part of a packed record? (Currently unsupported).
   * 4. Is the field a system header declaration? (Implicitly handled by
   *    TypeCorrectMatcher's IsModifiable, but reiterated here for logic
   * grouping).
   *
   * @param Field The field declaration to inspect.
   * @return true if the field is a valid candidate for rewriting.
   * @return false otherwise.
   */
  bool CanRewriteField(const clang::FieldDecl *Field) const;

  /**
   * @brief Checks if the record containing the field is "packed".
   *
   * Modifications to packed structs can alter alignment unexpectedly.
   * We generally avoid touching these unless explicitly handled.
   *
   * @param Field The field to check.
   * @return true if the parent record is packed.
   */
  bool IsPacked(const clang::FieldDecl *Field) const;

private:
  /** @brief Flag permitting modification of struct definitions. */
  bool AllowABIChanges;
};

#endif // TYPE_CORRECT_STRUCT_ANALYZER_H