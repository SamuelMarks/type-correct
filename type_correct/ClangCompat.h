#ifndef TYPE_CORRECT_CLANG_COMPAT_H
#define TYPE_CORRECT_CLANG_COMPAT_H

#include <clang/AST/Expr.h>
#include <clang/AST/Type.h>
#include <llvm/Config/llvm-config.h>

namespace type_correct::clang_compat {

constexpr clang::ExprValueKind PrValueKind() {
#if LLVM_VERSION_MAJOR >= 15
  return clang::VK_PRValue;
#else
  return clang::VK_RValue;
#endif
}

constexpr clang::TagTypeKind StructTagKind() {
#if LLVM_VERSION_MAJOR >= 18
  return clang::TagTypeKind::Struct;
#else
  return clang::TTK_Struct;
#endif
}

} // namespace type_correct::clang_compat

#endif // TYPE_CORRECT_CLANG_COMPAT_H
