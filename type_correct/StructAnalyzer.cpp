/**
 * @file StructAnalyzer.cpp
 * @brief Implementation of StructAnalyzer logic.
 *
 * Implements the safety checks for refactoring class and struct members.
 * Verifies ABI flags and structural properties like packing and bitfields.
 *
 * @author SamuelMarks
 * @license CC0
 */

#include "StructAnalyzer.h"

#include <clang/AST/RecordLayout.h>
#include <clang/Basic/AttrKinds.h>
#include <clang/AST/Attr.h> // Fixed: Added to define PackedAttr

using namespace clang;

//-----------------------------------------------------------------------------
// StructAnalyzer Implementation
//-----------------------------------------------------------------------------

/**
 * @brief Constructor for StructAnalyzer.
 *
 * @param AllowABIChanges Flag to enable or disable rewrite of fields.
 */
StructAnalyzer::StructAnalyzer(bool AllowABIChanges)
    : AllowABIChanges(AllowABIChanges) {}

/**
 * @brief Checks if a field is eligible for rewriting.
 *
 * @param Field The AST FieldDecl node to check.
 * @return true if safe to rewrite, false otherwise.
 */
bool StructAnalyzer::CanRewriteField(const FieldDecl *Field) const {
  if (!Field)
    return false;

  // Policy Check: ABI Changes
  // If the user hasn't explicitly opted-in to changing struct definitions
  // (which changes memory layout and breaks binary compatibility), abort.
  if (!AllowABIChanges) {
    return false;
  }

  // Bitfield Check
  // We do not currently support promoting: int x : 3; -> size_t x : 3;
  // This changes semantics significantly and requires width analysis.
  if (Field->isBitField()) {
    return false;
  }

  // Packing Check
  if (IsPacked(Field)) {
    return false;
  }

  // Check for Union
  // Changing types inside a Union is dangerous as it affects all other members.
  if (const auto *RD = Field->getParent()) {
    if (RD->isUnion()) {
      return false;
    }
  }

  return true;
}

/**
 * @brief Detects if the parent record uses __attribute__((packed)) or #pragma pack.
 *
 * @param Field The field to inspect.
 * @return true if packed.
 */
bool StructAnalyzer::IsPacked(const FieldDecl *Field) const {
  if (!Field)
    return false;

  const RecordDecl *RD = Field->getParent();
  if (!RD)
    return false;

  // Direct attribute on the struct
  if (RD->hasAttr<PackedAttr>()) {
    return true;
  }

  // Attribute on the field itself
  if (Field->hasAttr<PackedAttr>()) {
    return true;
  }

  // There are deeper checks for alignment (ASTContext::getTypeInfo),
  // but attribute checking covers explicit packing directives which
  // are the main source of danger.
  return false;
}