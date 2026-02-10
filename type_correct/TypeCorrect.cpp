/**
 * @file TypeCorrect.cpp
 * @brief Unified Implementation of TypeCorrect Logic.
 *
 * THIS FILE CONTAINS THE FULL DEFINITIONS REQUIRED BY TypeCorrect.h
 */

#include "TypeCorrect.h"

#include <algorithm>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/TemplateBase.h>
#include <clang/AST/TypeLoc.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <cctype>

using namespace clang;
using namespace clang::ast_matchers;

namespace {

TypeLoc GetBaseTypeLoc(TypeLoc TL) {
  while (true) {
    if (auto QTL = TL.getAs<QualifiedTypeLoc>()) {
      TL = QTL.getUnqualifiedLoc();
      continue;
    }
    if (auto ETL = TL.getAs<ElaboratedTypeLoc>()) {
      TL = ETL.getNamedTypeLoc();
      continue;
    }
    if (auto ATL = TL.getAs<AttributedTypeLoc>()) {
      TL = ATL.getModifiedLoc();
      continue;
    }
    if (auto PTL = TL.getAs<PointerTypeLoc>()) {
      TL = PTL.getPointeeLoc();
      continue;
    }
    if (auto RTL = TL.getAs<ReferenceTypeLoc>()) {
      TL = RTL.getPointeeLoc();
      continue;
    }
    if (auto ATL = TL.getAs<ArrayTypeLoc>()) {
      TL = ATL.getElementLoc();
      continue;
    }
    if (auto MPTL = TL.getAs<MemberPointerTypeLoc>()) {
      TL = MPTL.getPointeeLoc();
      continue;
    }
    if (auto PTL = TL.getAs<ParenTypeLoc>()) {
      TL = PTL.getInnerLoc();
      continue;
    }
    break;
  }
  return TL;
}

QualType NormalizeType(QualType T) {
  if (T.isNull())
    return T;
  T = T.getNonReferenceType();
  T = T.getUnqualifiedType();
  return T;
}

QualType GetWiderType(QualType A, QualType B, ASTContext &Ctx) {
  if (A.isNull())
    return B;
  if (B.isNull())
    return A;

  QualType UA = NormalizeType(A);
  QualType UB = NormalizeType(B);

  if (Ctx.hasSameType(UA, UB))
    return A;

  if (UA->isIncompleteType() || UB->isIncompleteType())
    return B;

  if (!UA->isScalarType() || !UB->isScalarType())
    return B;

  uint64_t SizeA = Ctx.getTypeSize(UA);
  uint64_t SizeB = Ctx.getTypeSize(UB);

  if (SizeB > SizeA)
    return B;
  if (SizeA > SizeB)
    return A;

  if (UB->isUnsignedIntegerType() && UA->isSignedIntegerType())
    return B;

  return A;
}

std::string TypeToString(QualType T, ASTContext &Ctx) {
  if (T.isNull())
    return {};
  PrintingPolicy Policy(Ctx.getLangOpts());
  Policy.SuppressTagKeyword = true;
  Policy.UsePreferredNames = true;
  return NormalizeType(T).getAsString(Policy);
}

const NamedDecl *ResolveNamedDecl(const Expr *E) {
  if (!E)
    return nullptr;
  E = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    return dyn_cast<NamedDecl>(DRE->getDecl());
  }
  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    return dyn_cast<NamedDecl>(ME->getMemberDecl());
  }
  return nullptr;
}

bool IsIdentifier(llvm::StringRef Text) {
  if (Text.empty())
    return false;
  auto IsStart = [](unsigned char C) { return std::isalpha(C) || C == '_'; };
  auto IsBody = [](unsigned char C) { return std::isalnum(C) || C == '_'; };
  if (!IsStart(static_cast<unsigned char>(Text.front())))
    return false;
  for (char C : Text.drop_front()) {
    if (!IsBody(static_cast<unsigned char>(C)))
      return false;
  }
  return true;
}

struct DeclUpdate {
  const NamedDecl *Decl = nullptr;
  QualType OriginalType;
  QualType DesiredType;
  SourceRange TypeRange;
  bool IsField = false;
  bool CanRewrite = true;
  bool IsMacroType = false;
  std::string MacroName;
};

struct TemplateArgUpdate {
  const VarDecl *Decl = nullptr;
  QualType OriginalType;
  QualType DesiredType;
  SourceRange ArgRange;
};

class TypeCorrectVisitor : public RecursiveASTVisitor<TypeCorrectVisitor> {
public:
  TypeCorrectVisitor(
      StructAnalyzer &StructEngine, bool ExpandAuto, ASTContext &Ctx,
      llvm::DenseMap<const NamedDecl *, DeclUpdate> &DeclUpdates,
      llvm::DenseMap<const VarDecl *, TemplateArgUpdate> &TemplateUpdates)
      : StructEngine(StructEngine), ExpandAuto(ExpandAuto), Ctx(Ctx),
        SM(Ctx.getSourceManager()), LangOpts(Ctx.getLangOpts()),
        DeclUpdates(DeclUpdates), TemplateUpdates(TemplateUpdates) {}

  bool VisitVarDecl(VarDecl *VD) {
    if (!VD || VD->isImplicit())
      return true;
    if (isa<ParmVarDecl>(VD))
      return true;

    RegisterDecl(VD, false);
    if (VD->hasInit())
      UpdateDeclType(VD, VD->getInit());
    return true;
  }

  bool VisitFieldDecl(FieldDecl *FD) {
    if (!FD || FD->isImplicit())
      return true;
    RegisterDecl(FD, true);
    return true;
  }

  bool VisitBinaryOperator(BinaryOperator *BO) {
    if (!BO || !BO->isAssignmentOp())
      return true;
    const NamedDecl *Target = ResolveNamedDecl(BO->getLHS());
    if (!Target)
      return true;
    UpdateDeclType(Target, BO->getRHS());
    return true;
  }

  bool VisitCXXMemberCallExpr(CXXMemberCallExpr *Call) {
    if (!Call)
      return true;
    const CXXMethodDecl *Method = Call->getMethodDecl();
    if (!Method)
      return true;
    llvm::StringRef MethodName = Method->getName();
    if (!(MethodName == "push_back" || MethodName == "emplace_back"))
      return true;
    if (Call->getNumArgs() < 1)
      return true;

    const Expr *Arg = Call->getArg(0);
    if (Arg)
      Arg = Arg->IgnoreParenImpCasts();
    const Expr *ObjectExpr = Call->getImplicitObjectArgument();
    const NamedDecl *ObjectDecl = ResolveNamedDecl(ObjectExpr);
    const auto *VD = dyn_cast_or_null<VarDecl>(ObjectDecl);
    if (!VD)
      return true;

    TemplateArgUpdate *Update = EnsureTemplateArgUpdate(VD);
    if (!Update)
      return true;
    QualType ArgType = NormalizeType(Arg->getType());
    Update->DesiredType = GetWiderType(Update->DesiredType, ArgType, Ctx);
    return true;
  }

#ifdef TYPE_CORRECT_TEST
  bool TestVisitVarDecl(VarDecl *VD) { return VisitVarDecl(VD); }
  bool TestVisitFieldDecl(FieldDecl *FD) { return VisitFieldDecl(FD); }
  bool TestVisitBinaryOperator(BinaryOperator *BO) {
    return VisitBinaryOperator(BO);
  }
  bool TestVisitCXXMemberCallExpr(CXXMemberCallExpr *Call) {
    return VisitCXXMemberCallExpr(Call);
  }
  DeclUpdate &TestRegisterDecl(const NamedDecl *ND, bool IsField) {
    return RegisterDecl(ND, IsField);
  }
  void TestUpdateDeclType(const NamedDecl *ND, const Expr *RHS) {
    UpdateDeclType(ND, RHS);
  }
  TemplateArgUpdate *TestEnsureTemplateArgUpdate(const VarDecl *VD) {
    return EnsureTemplateArgUpdate(VD);
  }
#endif

private:
  DeclUpdate &RegisterDecl(const NamedDecl *ND, bool IsField) {
    auto It = DeclUpdates.find(ND);
    if (It != DeclUpdates.end())
      return It->second;

    DeclUpdate Update;
    Update.Decl = ND;
    Update.IsField = IsField;

    QualType DeclType;
    const TypeSourceInfo *TSI = nullptr;

    if (const auto *VD = dyn_cast<VarDecl>(ND)) {
      DeclType = VD->getType();
      TSI = VD->getTypeSourceInfo();
    } else if (const auto *FD = dyn_cast<FieldDecl>(ND)) {
      DeclType = FD->getType();
      TSI = FD->getTypeSourceInfo();
    }

    Update.OriginalType = NormalizeType(DeclType);
    Update.DesiredType = Update.OriginalType;

    if (!TSI) {
      Update.CanRewrite = false;
      DeclUpdates[ND] = Update;
      return DeclUpdates[ND];
    }

    TypeLoc BaseLoc = GetBaseTypeLoc(TSI->getTypeLoc());
    Update.TypeRange = BaseLoc.getSourceRange();
    Update.IsMacroType =
        BaseLoc.getBeginLoc().isMacroID() || BaseLoc.getEndLoc().isMacroID();

    if (Update.IsMacroType && !Update.TypeRange.isInvalid()) {
      std::string Text =
          Lexer::getSourceText(
              CharSourceRange::getTokenRange(Update.TypeRange), SM, LangOpts)
              .str();
      llvm::StringRef Trimmed = llvm::StringRef(Text).trim();
      if (IsIdentifier(Trimmed))
        Update.MacroName = Trimmed.str();
    }

    if (BaseLoc.getAs<AutoTypeLoc>() && !ExpandAuto) {
      Update.CanRewrite = false;
    }

    if (IsField) {
      const auto *FD = dyn_cast<FieldDecl>(ND);
      Update.CanRewrite = Update.CanRewrite && FD &&
                          StructEngine.CanRewriteField(FD, SM);
    } else {
      Update.CanRewrite = Update.CanRewrite &&
                          !StructEngine.IsBoundaryFixed(ND, SM);
    }

    DeclUpdates[ND] = Update;
    return DeclUpdates[ND];
  }

  void UpdateDeclType(const NamedDecl *ND, const Expr *RHS) {
    if (!ND || !RHS)
      return;
    auto It = DeclUpdates.find(ND);
    if (It == DeclUpdates.end()) {
      RegisterDecl(ND, isa<FieldDecl>(ND));
      It = DeclUpdates.find(ND);
    }

    const Expr *BaseExpr = RHS->IgnoreParenImpCasts();
    QualType Candidate =
        NormalizeType(BaseExpr ? BaseExpr->getType() : RHS->getType());
    if (Candidate.isNull())
      return;
    It->second.DesiredType =
        GetWiderType(It->second.DesiredType, Candidate, Ctx);
  }

  TemplateArgUpdate *EnsureTemplateArgUpdate(const VarDecl *VD) {
    auto It = TemplateUpdates.find(VD);
    if (It != TemplateUpdates.end())
      return &It->second;

    const TypeSourceInfo *TSI = VD->getTypeSourceInfo();
    if (!TSI)
      return nullptr;

    TypeLoc BaseLoc = GetBaseTypeLoc(TSI->getTypeLoc());
    auto TSTL = BaseLoc.getAs<TemplateSpecializationTypeLoc>();
    if (!TSTL || TSTL.getNumArgs() < 1)
      return nullptr;

    TemplateArgumentLoc ArgLoc = TSTL.getArgLoc(0);
    if (ArgLoc.getArgument().getKind() != TemplateArgument::Type)
      return nullptr;

    const TypeSourceInfo *ArgTSI = ArgLoc.getTypeSourceInfo();
    if (!ArgTSI)
      return nullptr;

    TypeLoc ArgTypeLoc = GetBaseTypeLoc(ArgTSI->getTypeLoc());
    SourceRange ArgRange = ArgTypeLoc.getSourceRange();
    if (ArgRange.isInvalid())
      return nullptr;

    TemplateArgUpdate Update;
    Update.Decl = VD;
    Update.OriginalType = NormalizeType(ArgLoc.getArgument().getAsType());
    Update.DesiredType = Update.OriginalType;
    Update.ArgRange = ArgRange;

    TemplateUpdates[VD] = Update;
    return &TemplateUpdates[VD];
  }

  StructAnalyzer &StructEngine;
  bool ExpandAuto;
  ASTContext &Ctx;
  SourceManager &SM;
  const LangOptions &LangOpts;
  llvm::DenseMap<const NamedDecl *, DeclUpdate> &DeclUpdates;
  llvm::DenseMap<const VarDecl *, TemplateArgUpdate> &TemplateUpdates;
};

void RecordChange(std::vector<type_correct::ChangeRecord> &Changes,
                  SourceManager &SM, ASTContext &Ctx, const NamedDecl *Decl,
                  QualType OldType, QualType NewType) {
  if (!Decl)
    return;
  SourceLocation Loc = Decl->getLocation();
  if (Loc.isInvalid())
    return;
  PresumedLoc PLoc = SM.getPresumedLoc(Loc);
  if (!PLoc.isValid())
    return;
  type_correct::ChangeRecord Record;
  Record.FilePath = std::string(PLoc.getFilename());
  Record.Line = PLoc.getLine();
  Record.Symbol = Decl->getNameAsString();
  Record.OldType = TypeToString(OldType, Ctx);
  Record.NewType = TypeToString(NewType, Ctx);
  Changes.push_back(std::move(Record));
}

void ApplyTemplateUpdates(
    clang::Rewriter &Rewriter, bool AuditMode,
    std::vector<type_correct::ChangeRecord> &Changes, ASTContext &Ctx,
    llvm::DenseMap<const NamedDecl *, DeclUpdate> &DeclUpdates,
    llvm::DenseMap<const VarDecl *, TemplateArgUpdate> &TemplateUpdates) {
  SourceManager &SM = Rewriter.getSourceMgr();

  for (auto &Entry : TemplateUpdates) {
    TemplateArgUpdate &Update = Entry.second;
    if (Update.OriginalType.isNull() || Update.DesiredType.isNull())
      continue;
    if (Ctx.hasSameType(Update.OriginalType, Update.DesiredType))
      continue;

    auto DeclIt = DeclUpdates.find(Update.Decl);
    if (DeclIt != DeclUpdates.end() && !DeclIt->second.CanRewrite)
      continue;

    SourceLocation Begin = Update.ArgRange.getBegin();
    SourceLocation End = Update.ArgRange.getEnd();
    if (Begin.isInvalid() || End.isInvalid())
      continue;
    if (Begin.isMacroID() || End.isMacroID())
      continue;

    RecordChange(Changes, SM, Ctx, Update.Decl, Update.OriginalType,
                 Update.DesiredType);

    if (!AuditMode) {
      Rewriter.ReplaceText(CharSourceRange::getTokenRange(Update.ArgRange),
                           TypeToString(Update.DesiredType, Ctx));
    }
  }
}

std::map<std::string, QualType> CollectMacroUpdates(
    clang::Rewriter &Rewriter, bool AuditMode,
    std::vector<type_correct::ChangeRecord> &Changes, ASTContext &Ctx,
    llvm::DenseMap<const NamedDecl *, DeclUpdate> &DeclUpdates) {
  SourceManager &SM = Rewriter.getSourceMgr();
  std::map<std::string, QualType> MacroUpdates;

  for (auto &Entry : DeclUpdates) {
    DeclUpdate &Update = Entry.second;
    if (Update.OriginalType.isNull() || Update.DesiredType.isNull())
      continue;
    if (Ctx.hasSameType(Update.OriginalType, Update.DesiredType))
      continue;
    if (!Update.CanRewrite)
      continue;

    if (Update.IsMacroType && !Update.MacroName.empty()) {
      auto It = MacroUpdates.find(Update.MacroName);
      if (It == MacroUpdates.end()) {
        MacroUpdates[Update.MacroName] = Update.DesiredType;
      } else {
        It->second = GetWiderType(It->second, Update.DesiredType, Ctx);
      }
      RecordChange(Changes, SM, Ctx, Update.Decl, Update.OriginalType,
                   Update.DesiredType);
      continue;
    }

    SourceLocation Begin = Update.TypeRange.getBegin();
    SourceLocation End = Update.TypeRange.getEnd();
    if (Begin.isInvalid() || End.isInvalid())
      continue;
    if (Begin.isMacroID() || End.isMacroID())
      continue;

    RecordChange(Changes, SM, Ctx, Update.Decl, Update.OriginalType,
                 Update.DesiredType);

    if (!AuditMode) {
      Rewriter.ReplaceText(CharSourceRange::getTokenRange(Update.TypeRange),
                           TypeToString(Update.DesiredType, Ctx));
    }
  }

  return MacroUpdates;
}

void ApplyMacroUpdates(clang::Rewriter &Rewriter, bool AuditMode,
                       ASTContext &Ctx,
                       const std::map<std::string, QualType> &MacroUpdates) {
  if (MacroUpdates.empty())
    return;

  SourceManager &SM = Rewriter.getSourceMgr();
  FileID MainFile = SM.getMainFileID();
  llvm::StringRef Buffer = SM.getBufferData(MainFile);
  SourceLocation FileStart = SM.getLocForStartOfFile(MainFile);

  auto IsSpace = [](char C) {
    return C == ' ' || C == '\t' || C == '\r' || C == '\f' || C == '\v';
  };

  size_t Offset = 0;
  while (Offset <= Buffer.size()) {
    size_t LineEnd = Buffer.find('\n', Offset);
    if (LineEnd == llvm::StringRef::npos)
      LineEnd = Buffer.size();

    llvm::StringRef Line = Buffer.slice(Offset, LineEnd);
    size_t Pos = 0;
    while (Pos < Line.size() && IsSpace(Line[Pos]))
      ++Pos;

    static const llvm::StringRef DefineTok("#define");
    if (Line.substr(Pos).starts_with(DefineTok)) {
      Pos += DefineTok.size();
      if (Pos < Line.size() && !IsSpace(Line[Pos])) {
        Offset = (LineEnd == Buffer.size()) ? Buffer.size() : LineEnd + 1;
        continue;
      }
      while (Pos < Line.size() && IsSpace(Line[Pos]))
        ++Pos;

      size_t NameStart = Pos;
      while (Pos < Line.size() &&
             (std::isalnum(static_cast<unsigned char>(Line[Pos])) ||
              Line[Pos] == '_')) {
        ++Pos;
      }

      if (NameStart == Pos) {
        Offset = (LineEnd == Buffer.size()) ? Buffer.size() : LineEnd + 1;
        continue;
      }

      std::string MacroName = Line.substr(NameStart, Pos - NameStart).str();
      if (Pos < Line.size() && Line[Pos] == '(') {
        Offset = (LineEnd == Buffer.size()) ? Buffer.size() : LineEnd + 1;
        continue;
      }

      auto UpdateIt = MacroUpdates.find(MacroName);
      if (UpdateIt == MacroUpdates.end()) {
        Offset = (LineEnd == Buffer.size()) ? Buffer.size() : LineEnd + 1;
        continue;
      }

      while (Pos < Line.size() && IsSpace(Line[Pos]))
        ++Pos;

      size_t ReplStart = Pos;
      if (ReplStart >= Line.size()) {
        Offset = (LineEnd == Buffer.size()) ? Buffer.size() : LineEnd + 1;
        continue;
      }

      size_t ReplEnd = Line.size();
      size_t LineComment = Line.find("//", ReplStart);
      if (LineComment != llvm::StringRef::npos)
        ReplEnd = std::min(ReplEnd, LineComment);
      size_t BlockComment = Line.find("/*", ReplStart);
      if (BlockComment != llvm::StringRef::npos)
        ReplEnd = std::min(ReplEnd, BlockComment);
      while (ReplEnd > ReplStart && IsSpace(Line[ReplEnd - 1]))
        --ReplEnd;

      if (ReplEnd <= ReplStart) {
        Offset = (LineEnd == Buffer.size()) ? Buffer.size() : LineEnd + 1;
        continue;
      }

      std::string NewText = TypeToString(UpdateIt->second, Ctx);
      llvm::StringRef Existing = Line.substr(ReplStart, ReplEnd - ReplStart);
      if (Existing.trim() == NewText) {
        Offset = (LineEnd == Buffer.size()) ? Buffer.size() : LineEnd + 1;
        continue;
      }

      if (!AuditMode) {
        SourceLocation ReplaceStart =
            FileStart.getLocWithOffset(Offset + ReplStart);
        Rewriter.ReplaceText(ReplaceStart, ReplEnd - ReplStart, NewText);
      }
    }

    if (LineEnd == Buffer.size())
      break;
    Offset = LineEnd + 1;
  }
}

} // namespace

//------------------------------------------------------------------------------
// ✅ VTABLE ANCHOR
//------------------------------------------------------------------------------
TypeCorrectMatcher::~TypeCorrectMatcher() {}

//------------------------------------------------------------------------------
// ✅ CONSTRUCTOR (REQUIRED)
//------------------------------------------------------------------------------
TypeCorrectMatcher::TypeCorrectMatcher(
    clang::Rewriter &Rewriter,
    bool UseDecltype,
    bool ExpandAuto,
    std::string ProjectRoot,
    std::string ExcludePattern,
    bool InPlace,
    bool EnableAbiBreakingChanges,
    bool AuditMode,
    type_correct::Phase CurrentPhase,
    std::string FactsOutputDir,
    std::string ReportFile)
    : Rewriter(Rewriter),
      UseDecltype(UseDecltype),
      ExpandAuto(ExpandAuto),
      ProjectRoot(std::move(ProjectRoot)),
      ExcludePattern(std::move(ExcludePattern)),
      InPlace(InPlace),
      AuditMode(AuditMode),
      StructEngine(EnableAbiBreakingChanges, false, ProjectRoot),
      CurrentPhase(CurrentPhase),
      FactsOutputDir(std::move(FactsOutputDir)),
      ReportFile(std::move(ReportFile)) {}

std::vector<type_correct::ChangeRecord> TypeCorrectMatcher::GetChanges() const {
  return Changes;
}

//------------------------------------------------------------------------------
// ✅ Match callback (REQUIRED VIRTUAL)
//------------------------------------------------------------------------------
void TypeCorrectMatcher::run(
    const MatchFinder::MatchResult &Result) {

  ASTContext *Ctx = Result.Context;
  if (!Ctx)
    return;

  if (const auto *Cast =
          Result.Nodes.getNodeAs<ExplicitCastExpr>("explicit_cast")) {
    ExplicitCasts.push_back(Cast);
  }
}

void TypeCorrectMatcher::ProcessRedundantCasts(ASTContext &Ctx) {
  if (ExplicitCasts.empty())
    return;

  SourceManager &SM = Rewriter.getSourceMgr();
  const LangOptions &LangOpts = Ctx.getLangOpts();
  llvm::SmallPtrSet<const ExplicitCastExpr *, 32> Seen;

  for (const auto *Cast : ExplicitCasts) {
    if (!Cast || !Seen.insert(Cast).second)
      continue;

    SourceLocation Begin = Cast->getBeginLoc();
    SourceLocation End = Cast->getEndLoc();
    if (Begin.isInvalid() || End.isInvalid())
      continue;
    if (Begin.isMacroID() || End.isMacroID())
      continue;
    if (SM.getFileID(Begin) != SM.getMainFileID())
      continue;

    const Expr *SubExpr = Cast->getSubExpr();
    if (!SubExpr)
      continue;

    SourceLocation SubBegin = SubExpr->getBeginLoc();
    SourceLocation SubEnd = SubExpr->getEndLoc();
    if (SubBegin.isInvalid() || SubEnd.isInvalid())
      continue;
    if (SubBegin.isMacroID() || SubEnd.isMacroID())
      continue;

    QualType CastType = NormalizeType(Cast->getTypeAsWritten());
    QualType SubType = NormalizeType(SubExpr->getType());
    if (CastType.isNull() || SubType.isNull())
      continue;
    if (!Ctx.hasSameType(CastType, SubType))
      continue;

    std::string SubText =
        Lexer::getSourceText(
            CharSourceRange::getTokenRange(SubExpr->getSourceRange()), SM,
            LangOpts)
            .str();
    if (SubText.empty())
      continue;

    if (!AuditMode) {
      Rewriter.ReplaceText(
          CharSourceRange::getTokenRange(Cast->getSourceRange()), SubText);
    }
  }

  ExplicitCasts.clear();
}

void TypeCorrectMatcher::ProcessNarrowingSafety(ASTContext &Ctx) {
  llvm::DenseMap<const NamedDecl *, DeclUpdate> DeclUpdates;
  llvm::DenseMap<const VarDecl *, TemplateArgUpdate> TemplateUpdates;

  TypeCorrectVisitor Visitor(StructEngine, ExpandAuto, Ctx, DeclUpdates,
                             TemplateUpdates);
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());

  ApplyTemplateUpdates(Rewriter, AuditMode, Changes, Ctx, DeclUpdates,
                       TemplateUpdates);

  std::map<std::string, QualType> MacroUpdates =
      CollectMacroUpdates(Rewriter, AuditMode, Changes, Ctx, DeclUpdates);
  if (MacroUpdates.empty())
    return;

  ApplyMacroUpdates(Rewriter, AuditMode, Ctx, MacroUpdates);
}

//------------------------------------------------------------------------------
// ✅ End-of-TU hook (REQUIRED VIRTUAL)
//------------------------------------------------------------------------------
void TypeCorrectMatcher::onEndOfTranslationUnit(ASTContext &Ctx) {
  ProcessRedundantCasts(Ctx);
  ProcessNarrowingSafety(Ctx);

  SourceManager &SM = Rewriter.getSourceMgr();

  if (!InPlace) {
    if (const llvm::RewriteBuffer *Buf =
            Rewriter.getRewriteBufferFor(SM.getMainFileID())) {
      Buf->write(llvm::outs());
    } else {
      llvm::outs() << SM.getBufferData(SM.getMainFileID());
    }
    llvm::outs().flush();
  } else {
    Rewriter.overwriteChangedFiles();
  }
}

//------------------------------------------------------------------------------
// ✅ ASTConsumer constructor
//------------------------------------------------------------------------------
TypeCorrectASTConsumer::TypeCorrectASTConsumer(
    clang::Rewriter &Rewriter,
    bool UseDecltype,
    bool ExpandAuto,
    const std::string &ProjectRoot,
    const std::string &ExcludePattern,
    bool InPlace,
    bool EnableAbiBreakingChanges,
    bool AuditMode,
    type_correct::Phase CurrentPhase,
    const std::string &FactsOutputDir,
    const std::string &ReportFile)
    : Handler(Rewriter,
              UseDecltype,
              ExpandAuto,
              ProjectRoot,
              ExcludePattern,
              InPlace,
              EnableAbiBreakingChanges,
              AuditMode,
              CurrentPhase,
              FactsOutputDir,
              ReportFile) {

  Finder.addMatcher(explicitCastExpr().bind("explicit_cast"), &Handler);
}

//------------------------------------------------------------------------------
// ✅ ASTConsumer override
//------------------------------------------------------------------------------
void TypeCorrectASTConsumer::HandleTranslationUnit(ASTContext &Ctx) {
  Finder.matchAST(Ctx);
  Handler.onEndOfTranslationUnit(Ctx);
}

#ifdef TYPE_CORRECT_TEST
namespace type_correct::test_support {

TypeLoc GetBaseTypeLocForTest(TypeLoc TL) { return GetBaseTypeLoc(TL); }

QualType NormalizeTypeForTest(QualType T) { return NormalizeType(T); }

QualType GetWiderTypeForTest(QualType A, QualType B, ASTContext &Ctx) {
  return GetWiderType(A, B, Ctx);
}

std::string TypeToStringForTest(QualType T, ASTContext &Ctx) {
  return TypeToString(T, Ctx);
}

const NamedDecl *ResolveNamedDeclForTest(const Expr *E) {
  return ResolveNamedDecl(E);
}

bool IsIdentifierForTest(llvm::StringRef Text) { return IsIdentifier(Text); }

void CoverVisitorEdgeCases(ASTContext &Ctx) {
  StructAnalyzer Engine(true, true, "");
  llvm::DenseMap<const NamedDecl *, DeclUpdate> DeclUpdates;
  llvm::DenseMap<const VarDecl *, TemplateArgUpdate> TemplateUpdates;
  TypeCorrectVisitor Visitor(Engine, false, Ctx, DeclUpdates, TemplateUpdates);

  Visitor.TestVisitVarDecl(nullptr);
  Visitor.TestVisitFieldDecl(nullptr);
  Visitor.TestVisitBinaryOperator(nullptr);
  Visitor.TestVisitCXXMemberCallExpr(nullptr);

  IdentifierInfo &Id = Ctx.Idents.get("tc_edge_var");
  VarDecl *VD = VarDecl::Create(
      Ctx, Ctx.getTranslationUnitDecl(), SourceLocation(), SourceLocation(), &Id,
      Ctx.IntTy, Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), SC_None);
  Visitor.TestRegisterDecl(VD, false);
  Visitor.TestRegisterDecl(VD, false);

  IdentifierInfo &NoTsiId = Ctx.Idents.get("tc_no_tsi");
  VarDecl *NoTsi = VarDecl::Create(Ctx, Ctx.getTranslationUnitDecl(),
                                   SourceLocation(), SourceLocation(), &NoTsiId,
                                   Ctx.IntTy, nullptr, SC_None);
  NoTsi->setTypeSourceInfo(nullptr);
  Visitor.TestRegisterDecl(NoTsi, false);

  auto *Literal =
      IntegerLiteral::Create(Ctx, llvm::APInt(32, 1), Ctx.IntTy, SourceLocation());
  Visitor.TestUpdateDeclType(nullptr, Literal);
  Visitor.TestUpdateDeclType(VD, nullptr);

  IdentifierInfo &NewId = Ctx.Idents.get("tc_update_decl");
  VarDecl *NewDecl = VarDecl::Create(
      Ctx, Ctx.getTranslationUnitDecl(), SourceLocation(), SourceLocation(),
      &NewId, Ctx.IntTy, Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), SC_None);
  Visitor.TestUpdateDeclType(NewDecl, Literal);

  auto *Opaque =
      new (Ctx) OpaqueValueExpr(SourceLocation(), QualType(), VK_RValue);
  Visitor.TestUpdateDeclType(VD, Opaque);
}

void CoverEnsureTemplateArgUpdateEdges(ASTContext &Ctx,
                                       const VarDecl *TemplateDecl,
                                       const VarDecl *NonTemplateDecl,
                                       const VarDecl *NonTypeTemplateDecl,
                                       VarDecl *TrivialTemplateDecl) {
  StructAnalyzer Engine(true, true, "");
  llvm::DenseMap<const NamedDecl *, DeclUpdate> DeclUpdates;
  llvm::DenseMap<const VarDecl *, TemplateArgUpdate> TemplateUpdates;
  TypeCorrectVisitor Visitor(Engine, false, Ctx, DeclUpdates, TemplateUpdates);

  if (TemplateDecl) {
    Visitor.TestEnsureTemplateArgUpdate(TemplateDecl);
    Visitor.TestEnsureTemplateArgUpdate(TemplateDecl);
  }

  IdentifierInfo &NoTsiId = Ctx.Idents.get("tc_template_no_tsi");
  VarDecl *NoTsi = VarDecl::Create(Ctx, Ctx.getTranslationUnitDecl(),
                                   SourceLocation(), SourceLocation(), &NoTsiId,
                                   Ctx.IntTy, nullptr, SC_None);
  NoTsi->setTypeSourceInfo(nullptr);
  Visitor.TestEnsureTemplateArgUpdate(NoTsi);

  if (NonTemplateDecl)
    Visitor.TestEnsureTemplateArgUpdate(NonTemplateDecl);

  if (NonTypeTemplateDecl)
    Visitor.TestEnsureTemplateArgUpdate(NonTypeTemplateDecl);

  if (TrivialTemplateDecl) {
    TrivialTemplateDecl->setTypeSourceInfo(
        Ctx.getTrivialTypeSourceInfo(TrivialTemplateDecl->getType()));
    Visitor.TestEnsureTemplateArgUpdate(TrivialTemplateDecl);
  }
}

void CoverTemplateUpdateMapEdges(ASTContext &Ctx, Rewriter &Rewriter) {
  llvm::DenseMap<const NamedDecl *, DeclUpdate> DeclUpdates;
  llvm::DenseMap<const VarDecl *, TemplateArgUpdate> TemplateUpdates;
  std::vector<type_correct::ChangeRecord> Changes;
  SourceManager &SM = Rewriter.getSourceMgr();
  SourceLocation FileLoc = SM.getLocForStartOfFile(SM.getMainFileID());
  SourceLocation MacroLoc = SM.createExpansionLoc(FileLoc, FileLoc, FileLoc, 1);

  auto MakeVar = [&](const char *Name) {
    IdentifierInfo &Id = Ctx.Idents.get(Name);
    return VarDecl::Create(Ctx, Ctx.getTranslationUnitDecl(), FileLoc, FileLoc,
                           &Id, Ctx.IntTy,
                           Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), SC_None);
  };

  const VarDecl *NullTypes = MakeVar("tc_tpl_null");
  TemplateUpdates[NullTypes] = TemplateArgUpdate{
      NullTypes, QualType(), Ctx.IntTy, SourceRange(FileLoc, FileLoc)};

  const VarDecl *SameTypes = MakeVar("tc_tpl_same");
  TemplateUpdates[SameTypes] = TemplateArgUpdate{
      SameTypes, Ctx.IntTy, Ctx.IntTy, SourceRange(FileLoc, FileLoc)};

  const VarDecl *NoRewrite = MakeVar("tc_tpl_norewrite");
  DeclUpdate NoRewriteUpdate;
  NoRewriteUpdate.Decl = NoRewrite;
  NoRewriteUpdate.OriginalType = Ctx.IntTy;
  NoRewriteUpdate.DesiredType = Ctx.LongTy;
  NoRewriteUpdate.TypeRange = SourceRange(FileLoc, FileLoc);
  NoRewriteUpdate.CanRewrite = false;
  DeclUpdates[NoRewrite] = NoRewriteUpdate;
  TemplateUpdates[NoRewrite] = TemplateArgUpdate{
      NoRewrite, Ctx.IntTy, Ctx.LongTy, SourceRange(FileLoc, FileLoc)};

  const VarDecl *InvalidRange = MakeVar("tc_tpl_invalid");
  TemplateUpdates[InvalidRange] =
      TemplateArgUpdate{InvalidRange, Ctx.IntTy, Ctx.LongTy, SourceRange()};

  const VarDecl *MacroRange = MakeVar("tc_tpl_macro");
  TemplateUpdates[MacroRange] = TemplateArgUpdate{
      MacroRange, Ctx.IntTy, Ctx.LongTy, SourceRange(MacroLoc, MacroLoc)};

  ApplyTemplateUpdates(Rewriter, true, Changes, Ctx, DeclUpdates,
                       TemplateUpdates);
}

void CoverDeclUpdateMapEdges(ASTContext &Ctx, Rewriter &Rewriter) {
  llvm::DenseMap<const NamedDecl *, DeclUpdate> DeclUpdates;
  std::vector<type_correct::ChangeRecord> Changes;
  SourceManager &SM = Rewriter.getSourceMgr();
  SourceLocation FileLoc = SM.getLocForStartOfFile(SM.getMainFileID());

  auto MakeVar = [&](const char *Name) {
    IdentifierInfo &Id = Ctx.Idents.get(Name);
    return VarDecl::Create(Ctx, Ctx.getTranslationUnitDecl(), FileLoc, FileLoc,
                           &Id, Ctx.IntTy,
                           Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), SC_None);
  };

  const VarDecl *NullTypes = MakeVar("tc_decl_null");
  DeclUpdate NullUpdate;
  NullUpdate.Decl = NullTypes;
  NullUpdate.OriginalType = QualType();
  NullUpdate.DesiredType = Ctx.IntTy;
  NullUpdate.TypeRange = SourceRange(FileLoc, FileLoc);
  DeclUpdates[NullTypes] = NullUpdate;

  const VarDecl *MacroA = MakeVar("tc_macro_a");
  DeclUpdate MacroUpdateA;
  MacroUpdateA.Decl = MacroA;
  MacroUpdateA.OriginalType = Ctx.IntTy;
  MacroUpdateA.DesiredType = Ctx.LongLongTy;
  MacroUpdateA.IsMacroType = true;
  MacroUpdateA.MacroName = "MACRO";
  DeclUpdates[MacroA] = MacroUpdateA;

  const VarDecl *MacroB = MakeVar("tc_macro_b");
  DeclUpdate MacroUpdateB = MacroUpdateA;
  MacroUpdateB.Decl = MacroB;
  MacroUpdateB.DesiredType = Ctx.LongTy;
  DeclUpdates[MacroB] = MacroUpdateB;

  const VarDecl *InvalidRange = MakeVar("tc_decl_invalid");
  DeclUpdate InvalidUpdate;
  InvalidUpdate.Decl = InvalidRange;
  InvalidUpdate.OriginalType = Ctx.IntTy;
  InvalidUpdate.DesiredType = Ctx.LongTy;
  InvalidUpdate.TypeRange = SourceRange();
  DeclUpdates[InvalidRange] = InvalidUpdate;

  (void)CollectMacroUpdates(Rewriter, true, Changes, Ctx, DeclUpdates);
}

bool CoverMacroScannerEdges(ASTContext &Ctx, Rewriter &Rewriter) {
  SourceManager &SM = Rewriter.getSourceMgr();
  FileID MainFile = SM.getMainFileID();
  auto EntryRef = SM.getFileEntryRefForID(MainFile);
  if (!EntryRef)
    return false;

  std::string NewText = TypeToString(Ctx.getSizeType(), Ctx);
  std::string Buffer;
  Buffer += "  #defineFOO 1\n";
  Buffer += "#define   \n";
  Buffer += "#define FUNC(x) int\n";
  Buffer += "#define OTHER int\n";
  Buffer += "#define UPDATE\n";
  Buffer += "#define UPDATE2 /* comment */\n";
  Buffer += "#define UPDATE3 int // comment\n";
  Buffer += "#define UPDATE4 int /* comment */\n";
  Buffer += "#define MATCH " + NewText + "\n";
  auto Mem = llvm::MemoryBuffer::getMemBuffer(Buffer, "input.cc");
  SM.overrideFileContents(*EntryRef, std::move(Mem));

  std::map<std::string, QualType> MacroUpdates;
  MacroUpdates["UPDATE"] = Ctx.getSizeType();
  MacroUpdates["UPDATE2"] = Ctx.getSizeType();
  MacroUpdates["UPDATE3"] = Ctx.getSizeType();
  MacroUpdates["UPDATE4"] = Ctx.getSizeType();
  MacroUpdates["MATCH"] = Ctx.getSizeType();

  ApplyMacroUpdates(Rewriter, true, Ctx, MacroUpdates);
  return true;
}

void CoverRecordChangeEdges(ASTContext &Ctx, Rewriter &Rewriter) {
  std::vector<type_correct::ChangeRecord> Changes;
  SourceManager &SM = Rewriter.getSourceMgr();

  RecordChange(Changes, SM, Ctx, nullptr, Ctx.IntTy, Ctx.LongTy);

  IdentifierInfo &BadId = Ctx.Idents.get("tc_badloc");
  VarDecl *BadDecl =
      VarDecl::Create(Ctx, Ctx.getTranslationUnitDecl(), SourceLocation(),
                      SourceLocation(), &BadId, Ctx.IntTy,
                      Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), SC_None);
  RecordChange(Changes, SM, Ctx, BadDecl, Ctx.IntTy, Ctx.LongTy);

  IdentifierInfo &FakeId = Ctx.Idents.get("tc_fakeloc");
  SourceLocation FakeLoc = SourceLocation::getFromRawEncoding(1);
  VarDecl *FakeDecl = VarDecl::Create(
      Ctx, Ctx.getTranslationUnitDecl(), FakeLoc, FakeLoc, &FakeId, Ctx.IntTy,
      Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), SC_None);
  RecordChange(Changes, SM, Ctx, FakeDecl, Ctx.IntTy, Ctx.LongTy);
}

} // namespace type_correct::test_support
#endif
