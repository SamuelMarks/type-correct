/**
 * @file TypeCorrect.cpp
 * @brief Unified Implementation of TypeCorrect Logic with Audit & Reporting.
 *
 * This file integrates:
 * 1. **Standard Type Propagation**: Assignments, Initializations, Returns.
 * 2. **Deep Template Analysis**: Recursively rewriting template arguments.
 * 3. **CTU/Boundary Analysis**: Integration with Global Facts and System Boundaries.
 * 4. **Audit Engine**: Separation of change calculation from application.
 *
 * @author SamuelMarks
 * @license CC0
 */

#include "TypeCorrect.h"

#include <algorithm>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/AST/Expr.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/TypeLoc.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendPluginRegistry.h>
#include <clang/Index/USRGeneration.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Regex.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/JSON.h>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

using namespace clang;
using namespace clang::ast_matchers;

//-----------------------------------------------------------------------------
// Constructor & Configuration
//-----------------------------------------------------------------------------

TypeCorrectMatcher::TypeCorrectMatcher(
    clang::Rewriter &Rewriter, bool UseDecltype, bool ExpandAuto,
    std::string ProjectRoot, std::string ExcludePattern, bool InPlace,
    bool EnableAbiBreakingChanges, bool AuditMode,
    type_correct::Phase CurrentPhase, std::string FactsOutputDir,
    std::string ReportFile)
    : Rewriter(Rewriter), UseDecltype(UseDecltype), ExpandAuto(ExpandAuto),
      ProjectRoot(std::move(ProjectRoot)),
      ExcludePattern(std::move(ExcludePattern)), InPlace(InPlace),
      AuditMode(AuditMode),
      // Fix: Pass false for ForceRewrite (2nd arg) to match updated StructAnalyzer signature
      StructEngine(EnableAbiBreakingChanges, /*ForceRewrite=*/false, this->ProjectRoot),
      CurrentPhase(CurrentPhase), FactsOutputDir(std::move(FactsOutputDir)),
      ReportFile(std::move(ReportFile)) {
  EnsureGlobalFactsLoaded();
}

std::vector<type_correct::ChangeRecord> TypeCorrectMatcher::GetChanges() const {
  return Changes;
}

void TypeCorrectMatcher::EnsureGlobalFactsLoaded() {
  if (FactsOutputDir.empty())
    return;
  if (!GlobalFacts.empty())
    return;
  std::vector<type_correct::ctu::SymbolFact> Raw;
  std::string GlobalFile = FactsOutputDir + "/global.facts";
  if (type_correct::ctu::FactManager::ReadFacts(GlobalFile, Raw)) {
    for (const auto &F : Raw)
      GlobalFacts[F.USR] = F;
  }
}

//-----------------------------------------------------------------------------
// Constraint & Boundary Logic
//-----------------------------------------------------------------------------

void TypeCorrectMatcher::AddConstraint(const NamedDecl *Decl,
                                       QualType CandidateType,
                                       const Expr *BaseExpr, ASTContext *Ctx) {
  if (!Decl)
    return;

  // Boundary Check (Safe Mode)
  bool IsFixed = StructEngine.IsBoundaryFixed(Decl, Rewriter.getSourceMgr());

  if (const auto *FD = dyn_cast<FieldDecl>(Decl)) {
    if (!IsFixed && !StructEngine.CanRewriteField(FD, Rewriter.getSourceMgr()))
      IsFixed = true;
  }

  QualType InitialType;
  if (const auto *V = dyn_cast<ValueDecl>(Decl))
    InitialType = V->getType();
  else if (const auto *F = dyn_cast<FunctionDecl>(Decl))
    InitialType = F->getReturnType();

  Solver.AddNode(Decl, InitialType, IsFixed);
  ApplyGlobalFactIfExists(Decl, Ctx);
  Solver.AddConstraint(Decl, CandidateType, BaseExpr, Ctx);
}

void TypeCorrectMatcher::AddUsageEdge(const NamedDecl *Target,
                                      const NamedDecl *Source,
                                      ASTContext *Ctx) {
  if (!Target || !Source)
    return;

  bool TFixed = StructEngine.IsBoundaryFixed(Target, Rewriter.getSourceMgr());
  QualType TType;
  if (const auto *V = dyn_cast<ValueDecl>(Target))
    TType = V->getType();
  else if (const auto *F = dyn_cast<FunctionDecl>(Target))
    TType = F->getReturnType();
  Solver.AddNode(Target, TType, TFixed);
  ApplyGlobalFactIfExists(Target, Ctx);

  bool SFixed = StructEngine.IsBoundaryFixed(Source, Rewriter.getSourceMgr());
  QualType SType;
  if (const auto *V = dyn_cast<ValueDecl>(Source))
    SType = V->getType();
  else if (const auto *F = dyn_cast<FunctionDecl>(Source))
    SType = F->getReturnType();
  Solver.AddNode(Source, SType, SFixed);
  ApplyGlobalFactIfExists(Source, Ctx);

  Solver.AddEdge(Target, Source);
}

//-----------------------------------------------------------------------------
// Template & Type Resolution Logic (Rewriting)
//-----------------------------------------------------------------------------

bool TypeCorrectMatcher::RecursivelyRewriteType(TypeLoc TL, QualType TargetType,
                                                ASTContext *Ctx,
                                                const Expr *BaseExpr) {
  if (TL.isNull() || TargetType.isNull())
    return false;
  if (TL.getAs<AutoTypeLoc>() && !ExpandAuto)
    return false;
  
  // Macros are tricky; simplified handling for now
  if (TL.getBeginLoc().isMacroID()) {
    RewriteMacroType(TL.getBeginLoc(), TargetType, Ctx, BaseExpr);
    return true;
  }

  if (Ctx->hasSameType(TL.getType(), TargetType))
    return false;
  
  if (auto QTL = TL.getAs<QualifiedTypeLoc>())
    return RecursivelyRewriteType(QTL.getUnqualifiedLoc(),
                                  TargetType.getUnqualifiedType(), Ctx,
                                  BaseExpr);

  // Template Specialization Rewriting
  if (auto TSTL = TL.getAs<TemplateSpecializationTypeLoc>()) {
    const auto *TargetTST = TargetType->getAs<TemplateSpecializationType>();
    if (!TargetTST)
      return false;

    bool ChangedAny = false;
    const auto &TargetArgs = TargetTST->template_arguments();
    unsigned SourceIdx = 0;
    unsigned TargetIdx = 0;

    unsigned NumSourceArgs = TSTL.getNumArgs();
    unsigned NumTargetArgs = TargetArgs.size();

    while (SourceIdx < NumSourceArgs && TargetIdx < NumTargetArgs) {
      TemplateArgumentLoc SourceArgLoc = TSTL.getArgLoc(SourceIdx);
      const TemplateArgument &TargetArg = TargetArgs[TargetIdx];

      if (TargetArg.getKind() == TemplateArgument::Pack) {
        for (const auto &PackElement : TargetArg.pack_elements()) {
          if (SourceIdx >= NumSourceArgs)
            break;
          SourceArgLoc = TSTL.getArgLoc(SourceIdx);
          if (PackElement.getKind() == TemplateArgument::Type &&
              SourceArgLoc.getArgument().getKind() == TemplateArgument::Type) {

            QualType OldT = SourceArgLoc.getTypeSourceInfo()->getType();
            QualType NewT = PackElement.getAsType();
            if (IsTemplateInstantiationSafe(TargetTST, OldT, NewT, Ctx)) {
              if (TypeSourceInfo *TSI = SourceArgLoc.getTypeSourceInfo()) {
                if (RecursivelyRewriteType(TSI->getTypeLoc(), NewT, Ctx,
                                           BaseExpr))
                  ChangedAny = true;
              }
            }
          }
          SourceIdx++;
        }
        TargetIdx++;
      } else {
        if (TargetArg.getKind() == TemplateArgument::Type &&
            SourceArgLoc.getArgument().getKind() == TemplateArgument::Type) {

          QualType OldT = SourceArgLoc.getTypeSourceInfo()->getType();
          QualType NewT = TargetArg.getAsType();
          if (IsTemplateInstantiationSafe(TargetTST, OldT, NewT, Ctx)) {
            if (TypeSourceInfo *TSI = SourceArgLoc.getTypeSourceInfo()) {
              if (RecursivelyRewriteType(TSI->getTypeLoc(), NewT, Ctx,
                                         BaseExpr))
                ChangedAny = true;
            }
          }
        }
        SourceIdx++;
        TargetIdx++;
      }
    }
    return ChangedAny;
  }

  if (auto ETL = TL.getAs<ElaboratedTypeLoc>())
    return RecursivelyRewriteType(ETL.getNamedTypeLoc(), TargetType, Ctx,
                                  BaseExpr);

  if (auto FPTL = TL.getAs<FunctionProtoTypeLoc>()) {
    const auto *TargetFPT = TargetType->getAs<FunctionProtoType>();
    if (TargetFPT && FPTL.getNumParams() == TargetFPT->getNumParams()) {
      bool ChangedAny = false;
      if (RecursivelyRewriteType(FPTL.getReturnLoc(),
                                 TargetFPT->getReturnType(), Ctx, nullptr))
        ChangedAny = true;
      for (unsigned i = 0; i < FPTL.getNumParams(); ++i) {
        if (FPTL.getParam(i)->getTypeSourceInfo()) {
          if (RecursivelyRewriteType(FPTL.getParam(i)
                                         ->getTypeSourceInfo()
                                         ->getTypeLoc(),
                                     TargetFPT->getParamType(i), Ctx, nullptr))
            ChangedAny = true;
        }
      }
      return ChangedAny;
    }
  }

  // --- Final Leaf Rewrite logic with Audit Hooks ---
  
  if (!IsModifiable(TL.getBeginLoc(), Rewriter.getSourceMgr()))
    return false;

  std::string NewTypeStr;
  bool UsingDecltype = false;
  if (UseDecltype && BaseExpr) {
    QualType ExprType = BaseExpr->getType().getCanonicalType();
    QualType TargetCan = TargetType.getCanonicalType();
    if (Ctx->hasSameType(ExprType, TargetCan)) {
      SourceManager &SM = Rewriter.getSourceMgr();
      CharSourceRange Range =
          CharSourceRange::getTokenRange(BaseExpr->getSourceRange());
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

  // Audit / Recording
  SourceManager &SM = Rewriter.getSourceMgr();
  CharSourceRange Range = CharSourceRange::getTokenRange(TL.getSourceRange());
  StringRef OldTypeStr = Lexer::getSourceText(Range, SM, Ctx->getLangOpts());

  type_correct::ChangeRecord Record;
  Record.FilePath = SM.getFilename(TL.getBeginLoc()).str();
  Record.Line = SM.getPresumedLineNumber(TL.getBeginLoc());
  Record.Symbol = CurrentProcessingDecl ? CurrentProcessingDecl->getNameAsString() : "<?>";
  Record.OldType = OldTypeStr.str();
  Record.NewType = NewTypeStr;
  Changes.push_back(Record);

  // Application
  if (!AuditMode) {
      Rewriter.ReplaceText(TL.getSourceRange(), NewTypeStr);
  }
  
  return true;
}

//-----------------------------------------------------------------------------
// Unified Dispatch
//-----------------------------------------------------------------------------

void TypeCorrectMatcher::run(const MatchFinder::MatchResult &Result) {
  ASTContext *Ctx = Result.Context;

  // --- 1. Deep Template Hook ---
  if (const auto *Call =
          Result.Nodes.getNodeAs<CXXMemberCallExpr>("template_mem_call")) {
    const Expr *Obj = Call->getImplicitObjectArgument();
    const FunctionDecl *Callee = Call->getDirectCallee();
    if (Obj && Callee) {
      QualType ObjType = Obj->getType();
      if (const auto *TST = ObjType->getAs<TemplateSpecializationType>()) {
        unsigned NumArgs = Call->getNumArgs();
        unsigned NumParams = Callee->getNumParams();
        for (unsigned i = 0; i < std::min(NumArgs, NumParams); ++i) {
          const Expr *Arg = Call->getArg(i);
          const ParmVarDecl *Param = Callee->getParamDecl(i);
          QualType ArgT = GetTypeFromExpression(Arg->IgnoreParenImpCasts(), Ctx);
          if (ArgT.isNull())
            continue;

          // Check against template args (handling refs)
          QualType ParamT =
              Param->getType().getNonReferenceType().getUnqualifiedType();

          for (unsigned k = 0; k < TST->template_arguments().size(); ++k) {
            const TemplateArgument &TA = TST->template_arguments()[k];
            if (TA.getKind() == TemplateArgument::Type) {
              if (Ctx->hasSameType(TA.getAsType(), ParamT)) {
                if (const auto *DR =
                        dyn_cast<DeclRefExpr>(Obj->IgnoreParenImpCasts())) {
                  if (const auto *Var = dyn_cast<VarDecl>(DR->getDecl())) {
                    QualType NewCont =
                        SynthesizeContainerType(ObjType, ParamT, ArgT, Ctx);
                    AddConstraint(Var, NewCont, Arg, Ctx);
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  // --- 2. Truncation Safety Hook ---
  if (const auto *ME = Result.Nodes.getNodeAs<MemberExpr>("any_access_member")) {
    if (const auto *FD = dyn_cast<FieldDecl>(ME->getMemberDecl())) {
      if (const auto *Func =
              Result.Nodes.getNodeAs<FunctionDecl>("enclosing_func")) {
        StructEngine.AnalyzeTruncationSafety(FD, Func, Ctx);
      }
    }
  }

  // --- 3. Pointer Semantics ---
  if (const auto *Subscript =
          Result.Nodes.getNodeAs<ArraySubscriptExpr>("generic_subscript")) {
    const Expr *Idx = Subscript->getIdx()->IgnoreParenImpCasts();
    if (const auto *DR = dyn_cast<DeclRefExpr>(Idx)) {
      if (const auto *Var = dyn_cast<ValueDecl>(DR->getDecl())) {
        AddConstraint(Var, Var->getType(), nullptr, Ctx);
        Solver.AddPointerOffsetUsage(Var);
      }
    }
  }

  if (const auto *BO = Result.Nodes.getNodeAs<BinaryOperator>("ptr_arith")) {
    Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
    Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();
    const NamedDecl *OffsetVar = nullptr;
    if (LHS->getType()->isPointerType() && RHS->getType()->isIntegerType()) {
      if (const auto *DR = dyn_cast<DeclRefExpr>(RHS))
        OffsetVar = DR->getDecl();
    } else if (RHS->getType()->isPointerType() &&
               LHS->getType()->isIntegerType()) {
      if (const auto *DR = dyn_cast<DeclRefExpr>(LHS))
        OffsetVar = DR->getDecl();
    }
    if (OffsetVar) {
      if (const auto *V = dyn_cast<ValueDecl>(OffsetVar)) {
        AddConstraint(OffsetVar, V->getType(), nullptr, Ctx);
        Solver.AddPointerOffsetUsage(OffsetVar);
      }
    }
  }

  // --- 4. Standard Type Propagation (Assignments, Inits) ---
  const VarDecl *Var = Result.Nodes.getNodeAs<VarDecl>("bound_var_decl");
  if (Var && Result.Nodes.getNodeAs<Expr>("bound_init_expr")) {
    const Expr *Init = Result.Nodes.getNodeAs<Expr>("bound_init_expr");
    AddConstraint(Var, GetTypeFromExpression(Init, Ctx), nullptr, Ctx);
    if (const auto *DR = dyn_cast<DeclRefExpr>(Init->IgnoreParenImpCasts())) {
      if (const auto *Src = dyn_cast<NamedDecl>(DR->getDecl()))
        AddUsageEdge(Var, Src, Ctx);
    }
  }

  if (Result.Nodes.getNodeAs<BinaryOperator>("bound_assign_op")) {
    const NamedDecl *Target = nullptr;
    if (const auto *AV = Result.Nodes.getNodeAs<VarDecl>("bound_assign_var"))
      Target = AV;
    else if (const auto *AF =
                 Result.Nodes.getNodeAs<FieldDecl>("bound_assign_field"))
      Target = AF;
    const Expr *AssignExpr = Result.Nodes.getNodeAs<Expr>("bound_assign_expr");
    if (Target) {
      // Symbolic Analysis Hook
      bool HandledSymbolically = false;
      if (const auto *BO =
              dyn_cast<BinaryOperator>(AssignExpr->IgnoreParenImpCasts())) {
        type_correct::OpKind Op = type_correct::OpKind::None;
        if (BO->getOpcode() == BO_Add)
          Op = type_correct::OpKind::Add;
        else if (BO->getOpcode() == BO_Sub)
          Op = type_correct::OpKind::Sub;
        else if (BO->getOpcode() == BO_Mul)
          Op = type_correct::OpKind::Mul;
        if (Op != type_correct::OpKind::None) {
          const NamedDecl *LHS = nullptr;
          const NamedDecl *RHS = nullptr;
          if (const auto *LDR =
                  dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParenImpCasts()))
            LHS = LDR->getDecl();
          if (const auto *RDR =
                  dyn_cast<DeclRefExpr>(BO->getRHS()->IgnoreParenImpCasts()))
            RHS = RDR->getDecl();
          if (LHS && RHS) {
            if (const auto *ValTarget = dyn_cast<ValueDecl>(Target))
              Solver.AddNode(Target, ValTarget->getType(), false);
            if (const auto *V = dyn_cast<ValueDecl>(LHS))
              Solver.AddNode(LHS, V->getType(), false);
            if (const auto *V = dyn_cast<ValueDecl>(RHS))
              Solver.AddNode(RHS, V->getType(), false);
            Solver.AddSymbolicConstraint(Target, Op, LHS, RHS);
            HandledSymbolically = true;
          }
        }
      }
      if (!HandledSymbolically) {
        AddConstraint(Target, GetTypeFromExpression(AssignExpr, Ctx), nullptr,
                      Ctx);
        if (const auto *DR =
                dyn_cast<DeclRefExpr>(AssignExpr->IgnoreParenImpCasts())) {
          if (const auto *Src = dyn_cast<NamedDecl>(DR->getDecl()))
            AddUsageEdge(Target, Src, Ctx);
        }
      }
    }
  }

  // --- 5. Misc ---
  if (const auto *VarFn = Result.Nodes.getNodeAs<VarDecl>("std_func_var")) {
    const auto *LE = Result.Nodes.getNodeAs<LambdaExpr>("assigned_lambda");
    if (VarFn && LE) {
      if (const CXXMethodDecl *CallOp = LE->getCallOperator())
        StdFunctionToLambdaMap[VarFn] = CallOp;
    }
  }
}

void TypeCorrectMatcher::onEndOfTranslationUnit(ASTContext &Ctx) {
  auto UnsafeFields = StructEngine.GetLikelyUnsafeFields();
  for (const auto *UnsafeField : UnsafeFields) {
    Solver.AddNode(UnsafeField, UnsafeField->getType(), true);
  }

  std::map<const clang::NamedDecl *, type_correct::NodeState> Updates =
      Solver.Solve(&Ctx);

  for (const auto &Pair : Updates) {
    const NamedDecl *Decl = Pair.first;
    const type_correct::NodeState &State = Pair.second;

    if (StructEngine.IsBoundaryFixed(Decl, Rewriter.getSourceMgr()))
      continue;

    // Set Context for Audit Logging
    CurrentProcessingDecl = Decl;

    QualType FinalType = State.ConstraintType;
    if (const auto *V = dyn_cast<VarDecl>(Decl)) {
      if (V->getTypeSourceInfo() &&
          V->getType().getCanonicalType() != FinalType.getCanonicalType()) {
        ResolveType(V->getTypeSourceInfo()->getTypeLoc(), FinalType, &Ctx, V,
                    State.BaseExpr);
      }
    } else if (const auto *FD = dyn_cast<FieldDecl>(Decl)) {
      if (FD->getTypeSourceInfo())
        ResolveType(FD->getTypeSourceInfo()->getTypeLoc(), FinalType, &Ctx,
                    nullptr, State.BaseExpr);
    }
    
    CurrentProcessingDecl = nullptr;
  }

  // Handling Output
  SourceManager &SM = Rewriter.getSourceMgr();

  // Audit / Report Printing
  if (AuditMode || !ReportFile.empty()) {
      if (AuditMode) {
          llvm::outs() << "| File | Line | Symbol | Old Type | New Type |\n";
          llvm::outs() << "|---|---|---|---|---|\n";
          for(const auto &C : Changes) {
              llvm::outs() << "| " << llvm::sys::path::filename(C.FilePath) << " | " 
                           << C.Line << " | `" << C.Symbol << "` | `" 
                           << C.OldType << "` | `" << C.NewType << "` |\n";
          }
      }
      
      if (!ReportFile.empty()) {
          // JSON Output
          std::error_code EC;
          llvm::raw_fd_ostream OS(ReportFile, EC, llvm::sys::fs::OF_Append); // Simple append for multiple TUs, creates invalid JSON array but is line-delimited
          if (!EC) {
              for(const auto &C : Changes) {
                  OS << "{ \"file\": \"" << C.FilePath 
                     << "\", \"line\": " << C.Line 
                     << ", \"symbol\": \"" << C.Symbol 
                     << "\", \"old\": \"" << C.OldType 
                     << "\", \"new\": \"" << C.NewType << "\" }\n";
              }
          }
      }
  }

  // File Rewriting
  if (!AuditMode) {
      if (InPlace && !ProjectRoot.empty())
        Rewriter.overwriteChangedFiles();
      else {
        if (const llvm::RewriteBuffer *Buffer =
                Rewriter.getRewriteBufferFor(SM.getMainFileID()))
          Buffer->write(llvm::outs());
        else
          llvm::outs() << SM.getBufferOrFake(SM.getMainFileID()).getBuffer();
        llvm::outs().flush();
      }
  }
}

// -- Helper Implementations --
void TypeCorrectMatcher::ResolveType(const TypeLoc &OldLoc,
                                     const QualType &NewType, ASTContext *Ctx,
                                     const VarDecl *BoundVar,
                                     const Expr *BaseExpr) {
  RecursivelyRewriteType(OldLoc, NewType, Ctx, BaseExpr);
}

void TypeCorrectMatcher::RewriteMacroType(SourceLocation Loc,
                                          const QualType &NewType,
                                          ASTContext *Ctx,
                                          const Expr *BaseExpr) {
  if (!Loc.isMacroID())
    return;
  SourceManager &SM = Rewriter.getSourceMgr();
  SourceLocation SpellingLoc = SM.getSpellingLoc(Loc);
  if (!IsModifiable(SpellingLoc, SM))
    return;
  
  std::string NewTypeStr = NewType.getAsString(Ctx->getPrintingPolicy());

  // In Audit mode, just record it roughly
  if (AuditMode) {
      type_correct::ChangeRecord Record;
      Record.FilePath = SM.getFilename(SpellingLoc).str();
      Record.Line = SM.getPresumedLineNumber(SpellingLoc);
      Record.Symbol = CurrentProcessingDecl ? CurrentProcessingDecl->getNameAsString() : "MACRO";
      Record.OldType = "MACRO_EXPANSION"; 
      Record.NewType = NewTypeStr;
      Changes.push_back(Record);
      return; 
  }

  // Placeholder logic for macro text length
  unsigned TokenLength = 3; 
  if (TokenLength > 0)
    Rewriter.ReplaceText(SpellingLoc, TokenLength, NewTypeStr);
}

bool TypeCorrectMatcher::IsModifiable(SourceLocation Loc,
                                      const SourceManager &SM) {
  if (SM.isWrittenInMainFile(Loc))
    return true;
  if (!ProjectRoot.empty()) {
    FileID FID = SM.getFileID(SM.getSpellingLoc(Loc));
    auto EntryRef = SM.getFileEntryRefForID(FID);
    if (!EntryRef)
      return false;
    llvm::SmallString<128> AbsPath(EntryRef->getName());
    SM.getFileManager().makeAbsolutePath(AbsPath);
    if (!llvm::StringRef(AbsPath).starts_with(ProjectRoot))
      return false;
  }
  return true;
}

clang::QualType TypeCorrectMatcher::ParseTypeString(const std::string &TypeName,
                                                    clang::ASTContext *Ctx) {
  if (TypeName == "size_t")
    return Ctx->getSizeType();
  return Ctx->getSizeType();
}

void TypeCorrectMatcher::ApplyGlobalFactIfExists(const clang::NamedDecl *Decl,
                                                 clang::ASTContext *Ctx) {
  if (!Decl || GlobalFacts.empty())
    return;
  llvm::SmallString<128> USR;
  if (index::generateUSRForDecl(Decl, USR))
    return;
  auto It = GlobalFacts.find(USR.str().str());
  if (It != GlobalFacts.end()) {
    QualType T = ParseTypeString(It->second.TypeName, Ctx);
    Solver.AddGlobalConstraint(Decl, T, Ctx);
  }
}

const llvm::StringRef *GetDeclName(const clang::NamedDecl *D) { return nullptr; }

// -- Safety Checks --

bool TypeCorrectMatcher::IsTemplateInstantiationSafe(
    const TemplateSpecializationType *TST, QualType OldType, QualType NewType,
    ASTContext *Ctx) {

  TemplateName TN = TST->getTemplateName();
  ClassTemplateDecl *TD =
      dyn_cast_or_null<ClassTemplateDecl>(TN.getAsTemplateDecl());
  if (!TD)
    return true;

  SmallVector<TemplateArgument, 4> OldArgs;
  SmallVector<TemplateArgument, 4> NewArgs;

  for (const auto &Arg : TST->template_arguments()) {
    if (Arg.getKind() == TemplateArgument::Type &&
        Ctx->hasSameType(Arg.getAsType(), OldType)) {
      OldArgs.push_back(TemplateArgument(OldType));
      NewArgs.push_back(TemplateArgument(NewType));
    } else {
      OldArgs.push_back(Arg);
      NewArgs.push_back(Arg);
    }
  }

  const auto *SpecOld = DetermineActiveSpecialization(TD, OldArgs);
  const auto *SpecNew = DetermineActiveSpecialization(TD, NewArgs);

  return SpecOld == SpecNew;
}

const ClassTemplatePartialSpecializationDecl *
TypeCorrectMatcher::DetermineActiveSpecialization(
    ClassTemplateDecl *Template, const llvm::ArrayRef<TemplateArgument> &Args) {

  for (ClassTemplateSpecializationDecl *Spec : Template->specializations()) {
    auto *PS = dyn_cast<ClassTemplatePartialSpecializationDecl>(Spec);
    if (!PS)
      continue;

    const TemplateArgumentList &PSArgs = PS->getTemplateArgs();
    if (PSArgs.size() != Args.size())
      continue;

    bool Matches = true;
    for (unsigned i = 0; i < Args.size(); ++i) {
      if (PSArgs.get(i).getKind() == TemplateArgument::Type &&
          Args[i].getKind() == TemplateArgument::Type) {

        QualType PSA = PSArgs.get(i).getAsType();
        QualType A = Args[i].getAsType();
        if (!PSA->isDependentType()) {
          if (PSA.getCanonicalType() != A.getCanonicalType()) {
            Matches = false;
            break;
          }
        }
      }
    }
    if (Matches)
      return PS;
  }
  return nullptr;
}

clang::QualType TypeCorrectMatcher::SynthesizeContainerType(
    clang::QualType ContainerType, clang::QualType OldType,
    clang::QualType NewValueType, clang::ASTContext *Ctx) {
  const auto *TST = ContainerType->getAs<TemplateSpecializationType>();
  if (!TST)
    return ContainerType;

  llvm::SmallVector<TemplateArgument, 4> NewArgs;
  llvm::SmallVector<TemplateArgument, 4> CanonArgs;

  bool Changed = false;

  for (const auto &Arg : TST->template_arguments()) {
    TemplateArgument LocArg = Arg;

    if (Arg.getKind() == TemplateArgument::Type &&
        Ctx->hasSameType(Arg.getAsType(), OldType)) {

      LocArg = TemplateArgument(NewValueType);
      Changed = true;
    }

    NewArgs.push_back(LocArg);

    if (LocArg.getKind() == TemplateArgument::Type) {
      CanonArgs.push_back(
          TemplateArgument(LocArg.getAsType().getCanonicalType()));
    } else {
      CanonArgs.push_back(LocArg);
    }
  }

  if (!Changed)
    return ContainerType;

  return Ctx->getTemplateSpecializationType(TST->getTemplateName(), NewArgs,
                                            CanonArgs);
}

// Unused stubs
clang::QualType TypeCorrectMatcher::SynthesizeStdFunctionType(
    clang::QualType A, const clang::CXXMethodDecl * /*B*/,
    const std::map<const clang::NamedDecl *, type_correct::NodeState> & /*C*/,
    clang::ASTContext * /*D*/) {
  return A;
}

clang::QualType TypeCorrectMatcher::GetTypeFromExpression(const clang::Expr *E,
                                                          clang::ASTContext *Ctx) {
  if (const auto *DR = dyn_cast<DeclRefExpr>(E)) {
    if (const auto *V = dyn_cast<ValueDecl>(DR->getDecl()))
      return V->getType();
  }
  if (const auto *Cast = dyn_cast<ExplicitCastExpr>(E))
    return GetTypeFromExpression(Cast->getSubExpr(), Ctx);
  if (const auto *CE = dyn_cast<CallExpr>(E))
    return CE->getCallReturnType(*Ctx);
  return {};
}

void TypeCorrectMatcher::HandleMultiDecl(const clang::DeclStmt *DS,
                                         clang::ASTContext *Ctx) {}
void TypeCorrectMatcher::InjectCast(const clang::Expr *ExprToCast,
                                    const clang::QualType &TargetType,
                                    clang::ASTContext *Ctx) {}
void TypeCorrectMatcher::RemoveExplicitCast(const clang::ExplicitCastExpr *Cast,
                                            clang::ASTContext *Ctx) {}
void TypeCorrectMatcher::ScanPrintfArgs(
    const clang::StringLiteral *FormatStr,
    const std::vector<const clang::Expr *> &Args, clang::ASTContext *Ctx) {}
void TypeCorrectMatcher::ScanScanfArgs(
    const clang::StringLiteral *FormatStr,
    const std::vector<const clang::Expr *> &Args, clang::ASTContext *Ctx) {}
void TypeCorrectMatcher::UpdateFormatSpecifiers(
    const clang::NamedDecl *Decl, const clang::QualType &NewType,
    clang::ASTContext *Ctx) {}

//-----------------------------------------------------------------------------
// Consumer Implementation
//-----------------------------------------------------------------------------

TypeCorrectASTConsumer::TypeCorrectASTConsumer(
    clang::Rewriter &Rewriter, bool UseDecltype, bool ExpandAuto,
    std::string ProjectRoot, std::string ExcludePattern, bool InPlace,
    bool EnableAbiBreakingChanges, bool AuditMode,
    type_correct::Phase CurrentPhase, std::string FactsOutputDir,
    std::string ReportFile)
    : Handler(Rewriter, UseDecltype, ExpandAuto, std::move(ProjectRoot),
              std::move(ExcludePattern), InPlace, EnableAbiBreakingChanges,
              AuditMode, CurrentPhase, std::move(FactsOutputDir),
              std::move(ReportFile)) {

  // Register All Matchers
  Finder.addMatcher(
      cxxMemberCallExpr().bind("template_mem_call"), &Handler);
  Finder.addMatcher(
      functionDecl(hasBody(forEachDescendant(
                       memberExpr(member(fieldDecl())).bind("any_access_member"))))
          .bind("enclosing_func"),
      &Handler);
  Finder.addMatcher(arraySubscriptExpr().bind("generic_subscript"), &Handler);
  Finder.addMatcher(
      binaryOperator(hasEitherOperand(hasType(pointerType()))).bind("ptr_arith"),
      &Handler);

  auto TypeSource = expr(anyOf(callExpr(), explicitCastExpr()));

  Finder.addMatcher(
      varDecl(hasInitializer(ignoringParenImpCasts(TypeSource.bind("bound_init_expr"))))
          .bind("bound_var_decl"),
      &Handler);

  Finder.addMatcher(
      binaryOperator(
          hasOperatorName("="),
          hasLHS(ignoringParenImpCasts(anyOf(
              declRefExpr(to(varDecl().bind("bound_assign_var"))),
              memberExpr(member(fieldDecl().bind("bound_assign_field")))))),
          hasRHS(ignoringParenImpCasts(TypeSource.bind("bound_assign_expr"))))
          .bind("bound_assign_op"),
      &Handler);

  Finder.addMatcher(varDecl().bind("bound_var_decl"), &Handler);
  Finder.addMatcher(lambdaExpr().bind("lambda"), &Handler);
  Finder.addMatcher(
      varDecl(hasType(recordDecl(hasName("::std::function"))))
          .bind("std_func_var"),
      &Handler);
}

class TCPluginAction : public clang::PluginASTAction {
public:
  bool UseDecltype = false;
  bool ExpandAuto = false;
  std::string ProjectRoot;
  std::string ExcludePattern;
  bool InPlace = false;
  bool EnableAbiBreakingChanges = false;
  bool AuditMode = false;
  type_correct::Phase CurrentPhase = type_correct::Phase::Standalone;
  std::string FactsOutputDir;
  std::string ReportFile;

  bool ParseArgs(const clang::CompilerInstance &CI,
                 const std::vector<std::string> &args) override {
    for (size_t i = 0; i < args.size(); ++i) {
      if (args[i] == "-use-decltype")
        UseDecltype = true;
      else if (args[i] == "-expand-auto")
        ExpandAuto = true;
      else if (args[i] == "-project-root" && i + 1 < args.size())
        ProjectRoot = args[++i];
      else if (args[i] == "-exclude" && i + 1 < args.size())
        ExcludePattern = args[++i];
      else if (args[i] == "-in-place")
        InPlace = true;
      else if (args[i] == "-enable-abi-breaking-changes")
        EnableAbiBreakingChanges = true;
      else if (args[i] == "-audit")
        AuditMode = true;
      else if (args[i] == "-phase" && i + 1 < args.size()) {
        std::string p = args[++i];
        if (p == "map")
          CurrentPhase = type_correct::Phase::Map;
        else if (p == "apply")
          CurrentPhase = type_correct::Phase::Apply;
        else if (p == "iterative")
          CurrentPhase = type_correct::Phase::Iterative;
        else
          CurrentPhase = type_correct::Phase::Standalone;
      } else if (args[i] == "-facts-dir" && i + 1 < args.size())
        FactsOutputDir = args[++i];
      else if (args[i] == "-report-file" && i + 1 < args.size())
        ReportFile = args[++i];
    }
    return true;
  }
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef file) override {
    RewriterForTC.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<TypeCorrectASTConsumer>(
        RewriterForTC, UseDecltype, ExpandAuto, ProjectRoot, ExcludePattern,
        InPlace, EnableAbiBreakingChanges, AuditMode, CurrentPhase,
        FactsOutputDir, ReportFile);
  }

private:
  clang::Rewriter RewriterForTC;
};

static clang::FrontendPluginRegistry::Add<TCPluginAction>
    X("TypeCorrect", "Type Correction Plugin");