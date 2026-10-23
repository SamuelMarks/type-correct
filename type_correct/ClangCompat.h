/**
 * @file ClangCompat.h
 * @brief Compatibility helpers across LLVM/Clang major versions.
 *
 * Provides inline/constexpr wrappers around Clang AST APIs that changed across
 * LLVM major releases (e.g., LLVM 15, 18, 22).
 */
#ifndef TYPE_CORRECT_CLANG_COMPAT_H
#define TYPE_CORRECT_CLANG_COMPAT_H

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Type.h>
#include <llvm/Config/llvm-config.h>

namespace type_correct::clang_compat {

/**
 * @brief Returns the Clang expression value kind representing a prvalue/rvalue.
 * @return clang::ExprValueKind The prvalue or rvalue kind for the active LLVM
 * version.
 */
constexpr clang::ExprValueKind PrValueKind() noexcept {
#if LLVM_VERSION_MAJOR >= 15
  return clang::VK_PRValue;
#else
  return clang::VK_RValue;
#endif
}

/**
 * @brief Returns the Clang tag type kind representing a struct.
 * @return clang::TagTypeKind The struct tag kind for the active LLVM version.
 */
constexpr clang::TagTypeKind StructTagKind() noexcept {
#if LLVM_VERSION_MAJOR >= 18
  return clang::TagTypeKind::Struct;
#else
  return clang::TTK_Struct;
#endif
}

/**
 * @brief Retrieves the canonical record/tag QualType for a RecordDecl across
 * LLVM versions.
 *
 * LLVM 22 removed `ASTContext::getRecordType` in favor of
 * `ASTContext::getCanonicalTagType`.
 *
 * @param Ctx The Clang ASTContext.
 * @param Decl The RecordDecl to query.
 * @return clang::QualType The record type for Decl.
 */
inline clang::QualType GetRecordType(const clang::ASTContext &Ctx,
                                     const clang::RecordDecl *Decl) noexcept {
#if LLVM_VERSION_MAJOR >= 22
  return Ctx.getCanonicalTagType(Decl);
#else
  return Ctx.getRecordType(Decl);
#endif
}

} // namespace type_correct::clang_compat

#endif // TYPE_CORRECT_CLANG_COMPAT_H
