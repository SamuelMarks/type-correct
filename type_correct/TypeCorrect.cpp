/**
 * @file TypeCorrect.cpp
 * @brief Implementation of TypeCorrect logic.
 *
 * Implements the AST Matchers to find type mismatches and the Rewriter logic
 * to physically replace the incorrect types in the source buffer.
 * Includes logic for handling decltype(expr) string generation, smart 'auto' replacement,
 * and fixes for overlapping loop matcher replacements.
 *
 * @author Samuel Marks
 * @license CC0
 */

#include <optional>

#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendPluginRegistry.h>
#include <clang/Lex/Lexer.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include "TypeCorrect.h"

// Bring common AST matchers into scope for readability
using namespace clang;
using namespace clang::ast_matchers;

//-----------------------------------------------------------------------------
// TypeCorrectMatcher - Implementation
//-----------------------------------------------------------------------------

TypeCorrectMatcher::TypeCorrectMatcher(clang::Rewriter &Rewriter, bool UseDecltype, bool ExpandAuto)
    : Rewriter(Rewriter), UseDecltype(UseDecltype), ExpandAuto(ExpandAuto) {}

void TypeCorrectMatcher::run(const MatchFinder::MatchResult &Result) {
  ASTContext *Ctx = Result.Context;

  //---------------------------------------------------------------------------
  // Case 1: Variable Initialization Mismatch
  // Pattern: Type var = function_call();
  // Example: int n = strlen("...");
  //---------------------------------------------------------------------------
  if (const auto *Var = Result.Nodes.getNodeAs<VarDecl>("bound_var_decl")) {
    const auto *InitCall = Result.Nodes.getNodeAs<CallExpr>("bound_var_init");

    if (Var && InitCall) {
      QualType VarType = Var->getType();
      QualType InitType = InitCall->getCallReturnType(*Ctx);

      // Compare canonical types to ignore typedef sugar (e.g. size_t vs
      // unsigned long)
      if (VarType.getCanonicalType() != InitType.getCanonicalType()) {
        ResolveType(Var->getTypeSourceInfo()->getTypeLoc(), InitType, Ctx, Var);
      }
    }
  }

  //---------------------------------------------------------------------------
  // Case 2: Function Return Type Mismatch
  // Pattern: RetType function() { return val; }
  // Example: int f() { return long_val; }
  //---------------------------------------------------------------------------
  else if (const auto *Func =
               Result.Nodes.getNodeAs<FunctionDecl>("bound_func_decl")) {
    const auto *RetStmtNode =
        Result.Nodes.getNodeAs<ReturnStmt>("bound_ret_stmt");
    const auto *RetVal = Result.Nodes.getNodeAs<Expr>("bound_ret_val");

    if (Func && RetStmtNode && RetVal) {
      QualType DeclaredRetType = Func->getReturnType();
      QualType ActualRetType = RetVal->getType();

      if (DeclaredRetType.getCanonicalType() !=
          ActualRetType.getCanonicalType()) {
        // For functions, the TypeLoc is a FunctionTypeLoc. We need the Return Loc.
        TypeLoc TL = Func->getTypeSourceInfo()->getTypeLoc();
        if (auto FTL = TL.getAs<FunctionTypeLoc>()) {
          ResolveType(FTL.getReturnLoc(), ActualRetType, Ctx, nullptr);
        }
      }
    }
  }

  //---------------------------------------------------------------------------
  // Case 3: For Loop Mismatch
  // Covers two sub-strategies:
  // A. Condition-based: for(int i=0; i < strlen(); ...)
  // B. Init-based:      for(int i=strlen(); i!=0; ...)
  //---------------------------------------------------------------------------
  else if (const auto *Loop =
               Result.Nodes.getNodeAs<ForStmt>("bound_for_loop")) {
    const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("bound_loop_var");
    const auto *LoopLimit =
        Result.Nodes.getNodeAs<CallExpr>("bound_loop_limit");

    // Optional: Retrieve the instance caller if it was a member call
    // (e.g., 'numbers' in numbers.size()).
    const auto *LoopInstance = Result.Nodes.getNodeAs<Expr>("bound_call_instance");

    if (Loop && LoopVar && LoopLimit) {
      QualType VarType = LoopVar->getType();
      QualType LimitType = LoopLimit->getCallReturnType(*Ctx);

      if (VarType.getCanonicalType() != LimitType.getCanonicalType()) {
        ResolveType(LoopVar->getTypeSourceInfo()->getTypeLoc(), LimitType, Ctx, LoopVar, LoopInstance);
      }
    }
  }
}

void TypeCorrectMatcher::ResolveType(const TypeLoc &OldLoc,
                                     const QualType &NewType, 
                                     ASTContext *Ctx,
                                     const VarDecl *BoundVar,
                                     const Expr *BaseExpr) {
  // We need to drill down to the unqualified type locator.
  TypeLoc TL = OldLoc;
  while (true) {
    if (auto QTL = TL.getAs<QualifiedTypeLoc>()) {
      TL = QTL.getUnqualifiedLoc();
    } else {
      break;
    }
  }

  //---------------------------------------------------------------------------
  // Logic: AutoType Handling
  //---------------------------------------------------------------------------
  if (auto ATL = TL.getAs<AutoTypeLoc>()) {
    if (!ExpandAuto && BoundVar && BoundVar->getInit()) {
        const Expr *Init = BoundVar->getInit()->IgnoreParenImpCasts();
        bool IsFunCall = llvm::isa<CallExpr>(Init) || llvm::isa<CXXMemberCallExpr>(Init);
        
        // If initialized by call and we aren't aggressive, preserve auto.
        if (IsFunCall) {
            return; 
        }
    }
  }

  // Determine the string representation of the new type.
  std::string NewTypeStr;

  // DECLTYPE GENERATION STRATEGY
  bool DecltypeStrategySuccess = false;

  if (UseDecltype && BaseExpr) {
     SourceManager &SM = Rewriter.getSourceMgr();
     CharSourceRange Range = CharSourceRange::getTokenRange(BaseExpr->getSourceRange());
     StringRef BaseText = Lexer::getSourceText(Range, SM, Ctx->getLangOpts());

     if (!BaseText.empty()) {
         NewTypeStr = (llvm::Twine("decltype(") + BaseText + ")::size_type").str();
         DecltypeStrategySuccess = true;
     }
  }

  // Fallback / Standard Strategy
  if (!DecltypeStrategySuccess) {
      QualType T = NewType.getUnqualifiedType();
      
      // Check if T is a typedef defined inside a class/struct (nested).
      // If so, fall back to canonical to avoid invalid scope names like 'size_type'
      // appearing in main() without qualification.
      bool IsNestedTypedef = false;
      if (const auto *TT = T->getAs<TypedefType>()) {
          if (const auto *Decl = TT->getDecl()) {
              if (llvm::isa<RecordDecl>(Decl->getDeclContext())) {
                  IsNestedTypedef = true;
              }
          }
      }

      if (IsNestedTypedef) {
          NewTypeStr = T.getCanonicalType().getAsString(Ctx->getPrintingPolicy());
      } else {
          NewTypeStr = T.getAsString(Ctx->getPrintingPolicy());
      }
  }

  // Check if the source range is valid before attempting replacement
  if (TL.getSourceRange().isValid()) {
    SourceManager &SM = Rewriter.getSourceMgr();
    SourceLocation Start = TL.getBeginLoc();

    if (SM.isWrittenInMainFile(Start)) {
      Rewriter.ReplaceText(TL.getSourceRange(), NewTypeStr);
    }
  }
}

void TypeCorrectMatcher::onEndOfTranslationUnit() {
  SourceManager &SM = Rewriter.getSourceMgr();
  FileID MainFileID = SM.getMainFileID();

  if (const llvm::RewriteBuffer *Buffer =
          Rewriter.getRewriteBufferFor(MainFileID)) {
    Buffer->write(llvm::outs());
  } else {
    const llvm::MemoryBufferRef MainBuf = SM.getBufferOrFake(MainFileID);
    llvm::outs() << MainBuf.getBuffer();
  }
  llvm::outs().flush();
}

//-----------------------------------------------------------------------------
// ASTConsumer - Implementation
//-----------------------------------------------------------------------------

TypeCorrectASTConsumer::TypeCorrectASTConsumer(clang::Rewriter &R, bool UseDecltype, bool ExpandAuto)
    : Handler(R, UseDecltype, ExpandAuto) {

  //---------------------------------------------------------------------------
  // Matcher 1: Variable Declaration initialized by a CallExpr
  // Matches:   int x = foo();
  // 
  // EXCLUSION: We verify that the Variable is NOT a loop variable declared in 
  // a ForStmt (e.g. for(int i=call()...)). Those are handled by Strategy B 
  // below, and double matching causes corrupted replacement text.
  //---------------------------------------------------------------------------
  
  auto IsLoopInitVar = hasParent(declStmt(hasParent(forStmt())));

  Finder.addMatcher(
      varDecl(
          hasInitializer(ignoringParenImpCasts(callExpr().bind("bound_var_init"))),
          unless(parmVarDecl()),
          unless(IsLoopInitVar)
          )
          .bind("bound_var_decl"),
      &Handler);

  //---------------------------------------------------------------------------
  // Matcher 2: Function Return mismatches
  // Matches:   int f() { return x; }
  //---------------------------------------------------------------------------
  Finder.addMatcher(
      functionDecl(isDefinition(),
                   hasBody(forEachDescendant(
                       returnStmt(hasReturnValue(
                                      ignoringParenImpCasts(
                                          expr().bind("bound_ret_val"))))
                           .bind("bound_ret_stmt"))))
          .bind("bound_func_decl"),
      &Handler);

  //---------------------------------------------------------------------------
  // Matcher 3 & 4: For Loop Iterator mismatches
  //---------------------------------------------------------------------------

  // --- Shared Definition: The Type Source (Call Expression) ---
  auto MemberCallM = cxxMemberCallExpr(
      on(ignoringParenImpCasts(expr().bind("bound_call_instance")))
  ).bind("bound_loop_limit");

  auto FreeCallM = callExpr(unless(cxxMemberCallExpr())).bind("bound_loop_limit");
  auto TypeSourceCallM = anyOf(MemberCallM, FreeCallM);

  // --- Shared Definition: The Loop Variable ---
  auto LoopVarM = varDecl(hasType(isInteger())).bind("bound_loop_var");

  // --- Strategy A: Condition-based Mismatch ---
  auto LoopCondM = binaryOperator(
      hasOperatorName("<"),
      hasEitherOperand(ignoringParenImpCasts(declRefExpr(to(LoopVarM)))),
      hasEitherOperand(ignoringParenImpCasts(TypeSourceCallM)));

  Finder.addMatcher(forStmt(hasLoopInit(declStmt(hasSingleDecl(LoopVarM))),
                            hasCondition(LoopCondM))
                        .bind("bound_for_loop"),
                    &Handler);

  // --- Strategy B: Initialization-based Mismatch ---
  Finder.addMatcher(
      forStmt(
          hasLoopInit(declStmt(hasSingleDecl(
             varDecl(
                 hasType(isInteger()),
                 hasInitializer(ignoringParenImpCasts(TypeSourceCallM))
             ).bind("bound_loop_var")
          )))
      ).bind("bound_for_loop"),
      &Handler);
}

//-----------------------------------------------------------------------------
// Plugin Action
//-----------------------------------------------------------------------------

/**
 * @class TCPluginAction
 * @brief Definition of the clang plugin entry point.
 */
class TCPluginAction : public clang::PluginASTAction {
public:
  bool UseDecltype = false;
  bool ExpandAuto = false;

  /**
   * @brief Parses command line arguments passed to the plugin.
   *
   * Arguments:
   *  -use-decltype : Prefer 'decltype(x)::size_type' over canonical types.
   *  -expand-auto  : Force rewrite of 'auto' even if initialized by function call.
   */
  bool ParseArgs(const clang::CompilerInstance &CI,
                 const std::vector<std::string> &args) override {
    for (const auto &arg : args) {
        if (arg == "-use-decltype") {
            UseDecltype = true;
        } else if (arg == "-expand-auto") {
            ExpandAuto = true;
        }
    }
    return true;
  }

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI,
                    llvm::StringRef file) override {
    RewriterForTC.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<TypeCorrectASTConsumer>(RewriterForTC, UseDecltype, ExpandAuto);
  }

private:
  clang::Rewriter RewriterForTC;
};

// Register the plugin
static clang::FrontendPluginRegistry::Add<TCPluginAction>
    X(/*Name=*/"TypeCorrect",
      /*Desc=*/"Type Correction Plugin");