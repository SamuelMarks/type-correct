/** 
 * @file TypeCorrect.cpp
 * @brief Implementation of TypeCorrect logic including Argument Passing Matchers. 
 * 
 * This implementation includes strategies for identifying implicit casts when
 * arguments are passed to functions (like stdlib's memset/memcpy). 
 * It attempts to trace the argument back to the variable, reads the expected parameter
 * type from the function signature, and uses the "Widest Type" logic to update
 * the variable definition. 
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

using namespace clang; 
using namespace clang::ast_matchers; 

//----------------------------------------------------------------------------- 
// TypeCorrectMatcher - Helpers
//----------------------------------------------------------------------------- 

TypeCorrectMatcher::TypeCorrectMatcher(clang::Rewriter &Rewriter, bool UseDecltype, bool ExpandAuto) 
    : Rewriter(Rewriter), UseDecltype(UseDecltype), ExpandAuto(ExpandAuto) {} 

bool TypeCorrectMatcher::IsModifiable(SourceLocation DeclLoc, const SourceManager &SM) { 
    if (DeclLoc.isInvalid()) return false; 
    if (DeclLoc.isMacroID()) return false; 
    return SM.isWrittenInMainFile(DeclLoc); 
} 

bool TypeCorrectMatcher::IsWiderType(QualType Current, QualType Candidate, ASTContext *Ctx) { 
    if (Current.isNull()) return true; 

    QualType CanCurr = Current.getCanonicalType(); 
    QualType CanCand = Candidate.getCanonicalType(); 

    uint64_t SizeCurr = Ctx->getTypeSize(CanCurr); 
    uint64_t SizeCand = Ctx->getTypeSize(CanCand); 

    // Prefer wider types
    if (SizeCand > SizeCurr) return true; 

    // If same size, prefer Unsigned (e.g. size_t) over Signed (int) 
    if (SizeCand == SizeCurr) { 
        if (CanCand->isUnsignedIntegerType() && CanCurr->isSignedIntegerType()) { 
            return true; 
        } 
    } 
    return false; 
} 

void TypeCorrectMatcher::RegisterUpdate(const NamedDecl* Decl, 
                                        QualType CandidateType, 
                                        const Expr* BaseExpr, 
                                        ASTContext* Ctx) { 
    QualType CurrentBestType; 

    if (WidestTypeMap.find(Decl) != WidestTypeMap.end()) { 
        CurrentBestType = WidestTypeMap[Decl].Type; 
    } else { 
        if (const auto *V = dyn_cast<ValueDecl>(Decl)) { 
            CurrentBestType = V->getType(); 
        } 
    } 

    if (IsWiderType(CurrentBestType, CandidateType, Ctx)) { 
        WidestTypeState State; 
        State.Type = CandidateType; 
        State.BaseExpr = BaseExpr; 
        WidestTypeMap[Decl] = State; 
    } 
} 

//----------------------------------------------------------------------------- 
// TypeCorrectMatcher - Main Logic
//----------------------------------------------------------------------------- 

void TypeCorrectMatcher::run(const MatchFinder::MatchResult &Result) { 
  ASTContext *Ctx = Result.Context; 
  SourceManager &SM = Rewriter.getSourceMgr(); 

  //--------------------------------------------------------------------------- 
  // Case 1: Variable Initialization
  //--------------------------------------------------------------------------- 
  if (const auto *Var = Result.Nodes.getNodeAs<VarDecl>("bound_var_decl")) { 
    const auto *InitCall = Result.Nodes.getNodeAs<CallExpr>("bound_var_init"); 

    if (Var && InitCall) { 
        QualType InitType = InitCall->getCallReturnType(*Ctx); 
        if (IsModifiable(Var->getLocation(), SM)) { 
            RegisterUpdate(Var, InitType, nullptr, Ctx); 
        } else { 
             QualType VarType = Var->getType(); 
             if (VarType.getCanonicalType() != InitType.getCanonicalType()) { 
                 InjectCast(InitCall, VarType, Ctx); 
             } 
        } 
    } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 2: Function Return Type
  //--------------------------------------------------------------------------- 
  else if (const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("bound_func_decl")) { 
    const auto *RetVal = Result.Nodes.getNodeAs<Expr>("bound_ret_val"); 
    if (Func && RetVal) { 
        QualType ActualRetType = RetVal->getType(); 
        if (IsModifiable(Func->getLocation(), SM)) { 
             RegisterUpdate(Func, ActualRetType, nullptr, Ctx); 
        } else { 
             QualType DeclaredRetType = Func->getReturnType(); 
             if (DeclaredRetType.getCanonicalType() != ActualRetType.getCanonicalType()) { 
                 InjectCast(RetVal, DeclaredRetType, Ctx); 
             } 
        } 
    } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 3: For Loops
  //--------------------------------------------------------------------------- 
  else if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("bound_loop_var")) { 
      const auto *LoopLimit = Result.Nodes.getNodeAs<CallExpr>("bound_loop_limit"); 
      const auto *LoopInstance = Result.Nodes.getNodeAs<Expr>("bound_call_instance"); 

      if (LoopVar && LoopLimit) { 
          QualType LimitType = LoopLimit->getCallReturnType(*Ctx); 
          if (IsModifiable(LoopVar->getLocation(), SM)) { 
              RegisterUpdate(LoopVar, LimitType, LoopInstance, Ctx); 
          } 
      } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 4: Assignment (Var/Field) 
  //--------------------------------------------------------------------------- 
  else if (const auto *AssignVar = Result.Nodes.getNodeAs<VarDecl>("bound_assign_var")) { 
      const auto *AssignCall = Result.Nodes.getNodeAs<CallExpr>("bound_assign_call"); 

      if (AssignVar && AssignCall) { 
          QualType CallType = AssignCall->getCallReturnType(*Ctx); 
          if (IsModifiable(AssignVar->getLocation(), SM)) { 
              RegisterUpdate(AssignVar, CallType, nullptr, Ctx); 
          } else { 
              QualType VarType = AssignVar->getType(); 
              if (VarType.getCanonicalType() != CallType.getCanonicalType()) { 
                   InjectCast(AssignCall, VarType, Ctx); 
              } 
          } 
      } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 5: Argument Passing (Standard Library implicit casts) 
  // Pattern: func(arg) where arg resolves to a variable (possibly via casts).
  //--------------------------------------------------------------------------- 
  else if (const auto* ArgVar = Result.Nodes.getNodeAs<VarDecl>("bound_arg_var")) { 
      const auto* ParamDecl = Result.Nodes.getNodeAs<ParmVarDecl>("bound_param_decl"); 
      // Matched as generic Expr to be robust against various AST cast hierarchies
      const auto* ArgExpr = Result.Nodes.getNodeAs<Expr>("bound_arg_expr");

      if (ArgVar && ParamDecl && ArgExpr) { 
          QualType ParamType = ParamDecl->getType(); 
          
          if (IsModifiable(ArgVar->getLocation(), SM)) { 
              // Register that ArgVar really ought to be ParamType (e.g. size_t). 
              // IsWiderType logic in RegisterUpdate handles filtering.
              RegisterUpdate(ArgVar, ParamType, nullptr, Ctx); 
          } else { 
              // Fallback: If we can't change the variable definition, check if we need to cast.
              QualType VarType = ArgVar->getType();
              // Only inject cast if types strictly differ
              if (VarType.getCanonicalType() != ParamType.getCanonicalType()) { 
                  InjectCast(ArgExpr, ParamType, Ctx); 
              }
          } 
      } 
  } 
} 

void TypeCorrectMatcher::onEndOfTranslationUnit(ASTContext &Ctx) { 
    // Phase 1: Apply gathered updates from the map
    for (const auto &Pair : WidestTypeMap) { 
        const NamedDecl *Decl = Pair.first; 
        const WidestTypeState &State = Pair.second; 

        const VarDecl *V = dyn_cast<VarDecl>(Decl); 
        const FunctionDecl *F = dyn_cast<FunctionDecl>(Decl); 

        if (V && V->getTypeSourceInfo()) { 
             if (V->getType().getCanonicalType() != State.Type.getCanonicalType()) { 
                ResolveType(V->getTypeSourceInfo()->getTypeLoc(), State.Type, &Ctx, V, State.BaseExpr); 
             } 
        } 
        else if (F && F->getTypeSourceInfo()) { 
             QualType Ret = F->getReturnType(); 
             if (Ret.getCanonicalType() != State.Type.getCanonicalType()) { 
                 TypeLoc TL = F->getTypeSourceInfo()->getTypeLoc(); 
                 if (auto FTL = TL.getAs<FunctionTypeLoc>()) { 
                     ResolveType(FTL.getReturnLoc(), State.Type, &Ctx, nullptr, State.BaseExpr); 
                 } 
             } 
        } 
    } 

    // Phase 2: Flush Buffer
    // Note: This must run even if WidestTypeMap is empty to ensure output is generated. 
    SourceManager &SM = Rewriter.getSourceMgr(); 
    FileID MainFileID = SM.getMainFileID(); 

    if (const llvm::RewriteBuffer *Buffer = Rewriter.getRewriteBufferFor(MainFileID)) { 
        Buffer->write(llvm::outs()); 
    } else { 
        const llvm::MemoryBufferRef MainBuf = SM.getBufferOrFake(MainFileID); 
        llvm::outs() << MainBuf.getBuffer(); 
    } 
    llvm::outs().flush(); 
} 

void TypeCorrectMatcher::InjectCast(const Expr *ExprToCast, 
                                    const QualType &TargetType, 
                                    ASTContext *Ctx) { 
    if (!ExprToCast) return; 
    std::string TypeName = TargetType.getAsString(Ctx->getPrintingPolicy()); 
    std::string CastPrefix = Ctx->getLangOpts().CPlusPlus
                                 ? ("static_cast<" + TypeName + ">(") 
                                 : ("(" + TypeName + ")("); 
    std::string CastSuffix = ")"; 

    SourceRange SR = ExprToCast->getSourceRange(); 
    if (SR.isValid()) { 
         Rewriter.InsertTextBefore(SR.getBegin(), CastPrefix); 
         Rewriter.InsertTextAfterToken(SR.getEnd(), CastSuffix); 
    } 
} 

void TypeCorrectMatcher::ResolveType(const TypeLoc &OldLoc, 
                                     const QualType &NewType, 
                                     ASTContext *Ctx, 
                                     const VarDecl *BoundVar, 
                                     const Expr *BaseExpr) { 
  TypeLoc TL = OldLoc; 
  while (true) { 
    if (auto QTL = TL.getAs<QualifiedTypeLoc>()) TL = QTL.getUnqualifiedLoc(); 
    else break; 
  } 

  if (auto ATL = TL.getAs<AutoTypeLoc>()) { 
      if (!ExpandAuto && BoundVar && BoundVar->getInit()) { 
          const Expr *Init = BoundVar->getInit()->IgnoreParenImpCasts(); 
          if (isa<CallExpr>(Init) || isa<CXXMemberCallExpr>(Init)) return; 
      } 
  } 

  std::string NewTypeStr; 
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

  if (!DecltypeStrategySuccess) { 
      QualType T = NewType.getUnqualifiedType(); 
      bool IsNestedTypedef = false; 
      if (const auto *TT = T->getAs<TypedefType>()) { 
          if (const auto *Decl = TT->getDecl()) { 
              if (isa<RecordDecl>(Decl->getDeclContext())) IsNestedTypedef = true; 
          } 
      } 
      NewTypeStr = IsNestedTypedef
                       ? T.getCanonicalType().getAsString(Ctx->getPrintingPolicy()) 
                       : T.getAsString(Ctx->getPrintingPolicy()); 
  } 

  if (TL.getSourceRange().isValid()) { 
    if (IsModifiable(TL.getBeginLoc(), Rewriter.getSourceMgr())) { 
        Rewriter.ReplaceText(TL.getSourceRange(), NewTypeStr); 
    } 
  } 
} 

//----------------------------------------------------------------------------- 
// ASTConsumer & Registration
//----------------------------------------------------------------------------- 

TypeCorrectASTConsumer::TypeCorrectASTConsumer(clang::Rewriter &R, bool UseDecltype, bool ExpandAuto) 
    : Handler(R, UseDecltype, ExpandAuto) { 

  // 1. Variable Init
  auto IsLoopInitVar = hasParent(declStmt(hasParent(forStmt()))); 
  Finder.addMatcher( 
      varDecl( 
          hasInitializer(ignoringParenImpCasts(callExpr().bind("bound_var_init"))), 
          unless(parmVarDecl()), unless(IsLoopInitVar)).bind("bound_var_decl"), 
      &Handler); 

  // 2. Return Type
  Finder.addMatcher( 
      functionDecl(isDefinition(), 
                   hasBody(forEachDescendant( 
                       returnStmt(hasReturnValue(ignoringParenImpCasts(expr().bind("bound_ret_val")))) 
                           .bind("bound_ret_stmt")))) 
          .bind("bound_func_decl"), 
      &Handler); 

  // 3. For Loops
  auto MemberCallM = cxxMemberCallExpr(on(ignoringParenImpCasts(expr().bind("bound_call_instance")))).bind("bound_loop_limit"); 
  auto FreeCallM = callExpr(unless(cxxMemberCallExpr())).bind("bound_loop_limit"); 
  auto TypeSourceCallM = anyOf(MemberCallM, FreeCallM); 
  auto LoopVarM = varDecl(hasType(isInteger())).bind("bound_loop_var"); 

  auto LoopCondM = binaryOperator(hasOperatorName("<"), 
      hasEitherOperand(ignoringParenImpCasts(declRefExpr(to(LoopVarM)))), 
      hasEitherOperand(ignoringParenImpCasts(TypeSourceCallM))); 

  Finder.addMatcher(forStmt(hasLoopInit(declStmt(hasSingleDecl(LoopVarM))), hasCondition(LoopCondM)).bind("bound_for_loop"), &Handler); 
  Finder.addMatcher(forStmt(hasLoopInit(declStmt(hasSingleDecl(varDecl(hasType(isInteger()), hasInitializer(ignoringParenImpCasts(TypeSourceCallM))).bind("bound_loop_var"))))).bind("bound_for_loop"), &Handler); 

  // 4. Assignments
  Finder.addMatcher( 
      binaryOperator(hasOperatorName("="), 
          hasLHS(ignoringParenImpCasts(anyOf(declRefExpr(to(varDecl().bind("bound_assign_var"))), memberExpr(member(fieldDecl().bind("bound_assign_field")))))), 
          hasRHS(ignoringParenImpCasts(callExpr().bind("bound_assign_call")))) 
      .bind("bound_assign_op"), 
      &Handler); 

  // 5. Argument Passing
  // Matches: callExpr(argument(implicitCast(declRef(var)))) 
  // Uses 'forEachArgumentWithParam' to correlate argument to the parameter definition. 
  // Note: We use ignoringParenImpCasts matching against the expression to find
  // the variable reference, and we rely on run() to check type mismatch/widening.
  Finder.addMatcher( 
      callExpr( 
          forEachArgumentWithParam( 
            expr(ignoringParenImpCasts(declRefExpr(to(varDecl().bind("bound_arg_var")))))
                .bind("bound_arg_expr"), 
            parmVarDecl().bind("bound_param_decl") 
          ) 
      ).bind("bound_arg_call"), 
      &Handler); 
} 

//----------------------------------------------------------------------------- 
// Plugin Action
//----------------------------------------------------------------------------- 

class TCPluginAction : public clang::PluginASTAction { 
public: 
  bool UseDecltype = false; 
  bool ExpandAuto = false; 

  bool ParseArgs(const clang::CompilerInstance &CI, const std::vector<std::string> &args) override { 
    for (const auto &arg : args) { 
        if (arg == "-use-decltype") UseDecltype = true; 
        else if (arg == "-expand-auto") ExpandAuto = true; 
    } 
    return true; 
  } 

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef file) override { 
    RewriterForTC.setSourceMgr(CI.getSourceManager(), CI.getLangOpts()); 
    return std::make_unique<TypeCorrectASTConsumer>(RewriterForTC, UseDecltype, ExpandAuto); 
  } 
private: 
  clang::Rewriter RewriterForTC; 
}; 

static clang::FrontendPluginRegistry::Add<TCPluginAction> X("TypeCorrect", "Type Correction Plugin");