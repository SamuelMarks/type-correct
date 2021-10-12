//==============================================================================
// FILE:
//    TypeCorrect.h
//
// DESCRIPTION: Header for TypeCorrect.cpp (prototypes and exports for shared lib)
//
// License: CC0
//==============================================================================
#ifndef TYPE_CORRECT_H
#define TYPE_CORRECT_H

#include "clang/AST/ASTConsumer.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Rewrite/Frontend/FixItRewriter.h"
#include "clang/Tooling/CommonOptionsParser.h"

//-----------------------------------------------------------------------------
// ASTFinder callback
//-----------------------------------------------------------------------------
class TypeCorrectMatcher
        : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
    explicit TypeCorrectMatcher(clang::Rewriter &RewriterForTypeCorrect)
            : TypeCorrectRewriter(RewriterForTypeCorrect) {}
    void onEndOfTranslationUnit() override;

    void run(const clang::ast_matchers::MatchFinder::MatchResult &) override;

private:
    clang::Rewriter TypeCorrectRewriter;
};

//-----------------------------------------------------------------------------
// ASTConsumer
//-----------------------------------------------------------------------------
class TypeCorrectASTConsumer : public clang::ASTConsumer {
public:
    TypeCorrectASTConsumer(clang::Rewriter &R);
    void HandleTranslationUnit(clang::ASTContext &Ctx) override {
        Finder.matchAST(Ctx);
    }

private:
    clang::ast_matchers::MatchFinder Finder;
    TypeCorrectMatcher TypeCorrectHandler;
};

#endif /* TYPE_CORRECT_H */
