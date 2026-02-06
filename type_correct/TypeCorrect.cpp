/** 
 * @file TypeCorrect.cpp
 * @brief Implementation of TypeCorrect logic with Macro Analysis support. 
 * 
 * Includes logic for: 
 * - Macro expansion tracing (Mapping MacroID -> SpellingLoc). 
 * - Macro definition rewriting. 
 * - Template recursion. 
 * - Constraint Solver integration. 
 * 
 * @author SamuelMarks
 * @license CC0
 */ 

#include <optional>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/TypeLoc.h>
#include <clang/Index/USRGeneration.h> 
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendPluginRegistry.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Regex.h>

#include "TypeCorrect.h" 

using namespace clang; 
using namespace clang::ast_matchers; 

//----------------------------------------------------------------------------- 
// Constructor
//----------------------------------------------------------------------------- 

TypeCorrectMatcher::TypeCorrectMatcher(clang::Rewriter &Rewriter, 
                                       bool UseDecltype, 
                                       bool ExpandAuto, 
                                       std::string ProjectRoot, 
                                       std::string ExcludePattern, 
                                       bool InPlace, 
                                       bool EnableAbiBreakingChanges, 
                                       type_correct::Phase CurrentPhase, 
                                       std::string FactsOutputDir) 
    : Rewriter(Rewriter), UseDecltype(UseDecltype), ExpandAuto(ExpandAuto), 
      ProjectRoot(std::move(ProjectRoot)), ExcludePattern(std::move(ExcludePattern)), 
      InPlace(InPlace), StructEngine(EnableAbiBreakingChanges), 
      CurrentPhase(CurrentPhase), FactsOutputDir(std::move(FactsOutputDir)) { 
      if (CurrentPhase == type_correct::Phase::Apply) { 
          EnsureGlobalFactsLoaded(); 
      } 
} 

void TypeCorrectMatcher::EnsureGlobalFactsLoaded() { 
    if (FactsOutputDir.empty()) return; 
    if (!GlobalFacts.empty()) return; 

    std::vector<type_correct::ctu::SymbolFact> Raw; 
    std::string GlobalFile = FactsOutputDir + "/global.facts"; 
    if (type_correct::ctu::FactManager::ReadFacts(GlobalFile, Raw)) { 
        for(const auto& F : Raw) GlobalFacts[F.USR] = F; 
    } 
} 

//----------------------------------------------------------------------------- 
// Recursive Type Rewriting
//----------------------------------------------------------------------------- 

bool TypeCorrectMatcher::RecursivelyRewriteType(TypeLoc TL, QualType TargetType, 
                                                ASTContext *Ctx, const Expr *BaseExpr) { 
    if (TL.isNull() || TargetType.isNull()) return false; 
    
    // Check Macro Logic First
    if (TL.getBeginLoc().isMacroID()) { 
        RewriteMacroType(TL.getBeginLoc(), TargetType, Ctx, BaseExpr); 
        return true; 
    } 

    if (Ctx->hasSameType(TL.getType(), TargetType)) return false; 

    if (auto QTL = TL.getAs<QualifiedTypeLoc>()) { 
        return RecursivelyRewriteType(QTL.getUnqualifiedLoc(), TargetType.getUnqualifiedType(), Ctx, BaseExpr); 
    } 

    if (auto TSTL = TL.getAs<TemplateSpecializationTypeLoc>()) { 
        const auto *TargetTST = TargetType->getAs<TemplateSpecializationType>(); 
        if (!TargetTST) return false; 
        
        // Safety: Ensure templates match before attempting arg rewrite
        if (!TSTL.getTypePtr()->getTemplateName().getAsTemplateDecl() || 
             !TargetTST->getTemplateName().getAsTemplateDecl() || 
             TSTL.getTypePtr()->getTemplateName().getAsTemplateDecl() != 
             TargetTST->getTemplateName().getAsTemplateDecl()) { 
            return false; 
        } 

        bool ChangedAny = false; 
        unsigned NumArgs = std::min(TSTL.getNumArgs(), (unsigned)TargetTST->template_arguments().size()); 

        for (unsigned i = 0; i < NumArgs; ++i) { 
            TemplateArgumentLoc ArgLoc = TSTL.getArgLoc(i); 
            const TemplateArgument &TargetArg = TargetTST->template_arguments()[i]; 

            if (ArgLoc.getArgument().getKind() == TemplateArgument::Type && 
                TargetArg.getKind() == TemplateArgument::Type) { 
                
                // Safety check for TypeSourceInfo to prevent SegFault
                if (TypeSourceInfo *TSI = ArgLoc.getTypeSourceInfo()) { 
                    bool Changed = RecursivelyRewriteType(TSI->getTypeLoc(), 
                                                          TargetArg.getAsType(), Ctx, BaseExpr); 
                    if (Changed) ChangedAny = true; 
                } 
            } 
        } 
        return ChangedAny; 
    } 

    if (auto ETL = TL.getAs<ElaboratedTypeLoc>()) { 
        return RecursivelyRewriteType(ETL.getNamedTypeLoc(), TargetType, Ctx, BaseExpr); 
    } 

    if (!IsModifiable(TL.getBeginLoc(), Rewriter.getSourceMgr())) return false; 
    
    std::string NewTypeStr; 
    bool UsingDecltype = false; 

    if (UseDecltype && BaseExpr) { 
        QualType ExprType = BaseExpr->getType().getCanonicalType(); 
        QualType TargetCan = TargetType.getCanonicalType(); 
        if (Ctx->hasSameType(ExprType, TargetCan)) { 
             SourceManager &SM = Rewriter.getSourceMgr(); 
             CharSourceRange Range = CharSourceRange::getTokenRange(BaseExpr->getSourceRange()); 
             StringRef BaseText = Lexer::getSourceText(Range, SM, Ctx->getLangOpts()); 
             if (!BaseText.empty()) { 
                 NewTypeStr = "decltype(" + BaseText.str() + ")"; 
                 UsingDecltype = true; 
             } 
        } 
    } 

    if (!UsingDecltype) { 
        PrintingPolicy Policy = Ctx->getPrintingPolicy(); 
        Policy.SuppressScope = false; 
        Policy.FullyQualifiedName = true; 
        NewTypeStr = TargetType.getAsString(Policy); 
    } 
    
    Rewriter.ReplaceText(TL.getSourceRange(), NewTypeStr); 
    return true; 
} 

//----------------------------------------------------------------------------- 
// Macro Rewriting Logic
//----------------------------------------------------------------------------- 

void TypeCorrectMatcher::RewriteMacroType(SourceLocation Loc, const QualType &NewType, 
                                          ASTContext *Ctx, const Expr *BaseExpr) { 
    if (!Loc.isMacroID()) return; 

    SourceManager &SM = Rewriter.getSourceMgr(); 
    SourceLocation SpellingLoc = SM.getSpellingLoc(Loc); 
    
    if (!IsModifiable(SpellingLoc, SM)) return; 

    PrintingPolicy Policy = Ctx->getPrintingPolicy(); 
    Policy.SuppressScope = false; 
    Policy.FullyQualifiedName = true; 
    std::string NewTypeStr = NewType.getAsString(Policy); 

    unsigned TokenLength = 0; 
    std::pair<FileID, unsigned> Decomposed = SM.getDecomposedLoc(SpellingLoc); 
    bool Invalid = false; 
    StringRef Buffer = SM.getBufferData(Decomposed.first, &Invalid); 
    
    if (!Invalid) { 
        const char *TokenStart = Buffer.data() + Decomposed.second; 
        Lexer RawLexer(SM.getLocForStartOfFile(Decomposed.first), Ctx->getLangOpts(), 
                       Buffer.begin(), TokenStart, Buffer.end()); 
        Token Tok; 
        if (!RawLexer.LexFromRawLexer(Tok)) { 
            TokenLength = Tok.getLength(); 
        } 
    } 

    if (TokenLength > 0) { 
        Rewriter.ReplaceText(SpellingLoc, TokenLength, NewTypeStr); 
    } 
} 

//----------------------------------------------------------------------------- 
// Standard Helpers
//----------------------------------------------------------------------------- 

bool TypeCorrectMatcher::IsModifiable(SourceLocation DeclLoc, const SourceManager &SM) { 
    if (DeclLoc.isInvalid()) return false; 
    
    if (DeclLoc.isMacroID()) { 
        SourceLocation Spelling = SM.getSpellingLoc(DeclLoc); 
        return IsModifiable(Spelling, SM); 
    } 

    if (SM.isWrittenInMainFile(DeclLoc)) return true; 
    if (ProjectRoot.empty()) return false; 

    FileID FID = SM.getFileID(DeclLoc); 
    auto EntryRef = SM.getFileEntryRefForID(FID); 
    if (!EntryRef) return false; 
    StringRef FileName = EntryRef->getName(); 
    auto &Entry = EntryRef->getFileEntry(); 
    if (!Entry.tryGetRealPathName().empty()) FileName = Entry.tryGetRealPathName(); 

    llvm::SmallString<128> AbsPath(FileName); 
    SM.getFileManager().makeAbsolutePath(AbsPath); 
    llvm::sys::path::native(AbsPath); 
    
    if (!ExcludePattern.empty()) { 
        llvm::Regex R(ExcludePattern); 
        if (R.match(AbsPath)) return false; 
    } 
    return StringRef(AbsPath).starts_with(ProjectRoot); 
} 

void TypeCorrectMatcher::AddConstraint(const NamedDecl* Decl, 
                                       QualType CandidateType, 
                                       const Expr* BaseExpr, 
                                       ASTContext* Ctx) { 
    if (!Decl) return; 
    QualType InitialType; 
    if (const auto *V = dyn_cast<ValueDecl>(Decl)) InitialType = V->getType(); 
    else if (const auto *F = dyn_cast<FunctionDecl>(Decl)) InitialType = F->getReturnType(); 

    bool IsFixed = true; 
    if (const auto *FD = dyn_cast<FieldDecl>(Decl)) { 
        if (StructEngine.CanRewriteField(FD) && IsModifiable(FD->getLocation(), Rewriter.getSourceMgr())) IsFixed = false; 
    } else { 
        if (IsModifiable(Decl->getLocation(), Rewriter.getSourceMgr())) IsFixed = false; 
    } 
    Solver.AddNode(Decl, InitialType, IsFixed); 
    Solver.AddConstraint(Decl, CandidateType, BaseExpr, Ctx); 
} 

void TypeCorrectMatcher::AddUsageEdge(const NamedDecl *Target, const NamedDecl *Source, ASTContext *Ctx) { 
    if (!Target || !Source) return; 
    QualType TType, SType; 
    if (const auto *V = dyn_cast<ValueDecl>(Target)) TType = V->getType(); 
    if (const auto *V = dyn_cast<ValueDecl>(Source)) SType = V->getType(); 
    
    bool TFixed = !IsModifiable(Target->getLocation(), Rewriter.getSourceMgr()); 
    Solver.AddNode(Target, TType, TFixed); 
    
    bool SFixed = !IsModifiable(Source->getLocation(), Rewriter.getSourceMgr()); 
    Solver.AddNode(Source, SType, SFixed); 

    Solver.AddEdge(Target, Source); 
} 

QualType TypeCorrectMatcher::SynthesizeContainerType(QualType ContainerType, QualType NewValueType, ASTContext *Ctx) { 
    const auto *TST = ContainerType->getAs<TemplateSpecializationType>(); 
    if (!TST) return ContainerType; 
    
    SmallVector<TemplateArgument, 4> Args; 
    for (const auto &Arg : TST->template_arguments()) Args.push_back(Arg); 
    
    if (!Args.empty() && Args[0].getKind() == TemplateArgument::Type) { 
        if (!NewValueType.isNull()) { 
            Args[0] = TemplateArgument(NewValueType); 
        } 
    } 
    
    TemplateName Name = TST->getTemplateName(); 
    
    // Construct canonical arguments required by LLVM 16+ API
    SmallVector<TemplateArgument, 4> CanonArgs; 
    for (const auto &Arg : Args) { 
        if (Arg.getKind() == TemplateArgument::Type && !Arg.getAsType().isNull()) { 
            // Canonical type is required for the 3rd argument to prevent AST corruption
            CanonArgs.push_back(TemplateArgument(Ctx->getCanonicalType(Arg.getAsType()))); 
        } else { 
            // Fallback for non-type arguments (structurally identical) 
            CanonArgs.push_back(Arg); 
        } 
    } 

    // Explicitly call the 3-argument overload 
    // (TemplateName, SpecifiedArgs, CanonicalArgs) 
    return Ctx->getTemplateSpecializationType(Name, Args, CanonArgs); 
} 

QualType TypeCorrectMatcher::GetTypeFromExpression(const Expr *E, ASTContext *Ctx) { 
    if (!E) return {}; 
    if (const auto *CE = dyn_cast<CallExpr>(E)) return CE->getCallReturnType(*Ctx); 
    if (const auto *UE = dyn_cast<UnaryExprOrTypeTraitExpr>(E)) return Ctx->getSizeType(); 
    if (const auto *CO = dyn_cast<ConditionalOperator>(E)) return CO->getType(); 
    if (const auto *Cast = dyn_cast<ExplicitCastExpr>(E)) return GetTypeFromExpression(Cast->getSubExpr()->IgnoreParenImpCasts(), Ctx); 
    if (const auto *DR = dyn_cast<DeclRefExpr>(E)) { if (const auto *V = dyn_cast<ValueDecl>(DR->getDecl())) return V->getType(); } 
    if (const auto *ME = dyn_cast<MemberExpr>(E)) { if (const auto *V = dyn_cast<ValueDecl>(ME->getMemberDecl())) return V->getType(); } 
    if (const auto *BO = dyn_cast<BinaryOperator>(E)) { 
        auto Op = BO->getOpcode(); 
        if (Op == BO_Add || Op == BO_Sub || Op == BO_Mul) { 
            QualType LHS = GetTypeFromExpression(BO->getLHS()->IgnoreParenImpCasts(), Ctx); 
            QualType RHS = GetTypeFromExpression(BO->getRHS()->IgnoreParenImpCasts(), Ctx); 
            if (!LHS.isNull() && !RHS.isNull() && LHS->isIntegerType() && RHS->isIntegerType()) { 
                uint64_t SizeL = Ctx->getTypeSize(LHS); uint64_t SizeR = Ctx->getTypeSize(RHS); 
                if (SizeL > SizeR) return LHS; if (SizeR > SizeL) return RHS; if (LHS->isUnsignedIntegerType()) return LHS; return RHS; 
            } 
        } 
    } 
    return {}; 
} 

void TypeCorrectMatcher::ScanPrintfArgs(const clang::StringLiteral *FormatStr, const std::vector<const clang::Expr*> &Args, clang::ASTContext *Ctx) { 
} 
void TypeCorrectMatcher::ScanScanfArgs(const clang::StringLiteral *FormatStr, const std::vector<const clang::Expr*> &Args, clang::ASTContext *Ctx) { 
} 
void TypeCorrectMatcher::UpdateFormatSpecifiers(const clang::NamedDecl *Decl, const clang::QualType &NewType, clang::ASTContext *Ctx) { 
} 

//----------------------------------------------------------------------------- 
// Main Logic
//----------------------------------------------------------------------------- 

void TypeCorrectMatcher::run(const MatchFinder::MatchResult &Result) { 
  ASTContext *Ctx = Result.Context; 
  const VarDecl *Var = Result.Nodes.getNodeAs<VarDecl>("bound_var_decl"); 
  const FunctionDecl *Func = Result.Nodes.getNodeAs<FunctionDecl>("bound_func_decl"); 

  const VarDecl *ContainerVar = Result.Nodes.getNodeAs<VarDecl>("bound_container_var"); 
  const Expr *PushVal = Result.Nodes.getNodeAs<Expr>("bound_push_val"); 

  if (ContainerVar && PushVal) { 
      QualType ValType = GetTypeFromExpression(PushVal, Ctx); 
      // Safe Guard: Ensure the type extracted from the expression is valid before proceeding
      if (ValType.isNull()) return; 

      QualType ContType = ContainerVar->getType(); 
      QualType TargetContType = SynthesizeContainerType(ContType, ValType, Ctx); 
      AddConstraint(ContainerVar, TargetContType, PushVal, Ctx); 
  } 
  else if (Var && Result.Nodes.getNodeAs<Expr>("bound_init_expr")) { 
      const Expr* Init = Result.Nodes.getNodeAs<Expr>("bound_init_expr"); 
      AddConstraint(Var, GetTypeFromExpression(Init, Ctx), nullptr, Ctx); 
      if (const auto *DR = dyn_cast<DeclRefExpr>(Init->IgnoreParenImpCasts())) { 
          if (const auto *Src = dyn_cast<NamedDecl>(DR->getDecl())) AddUsageEdge(Var, Src, Ctx); 
      } 
  } 
  else if (Func) { 
      if (const auto *RetVal = Result.Nodes.getNodeAs<Expr>("bound_ret_val")) { 
          AddConstraint(Func, RetVal->getType(), nullptr, Ctx); 
      } 
  } 
  else if (Result.Nodes.getNodeAs<BinaryOperator>("bound_assign_op")) { 
      const NamedDecl *Target = nullptr; 
      if (const auto *AV = Result.Nodes.getNodeAs<VarDecl>("bound_assign_var")) Target = AV; 
      else if (const auto *AF = Result.Nodes.getNodeAs<FieldDecl>("bound_assign_field")) Target = AF; 
      
      const Expr *AssignExpr = Result.Nodes.getNodeAs<Expr>("bound_assign_expr"); 
      if (Target) { 
          AddConstraint(Target, GetTypeFromExpression(AssignExpr, Ctx), nullptr, Ctx); 
          if (const auto *DR = dyn_cast<DeclRefExpr>(AssignExpr->IgnoreParenImpCasts())) { 
              if (const auto *Src = dyn_cast<NamedDecl>(DR->getDecl())) AddUsageEdge(Target, Src, Ctx); 
          } 
      } 
  } 
} 

void TypeCorrectMatcher::onEndOfTranslationUnit(ASTContext &Ctx) { 
    if (CurrentPhase == type_correct::Phase::Map) return; 

    std::map<const clang::NamedDecl *, type_correct::NodeState> Updates = Solver.Solve(&Ctx); 

    for (const auto &Pair : Updates) { 
        const NamedDecl *Decl = Pair.first; 
        const type_correct::NodeState &State = Pair.second; 

        bool IsUnsignedCandidate = State.ConstraintType->isUnsignedIntegerType(); 
        if (VariablesWithNegativeValues.count(Decl) && IsUnsignedCandidate) continue; 

        const VarDecl *V = dyn_cast<VarDecl>(Decl); 
        const FunctionDecl *F = dyn_cast<FunctionDecl>(Decl); 
        const FieldDecl *FD = dyn_cast<FieldDecl>(Decl); 

        if (V) { 
             if (V->getTypeSourceInfo() && V->getType().getCanonicalType() != State.ConstraintType.getCanonicalType()) { 
                ResolveType(V->getTypeSourceInfo()->getTypeLoc(), State.ConstraintType, &Ctx, V, State.BaseExpr); 
             } 
             if (CastsToRemove.count(V)) for (const auto *Cast : CastsToRemove[V]) RemoveExplicitCast(Cast, &Ctx); 
             UpdateFormatSpecifiers(V, State.ConstraintType, &Ctx); 
        } 
        else if (F && F->getTypeSourceInfo()) { 
             TypeLoc TL = F->getTypeSourceInfo()->getTypeLoc(); 
             if (auto FTL = TL.getAs<FunctionTypeLoc>()) 
                 ResolveType(FTL.getReturnLoc(), State.ConstraintType, &Ctx, nullptr, State.BaseExpr); 
        } 
        else if (FD) { 
             if (FD->getTypeSourceInfo()) ResolveType(FD->getTypeSourceInfo()->getTypeLoc(), State.ConstraintType, &Ctx, nullptr, State.BaseExpr); 
        } 
    } 

    SourceManager &SM = Rewriter.getSourceMgr(); 
    if (InPlace && !ProjectRoot.empty()) Rewriter.overwriteChangedFiles(); 
    else { 
        if (const llvm::RewriteBuffer *Buffer = Rewriter.getRewriteBufferFor(SM.getMainFileID())) Buffer->write(llvm::outs()); 
        else { llvm::outs() << SM.getBufferOrFake(SM.getMainFileID()).getBuffer(); } 
        llvm::outs().flush(); 
    } 
} 

void TypeCorrectMatcher::ResolveType(const TypeLoc &OldLoc, const QualType &NewType, ASTContext *Ctx, 
                                     const VarDecl *BoundVar, const Expr *BaseExpr) { 
  if (BoundVar) { 
      if (!Ctx->getParents(*BoundVar).empty()) { 
          const Stmt *ParentStmt = Ctx->getParents(*BoundVar)[0].get<Stmt>(); 
          if (const auto *DS = dyn_cast_or_null<DeclStmt>(ParentStmt)) { 
              if (!DS->isSingleDecl()) { 
                  HandleMultiDecl(DS, Ctx); 
                  return; 
              } 
          } 
      } 
  } 
  RecursivelyRewriteType(OldLoc, NewType, Ctx, BaseExpr); 
} 

void TypeCorrectMatcher::RemoveExplicitCast(const ExplicitCastExpr *CastExpr, ASTContext *Ctx) { 
     if (!CastExpr) return; 
     SourceRange CastRange = CastExpr->getSourceRange(); 
     SourceRange SubExprRange = CastExpr->getSubExpr()->getSourceRange(); 
     if (CastRange.isValid() && SubExprRange.isValid() && IsModifiable(CastRange.getBegin(), Rewriter.getSourceMgr())) { 
         StringRef SubExprText = Lexer::getSourceText(CharSourceRange::getTokenRange(SubExprRange), Rewriter.getSourceMgr(), Ctx->getLangOpts()); 
         if (!SubExprText.empty()) Rewriter.ReplaceText(CastRange, SubExprText); 
     } 
} 

void TypeCorrectMatcher::InjectCast(const Expr *ExprToCast, const QualType &TargetType, ASTContext *Ctx) { 
    if (!ExprToCast) return; 
    clang::PrintingPolicy Policy = Ctx->getPrintingPolicy(); Policy.SuppressScope = false; Policy.FullyQualifiedName = true; 
    std::string TypeName = TargetType.getAsString(Policy); 
    std::string CastPrefix = Ctx->getLangOpts().CPlusPlus ? ("static_cast<" + TypeName + ">(") : ("(" + TypeName + ")("); 
    SourceRange SR = ExprToCast->getSourceRange(); 
    if (SR.isValid()) { Rewriter.InsertTextBefore(SR.getBegin(), CastPrefix); Rewriter.InsertTextAfterToken(SR.getEnd(), ")"); } 
} 

void TypeCorrectMatcher::HandleMultiDecl(const DeclStmt *DS, ASTContext *Ctx) { 
    if (RewrittenStmts.count(DS)) return; 
    std::string ReplacementBlock; llvm::raw_string_ostream Stream(ReplacementBlock); 
    for (const auto *D : DS->decls()) { 
        const VarDecl *VD = dyn_cast<VarDecl>(D); if (!VD) continue; 
        Stream << VD->getType().getAsString() << " " << VD->getNameAsString() << "; "; 
    } 
    Rewriter.ReplaceText(DS->getSourceRange(), Stream.str()); 
    RewrittenStmts.insert(DS); 
} 

//----------------------------------------------------------------------------- 
// Consumer & Plugin Registry
//----------------------------------------------------------------------------- 
TypeCorrectASTConsumer::TypeCorrectASTConsumer(clang::Rewriter &R, bool UseDecltype, bool ExpandAuto, std::string ProjectRoot, std::string ExcludePattern, bool InPlace, bool EnableAbiBreakingChanges, type_correct::Phase CurrentPhase, std::string FactsOutputDir) 
    : Handler(R, UseDecltype, ExpandAuto, std::move(ProjectRoot), std::move(ExcludePattern), InPlace, EnableAbiBreakingChanges, CurrentPhase, std::move(FactsOutputDir)) { 

    auto SizeOrAlign = unaryExprOrTypeTraitExpr(anyOf(ofKind(UETT_SizeOf), ofKind(UETT_AlignOf))); 
    auto BaseSource = anyOf(callExpr(), SizeOrAlign); 
    auto TernarySource = conditionalOperator(anyOf(hasTrueExpression(ignoringParenImpCasts(BaseSource)), hasFalseExpression(ignoringParenImpCasts(BaseSource)))); 
    auto TypeSource = expr(anyOf(BaseSource, TernarySource)); 

    Finder.addMatcher(varDecl(hasInitializer(ignoringParenImpCasts(TypeSource.bind("bound_init_expr")))).bind("bound_var_decl"), &Handler); 
    
    // Matched for function return types
    Finder.addMatcher( 
        functionDecl( 
            isDefinition(), 
            hasBody(forEachDescendant(returnStmt(hasReturnValue(ignoringParenImpCasts(TypeSource.bind("bound_ret_val")))))) 
        ).bind("bound_func_decl"), &Handler
    ); 

    Finder.addMatcher( 
        cxxMemberCallExpr( 
            callee(functionDecl(hasName("push_back"))), 
            on(declRefExpr(to(varDecl().bind("bound_container_var")))), 
            hasArgument(0, ignoringParenImpCasts(TypeSource.bind("bound_push_val"))) 
        ), &Handler
    ); 

    Finder.addMatcher(binaryOperator(hasOperatorName("="), hasLHS(ignoringParenImpCasts(anyOf(declRefExpr(to(varDecl().bind("bound_assign_var"))), memberExpr(member(fieldDecl().bind("bound_assign_field")))))), hasRHS(ignoringParenImpCasts(TypeSource.bind("bound_assign_expr")))).bind("bound_assign_op"), &Handler); 
} 

class TCPluginAction : public clang::PluginASTAction { 
public: 
  bool UseDecltype = false; bool ExpandAuto = false; std::string ProjectRoot; std::string ExcludePattern; bool InPlace = false; bool EnableAbiBreakingChanges = false; 
  type_correct::Phase CurrentPhase = type_correct::Phase::Standalone; std::string FactsOutputDir; 

  bool ParseArgs(const clang::CompilerInstance &CI, const std::vector<std::string> &args) override { 
    for (size_t i = 0; i < args.size(); ++i) { 
        if (args[i] == "-use-decltype") UseDecltype = true; 
        else if (args[i] == "-expand-auto") ExpandAuto = true; 
        else if (args[i] == "-project-root" && i + 1 < args.size()) ProjectRoot = args[++i]; 
        else if (args[i] == "-exclude" && i + 1 < args.size()) ExcludePattern = args[++i]; 
        else if (args[i] == "-in-place") InPlace = true; 
        else if (args[i] == "-enable-abi-breaking-changes") EnableAbiBreakingChanges = true; 
        else if (args[i] == "-phase" && i + 1 < args.size()) { /*...*/ } 
        else if (args[i] == "-facts-dir" && i + 1 < args.size()) { /*...*/ } 
    } 
    return true; 
  } 

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef file) override { 
    RewriterForTC.setSourceMgr(CI.getSourceManager(), CI.getLangOpts()); 
    return std::make_unique<TypeCorrectASTConsumer>(RewriterForTC, UseDecltype, ExpandAuto, ProjectRoot, ExcludePattern, InPlace, EnableAbiBreakingChanges, CurrentPhase, FactsOutputDir); 
  } 
private: 
  clang::Rewriter RewriterForTC; 
}; 

static clang::FrontendPluginRegistry::Add<TCPluginAction> X("TypeCorrect", "Type Correction Plugin");