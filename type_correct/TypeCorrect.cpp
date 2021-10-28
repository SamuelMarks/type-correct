//==============================================================================
// FILE:
//    TypeCorrect.cpp
//
// DESCRIPTION:
//  Convert types to be correct (comparison type and return type take precedence)
//
//    * long f() { int a = 0; return a;} --> long f() { long a = 0; return a;}
//    * long g() { return 0L; }; int b = g(); --> long g() { return 0L; }; long b = g();
//    * for(int i=0; i<g(); i++) {} --> for(long i=0; i<g(); i++) {}
//
//  Resolves common UB and incorrect typings in C and C++ code.
//
// USAGE:
//    * clang -cc1 -load <BUILD_DIR>/lib/libTypeCorrect.dylib `\`
//        -plugin TypeCorrect test/MBA_add_int.cpp
//
// License: CC0
//==============================================================================

#include <llvm/Support/CommandLine.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendPluginRegistry.h>

#include "TypeCorrect.h"

//-----------------------------------------------------------------------------
// TypeCorrect - implementation
//-----------------------------------------------------------------------------
void TypeCorrectMatcher::run(const clang::ast_matchers::MatchFinder::MatchResult &Result) {
    // ASTContext is used to retrieve the source location
    clang::ASTContext *Ctx = Result.Context;

    // Callee and caller are accessed via .bind("callee") and .bind("caller"),
    // respecitvely, from the ASTMatcher
    const clang::FunctionDecl *CalleeDecl =
            Result.Nodes.getNodeAs<clang::FunctionDecl>("callee");
    const clang::CallExpr *TheCall = Result.Nodes.getNodeAs<clang::CallExpr>("caller");

    // Basic sanity checking
    assert(TheCall && CalleeDecl &&
           "The matcher matched, so callee and caller should be non-null");

    // No arguments means there's nothing to comment
    if (CalleeDecl->parameters().empty())
        return;

    // Get the arguments
    clang::Expr const *const *Args = TheCall->getArgs();
    clang::QualType const Returns = TheCall->getCallReturnType(*Ctx);
    size_t NumArgs = TheCall->getNumArgs();

    // If this is a call to an overloaded operator (e.g. `+`), then the first
    // parameter is the object itself (i.e. `this` pointer). Skip it.
    if (isa<clang::CXXOperatorCallExpr>(TheCall)) {
        Args++;
        NumArgs--;
    }

    // For each argument match it with the callee parameter. If it is an integer,
    // float, boolean, character or string literal insert a comment.
    for (unsigned Idx = 0; Idx < NumArgs; Idx++) {
        const clang::Expr *AE = Args[Idx]->IgnoreParenCasts();
        /*llvm::outs() << "Args[" << Idx << "] = ";
        Args[Idx]->dump();
        llvm::outs() << ';';*/

        if (!dyn_cast<clang::IntegerLiteral>(AE) && !dyn_cast<clang::CXXBoolLiteralExpr>(AE) &&
            !dyn_cast<clang::FloatingLiteral>(AE) && !dyn_cast<clang::StringLiteral>(AE) &&
            !dyn_cast<clang::CharacterLiteral>(AE))
            continue;

        // Parameter declaration
        clang::ParmVarDecl *ParamDecl = CalleeDecl->parameters()[Idx];

        // Source code locations (parameter and argument)
        clang::FullSourceLoc ParamLocation = Ctx->getFullLoc(ParamDecl->getBeginLoc());
        clang::FullSourceLoc ArgLoc = Ctx->getFullLoc(AE->getBeginLoc());

        if (ParamLocation.isValid() && !ParamDecl->getDeclName().isEmpty() &&
            EditedLocations.insert(ArgLoc).second)
            // Insert the comment immediately before the argument
            LACRewriter.InsertText(
                    ArgLoc,
                    (llvm::Twine("/*") + ParamDecl->getDeclName().getAsString() + "=*/").str());
    }
}

void TypeCorrectMatcher::onEndOfTranslationUnit() {
    // Replace in place
    // LACRewriter.overwriteChangedFiles();

    // Output to stdout
    LACRewriter.getEditBuffer(LACRewriter.getSourceMgr().getMainFileID())
            .write(llvm::outs());
}

TypeCorrectASTConsumer::TypeCorrectASTConsumer(clang::Rewriter &R) : TCHandler(R) {
    const clang::ast_matchers::StatementMatcher CallSiteMatcher =
            clang::ast_matchers::callExpr(
                    clang::ast_matchers::allOf(clang::ast_matchers::callee(clang::ast_matchers::functionDecl(clang::ast_matchers::unless(clang::ast_matchers::isVariadic())).bind("callee")),
                          clang::ast_matchers::unless(clang::ast_matchers::cxxMemberCallExpr(
                                  clang::ast_matchers::on(clang::ast_matchers::hasType(clang::ast_matchers::substTemplateTypeParmType())))),
                          clang::ast_matchers::anyOf(clang::ast_matchers::hasAnyArgument(clang::ast_matchers::ignoringParenCasts(clang::ast_matchers::cxxBoolLiteral())),
                                clang::ast_matchers::hasAnyArgument(clang::ast_matchers::ignoringParenCasts(clang::ast_matchers::integerLiteral())),
                                clang::ast_matchers::hasAnyArgument(clang::ast_matchers::ignoringParenCasts(clang::ast_matchers::stringLiteral())),
                                clang::ast_matchers::hasAnyArgument(clang::ast_matchers::ignoringParenCasts(clang::ast_matchers::characterLiteral())),
                                clang::ast_matchers::hasAnyArgument(clang::ast_matchers::ignoringParenCasts(clang::ast_matchers::floatLiteral())))))
                    .bind("caller");

    // LAC is the callback that will run when the ASTMatcher finds the pattern
    // above.
    Finder.addMatcher(CallSiteMatcher, &TCHandler);
}

//-----------------------------------------------------------------------------
// FrotendAction
//-----------------------------------------------------------------------------
class TCPluginAction : public clang::PluginASTAction{
public:
    // Our plugin can alter behavior based on the command line options
    bool ParseArgs(const clang::CompilerInstance &,
                   const std::vector<std::string> &) override {
        return true;
    }

    // Returns our ASTConsumer per translation unit.
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI,
                                                   llvm::StringRef file) override {
        RewriterForTC.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
        return std::make_unique<TypeCorrectASTConsumer>(RewriterForTC);
    }

private:
    clang::Rewriter RewriterForTC;
};

//-----------------------------------------------------------------------------
// Registration
//-----------------------------------------------------------------------------
static clang::FrontendPluginRegistry::Add<TCPluginAction>
        X(/*Name=*/"LAC",
        /*Desc=*/"Literal Argument Commenter");
