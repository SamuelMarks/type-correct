/** 
 * @file TypeCorrect.cpp
 * @brief Implementation of TypeCorrect logic. 
 * 
 * Implements the AST Matchers to find type mismatches and the Rewriter logic
 * to physically replace the incorrect types in the source buffer. 
 * 
 * @author Samuel Marks
 * @license CC0
 */ 

#include <optional>

#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/FileManager.h>
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

TypeCorrectMatcher::TypeCorrectMatcher(clang::Rewriter &Rewriter) 
    : Rewriter(Rewriter) {} 

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

      // Compare canonical types to ignore typedef sugar (e.g. size_t vs unsigned long) 
      if (VarType.getCanonicalType() != InitType.getCanonicalType()) { 
          ResolveType(Var->getTypeSourceInfo()->getTypeLoc(), InitType, Ctx); 
      } 
    } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 2: Function Return Type Mismatch
  // Pattern: RetType function() { return val; } 
  // Example: int f() { return long_val; } 
  //--------------------------------------------------------------------------- 
  else if (const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("bound_func_decl")) { 
    const auto *RetStmtNode = Result.Nodes.getNodeAs<ReturnStmt>("bound_ret_stmt"); 
    const auto *RetVal = Result.Nodes.getNodeAs<Expr>("bound_ret_val"); 

    if (Func && RetStmtNode && RetVal) { 
      QualType DeclaredRetType = Func->getReturnType(); 
      QualType ActualRetType = RetVal->getType(); 

      if (DeclaredRetType.getCanonicalType() != ActualRetType.getCanonicalType()) { 
          // For functions, the TypeLoc is a FunctionTypeLoc. We need the Return Loc. 
          TypeLoc TL = Func->getTypeSourceInfo()->getTypeLoc(); 
          if (auto FTL = TL.getAs<FunctionTypeLoc>()) { 
               ResolveType(FTL.getReturnLoc(), ActualRetType, Ctx); 
          } 
      } 
    } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 3: For Loop Mismatch
  // Pattern: for(int i=0; i < strlen(...); i++) 
  //--------------------------------------------------------------------------- 
  else if (const auto *Loop = Result.Nodes.getNodeAs<ForStmt>("bound_for_loop")) { 
    const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("bound_loop_var"); 
    const auto *LoopLimit = Result.Nodes.getNodeAs<CallExpr>("bound_loop_limit"); 

    if (Loop && LoopVar && LoopLimit) { 
      QualType VarType = LoopVar->getType(); 
      QualType LimitType = LoopLimit->getCallReturnType(*Ctx); 

      if (VarType.getCanonicalType() != LimitType.getCanonicalType()) { 
        ResolveType(LoopVar->getTypeSourceInfo()->getTypeLoc(), LimitType, Ctx); 
      } 
    } 
  } 
} 

void TypeCorrectMatcher::ResolveType(const TypeLoc &OldLoc, 
                                     const QualType &NewType, 
                                     ASTContext *Ctx) { 
    // We need to drill down to the unqualified type locator. 
    // E.g., if we have "const int", OldLoc is a QualifiedTypeLoc. 
    // If we replace "const int" with "size_t", we lose const. 
    // Typically we want to find the "int" part and replace it with "size_t", 
    // resulting in "const size_t". 
    
    TypeLoc TL = OldLoc; 
    while(true) { 
        if (auto QTL = TL.getAs<QualifiedTypeLoc>()) { 
            TL = QTL.getUnqualifiedLoc(); 
        } else { 
            break; 
        } 
    } 
    
    // Determine the string representation of the new type. 
    // We use getUnqualifiedType() because we preserved qualifiers (const/volatile) 
    // by drilling down in the loop above. 
    std::string NewTypeStr = NewType.getUnqualifiedType().getAsString(Ctx->getPrintingPolicy()); 

    // Check if the source range is valid before attempting replacement
    if (TL.getSourceRange().isValid()) { 
        // SourceManager and FileEntryRef usage: 
        // Ensure the location is in the main file and writable. 
        SourceManager &SM = Rewriter.getSourceMgr(); 
        SourceLocation Start = TL.getBeginLoc(); 
        
        // In LLVM 16+, FileID operations are stricter, but SourceLocation based
        // checks usually abstract the FileEntryRef details. 
        if (SM.isWrittenInMainFile(Start)) { 
             Rewriter.ReplaceText(TL.getSourceRange(), NewTypeStr); 
        } 
    } 
} 

void TypeCorrectMatcher::onEndOfTranslationUnit() { 
  // LLVM 16+ API Modernization: 
  // getMainFileID() returns a FileID directly in most versions, but interacting with result
  // buffers requires care. 
  SourceManager &SM = Rewriter.getSourceMgr(); 
  FileID MainFileID = SM.getMainFileID(); 
  
  // We use the Rewriter's getEditBuffer. If no changes were made to the main file, 
  // this might create an empty buffer or return an iterator end. 
  // We check via getRewriteBufferFor to be safe, though getEditBuffer will lazily create one. 
  if (const llvm::RewriteBuffer *Buffer = Rewriter.getRewriteBufferFor(MainFileID)) { 
      Buffer->write(llvm::outs()); 
  } else { 
      // If no changes were made, we might want to output the original file 
      // or nothing depending on the tool's contract. 
      // For this plugin, expecting output of transformed code: 
      const llvm::MemoryBufferRef MainBuf = SM.getBufferOrFake(MainFileID); 
      llvm::outs() << MainBuf.getBuffer(); 
  } 
  // Ensure the output stream is flushed to the FD for capture
  llvm::outs().flush();
} 

//----------------------------------------------------------------------------- 
// ASTConsumer - Implementation
//----------------------------------------------------------------------------- 

TypeCorrectASTConsumer::TypeCorrectASTConsumer(clang::Rewriter &R) 
    : Handler(R) { 

  //--------------------------------------------------------------------------- 
  // Matcher 1: Variable Declaration initialized by a CallExpr
  // Matches:   int x = foo(); 
  // Binds:     "bound_var_decl" (VarDecl), "bound_var_init" (CallExpr) 
  //--------------------------------------------------------------------------- 
  Finder.addMatcher( 
      varDecl( 
          hasInitializer(ignoringParenImpCasts(callExpr().bind("bound_var_init"))), 
          unless(parmVarDecl()) // Ignore function parameters
      ).bind("bound_var_decl"), 
      &Handler
  ); 

  //--------------------------------------------------------------------------- 
  // Matcher 2: Function Return mismatches
  // Matches:   int f() { return x; } 
  // Binds:     "bound_func_decl" (FunctionDecl), 
  //            "bound_ret_stmt" (ReturnStmt), 
  //            "bound_ret_val" (Expr) 
  //--------------------------------------------------------------------------- 
  Finder.addMatcher( 
      functionDecl( 
          isDefinition(), 
          hasBody( 
              forEachDescendant( 
                returnStmt(hasReturnValue(
                    // FIXED: Ignore implicit casts (e.g. integer promotion/demotion)
                    // binding to the underlying expression (long b) instead of the 
                    // cast expression (int).
                    ignoringParenImpCasts(expr().bind("bound_ret_val"))
                )) 
                .bind("bound_ret_stmt") 
              ) 
          ) 
      ).bind("bound_func_decl"), 
      &Handler
  ); 

  //--------------------------------------------------------------------------- 
  // Matcher 3: For Loop Iterator mismatches
  // Matches:   for(int i=0; i < foo(); i++) 
  // Binds:     "bound_for_loop" (ForStmt), 
  //            "bound_loop_var" (VarDecl), 
  //            "bound_loop_limit" (CallExpr) 
  //--------------------------------------------------------------------------- 
  // 1. Identify the Loop Variable (e.g., int i) 
  auto LoopVarM = varDecl(hasType(isInteger())).bind("bound_loop_var"); 

  // 2. Identify the Limit Function (e.g., strlen()) 
  auto LoopLimitM = callExpr().bind("bound_loop_limit"); 

  // 3. Identify the Condition (e.g., i < strlen()) 
  auto LoopCondM = binaryOperator( 
        hasOperatorName("<"), 
        hasEitherOperand(ignoringParenImpCasts(declRefExpr(to(LoopVarM)))), 
        hasEitherOperand(ignoringParenImpCasts(LoopLimitM)) 
  ); 

  Finder.addMatcher( 
      forStmt( 
          hasLoopInit(declStmt(hasSingleDecl(LoopVarM))), 
          hasCondition(LoopCondM) 
      ).bind("bound_for_loop"), 
      &Handler
  ); 
} 

//----------------------------------------------------------------------------- 
// Plugin Action
//----------------------------------------------------------------------------- 

/** 
 * @class TCPluginAction
 * @brief Definition of the clang plugin entry point. 
 * 
 * Scans arguments and creates the ASTConsumer. 
 */ 
class TCPluginAction : public clang::PluginASTAction { 
public: 
  /** 
   * @brief Parses command line arguments passed to the plugin. 
   * 
   * @param CI CompilerInstance context. 
   * @param args Command line arguments. 
   * @return true Always returns true (arguments ignored). 
   */ 
  bool ParseArgs(const clang::CompilerInstance &CI, 
                 const std::vector<std::string> &args) override { 
    return true; 
  } 

  /** 
   * @brief Factory method for the ASTConsumer. 
   * 
   * Initializes the Rewriter with the compiler's source manager. 
   * 
   * @param CI CompilerInstance context. 
   * @param file Input file path. 
   * @return std::unique_ptr<clang::ASTConsumer> The correctly configured consumer. 
   */ 
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI, 
                    llvm::StringRef file) override { 
    RewriterForTC.setSourceMgr(CI.getSourceManager(), CI.getLangOpts()); 
    return std::make_unique<TypeCorrectASTConsumer>(RewriterForTC); 
  } 

private: 
  /** 
   * @brief The rewriter instance maintained for the lifespan of the action. 
   */ 
  clang::Rewriter RewriterForTC; 
}; 

// Register the plugin
static clang::FrontendPluginRegistry::Add<TCPluginAction>
    X(/*Name=*/"TypeCorrect", 
      /*Desc=*/"Type Correction Plugin");