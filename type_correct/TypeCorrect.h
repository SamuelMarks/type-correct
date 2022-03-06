//==============================================================================
// FILE:
//    TypeCorrect.h
//
// DESCRIPTION: Header for TypeCorrect.cpp (prototypes and exports for shared
// lib)
//
// License: CC0
//==============================================================================
#ifndef TYPE_CORRECT_H
#define TYPE_CORRECT_H

#include <clang/AST/ASTConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Rewrite/Core/Rewriter.h>

#include "type_correct_export.h"

//-----------------------------------------------------------------------------
// ASTMatcher callback
//-----------------------------------------------------------------------------
class TYPE_CORRECT_EXPORT TypeCorrectMatcher
    : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  explicit TypeCorrectMatcher(clang::Rewriter &LACRewriter) : LACRewriter(LACRewriter) {}
  // Callback that's executed whenever the Matcher in TypeCorrectASTConsumer
  // matches.
  void run(const clang::ast_matchers::MatchFinder::MatchResult &) override;
  // Callback that's executed at the end of the translation unit
  void onEndOfTranslationUnit() override;

private:
  clang::Rewriter LACRewriter;
  llvm::SmallSet<clang::FullSourceLoc, 8> EditedLocations;
};

//-----------------------------------------------------------------------------
// ASTConsumer
//-----------------------------------------------------------------------------
class TYPE_CORRECT_EXPORT TypeCorrectASTConsumer : public clang::ASTConsumer {
public:
  TypeCorrectASTConsumer(clang::Rewriter &R);
  void HandleTranslationUnit(clang::ASTContext &Ctx) override {
    Finder.matchAST(Ctx);
  }

private:
  clang::ast_matchers::MatchFinder Finder;
  TypeCorrectMatcher TCHandler;
};

#endif /* TYPE_CORRECT_H */
