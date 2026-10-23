/**
 * @file TypeCorrect.cpp
 * @brief Unified Implementation of TypeCorrect Logic.
 *
 * THIS FILE CONTAINS THE FULL DEFINITIONS REQUIRED BY TypeCorrect.h
 */

#include "TypeCorrect.h"
#include "ClangCompat.h"

#include <algorithm>
#include <cctype>
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

using namespace clang;
using namespace clang::ast_matchers;

namespace {

TypeLoc GetBaseTypeLoc(TypeLoc TL) noexcept {
  while (true) {
    if (auto QTL = TL.getAs<QualifiedTypeLoc>()) {
      TL = QTL.getUnqualifiedLoc();
      continue;
    }
#if LLVM_VERSION_MAJOR < 22
    if (auto ETL = TL.getAs<clang::ElaboratedTypeLoc>()) {
      TL = ETL.getNamedTypeLoc();
      continue;
    }
#endif
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

QualType NormalizeType(QualType T) noexcept {
  if (T.isNull())
    return T;
  T = T.getNonReferenceType();
  T = T.getUnqualifiedType();
  return T;
}

QualType GetWiderType(QualType A, QualType B, ASTContext &Ctx) noexcept {
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

std::string TypeToString(QualType T, ASTContext &Ctx) noexcept {
  if (T.isNull())
    return {};
  PrintingPolicy Policy(Ctx.getLangOpts());
  Policy.SuppressTagKeyword = true;
  Policy.UsePreferredNames = true;
  return NormalizeType(T).getAsString(Policy);
}

std::optional<const NamedDecl *> ResolveNamedDecl(const Expr *E) noexcept {
  if (!E)
    return std::nullopt;
  E = E->IgnoreParenImpCasts();
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    return dyn_cast<NamedDecl>(DRE->getDecl());
  }
  if (const auto *ME = dyn_cast<MemberExpr>(E)) {
    return dyn_cast<NamedDecl>(ME->getMemberDecl());
  }
  return std::nullopt;
}

bool IsIdentifier(llvm::StringRef Text) noexcept {
  if (Text.empty())
    return false;
  auto IsStart = [](unsigned char C) noexcept {
    return std::isalpha(C) || C == '_';
  };
  auto IsBody = [](unsigned char C) noexcept {
    return std::isalnum(C) || C == '_';
  };
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
  TypeCorrectVisitor(StructAnalyzer &StructEngine, bool ExpandAuto,
                     ASTContext &Ctx,
                     llvm::DenseMap<const NamedDecl *, DeclUpdate> &DeclUpdates,
                     llvm::DenseMap<const VarDecl *, TemplateArgUpdate>
                         &TemplateUpdates) noexcept
      : StructEngine(StructEngine), ExpandAuto(ExpandAuto), Ctx(Ctx),
        SM(Ctx.getSourceManager()), LangOpts(Ctx.getLangOpts()),
        DeclUpdates(DeclUpdates), TemplateUpdates(TemplateUpdates) {}

  bool VisitVarDecl(VarDecl *VD) noexcept {
    if (!VD || VD->isImplicit())
      return true;
    if (isa<ParmVarDecl>(VD))
      return true;

    RegisterDecl(VD, false);
    if (VD->hasInit())
      UpdateDeclType(VD, VD->getInit());
    return true;
  }

  bool VisitFieldDecl(FieldDecl *FD) noexcept {
    if (!FD || FD->isImplicit())
      return true;
    RegisterDecl(FD, true);
    return true;
  }

  bool VisitBinaryOperator(BinaryOperator *BO) noexcept {
    if (!BO || !BO->isAssignmentOp())
      return true;
    auto TargetOpt = ResolveNamedDecl(BO->getLHS());
    if (!TargetOpt)
      return true;
    const NamedDecl *Target = *TargetOpt;
    UpdateDeclType(Target, BO->getRHS());
    return true;
  }

  bool VisitCXXMemberCallExpr(CXXMemberCallExpr *Call) noexcept {
    if (!Call)
      return true;
    const CXXMethodDecl *Method = Call->getMethodDecl();
    llvm::StringRef MethodName = Method->getName();
    if (!(MethodName == "push_back" || MethodName == "emplace_back"))
      return true;
    if (Call->getNumArgs() < 1)
      return true;

    const Expr *Arg = Call->getArg(0)->IgnoreParenImpCasts();
    const Expr *ObjectExpr = Call->getImplicitObjectArgument();
    auto ObjectDeclOpt = ResolveNamedDecl(ObjectExpr);
    if (!ObjectDeclOpt)
      return true;
    const NamedDecl *ObjectDecl = *ObjectDeclOpt;
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
  bool TestVisitVarDecl(VarDecl *VD) noexcept { return VisitVarDecl(VD); }
  bool TestVisitFieldDecl(FieldDecl *FD) noexcept { return VisitFieldDecl(FD); }
  bool TestVisitBinaryOperator(BinaryOperator *BO) noexcept {
    return VisitBinaryOperator(BO);
  }
  bool TestVisitCXXMemberCallExpr(CXXMemberCallExpr *Call) noexcept {
    return VisitCXXMemberCallExpr(Call);
  }
  DeclUpdate &TestRegisterDecl(const NamedDecl *ND, bool IsField) noexcept {
    return RegisterDecl(ND, IsField);
  }
  void TestUpdateDeclType(const NamedDecl *ND, const Expr *RHS) noexcept {
    UpdateDeclType(ND, RHS);
  }
  TemplateArgUpdate *TestEnsureTemplateArgUpdate(const VarDecl *VD) noexcept {
    return EnsureTemplateArgUpdate(VD);
  }
#endif

private:
  DeclUpdate &RegisterDecl(const NamedDecl *ND, bool IsField) noexcept {
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
    } else {
      const auto *FD = cast<FieldDecl>(ND);
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
    Update.IsMacroType = BaseLoc.getBeginLoc().isMacroID();

    if (Update.IsMacroType) {
      std::string Text =
          Lexer::getSourceText(CharSourceRange::getTokenRange(Update.TypeRange),
                               SM, LangOpts)
              .str();
      llvm::StringRef Trimmed = llvm::StringRef(Text).trim();
      if (IsIdentifier(Trimmed))
        Update.MacroName = Trimmed.str();
    }

    if (BaseLoc.getAs<AutoTypeLoc>() && !ExpandAuto) {
      Update.CanRewrite = false;
    }

    if (IsField) {
      const auto *FD = cast<FieldDecl>(ND);
      Update.CanRewrite = StructEngine.CanRewriteField(FD, SM);
    } else {
      Update.CanRewrite =
          Update.CanRewrite && !StructEngine.IsBoundaryFixed(ND, SM);
    }

    DeclUpdates[ND] = Update;
    return DeclUpdates[ND];
  }

  void UpdateDeclType(const NamedDecl *ND, const Expr *RHS) noexcept {
    if (!ND || !RHS)
      return;
    auto It = DeclUpdates.find(ND);
    if (It == DeclUpdates.end()) {
      RegisterDecl(ND, isa<FieldDecl>(ND));
      It = DeclUpdates.find(ND);
    }

    const Expr *BaseExpr = RHS->IgnoreParenImpCasts();
    QualType Candidate = NormalizeType(BaseExpr->getType());
    It->second.DesiredType =
        GetWiderType(It->second.DesiredType, Candidate, Ctx);
  }

  TemplateArgUpdate *EnsureTemplateArgUpdate(const VarDecl *VD) noexcept {
    auto It = TemplateUpdates.find(VD);
    if (It != TemplateUpdates.end())
      return &It->second;

    const TypeSourceInfo *TSI = VD->getTypeSourceInfo();
    if (!TSI)
      return nullptr;

    TypeLoc BaseLoc = GetBaseTypeLoc(TSI->getTypeLoc());
    auto TSTL = BaseLoc.getAs<TemplateSpecializationTypeLoc>();
    if (!TSTL)
      return nullptr;

    TemplateArgumentLoc ArgLoc = TSTL.getArgLoc(0);
    if (ArgLoc.getArgument().getKind() != TemplateArgument::Type)
      return nullptr;

    const TypeSourceInfo *ArgTSI = ArgLoc.getTypeSourceInfo();
    TypeLoc ArgTypeLoc = GetBaseTypeLoc(ArgTSI->getTypeLoc());
    SourceRange ArgRange = ArgTypeLoc.getSourceRange();

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
                  QualType OldType, QualType NewType) noexcept {
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
    llvm::DenseMap<const VarDecl *, TemplateArgUpdate>
        &TemplateUpdates) noexcept {
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
    llvm::DenseMap<const NamedDecl *, DeclUpdate> &DeclUpdates) noexcept {
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

void ApplyMacroUpdates(
    clang::Rewriter &Rewriter, bool AuditMode, ASTContext &Ctx,
    const std::map<std::string, QualType> &MacroUpdates) noexcept {
  SourceManager &SM = Rewriter.getSourceMgr();
  FileID MainFile = SM.getMainFileID();
  llvm::StringRef Buffer = SM.getBufferData(MainFile);
  SourceLocation FileStart = SM.getLocForStartOfFile(MainFile);

  auto IsSpace = [](char C) noexcept {
    return std::isspace(static_cast<unsigned char>(C)) != 0;
  };

  size_t Offset = 0;
  while (true) {
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
      if (Pos == Line.size() || IsSpace(Line[Pos])) {
        while (Pos < Line.size() && IsSpace(Line[Pos]))
          ++Pos;

        size_t NameStart = Pos;
        while (Pos < Line.size() &&
               (std::isalnum(static_cast<unsigned char>(Line[Pos])) ||
                Line[Pos] == '_')) {
          ++Pos;
        }

        if (NameStart != Pos && (Pos >= Line.size() || Line[Pos] != '(')) {
          std::string MacroName = Line.substr(NameStart, Pos - NameStart).str();
          auto UpdateIt = MacroUpdates.find(MacroName);
          if (UpdateIt != MacroUpdates.end()) {
            while (Pos < Line.size() && IsSpace(Line[Pos]))
              ++Pos;

            size_t ReplStart = Pos;
            if (ReplStart < Line.size()) {
              size_t ReplEnd = Line.size();
              size_t LineComment = Line.find("//", ReplStart);
              if (LineComment != llvm::StringRef::npos)
                ReplEnd = std::min(ReplEnd, LineComment);
              size_t BlockComment = Line.find("/*", ReplStart);
              if (BlockComment != llvm::StringRef::npos)
                ReplEnd = std::min(ReplEnd, BlockComment);
              while (ReplEnd > ReplStart && IsSpace(Line[ReplEnd - 1]))
                --ReplEnd;

              if (ReplEnd > ReplStart) {
                std::string NewText = TypeToString(UpdateIt->second, Ctx);
                llvm::StringRef Existing =
                    Line.substr(ReplStart, ReplEnd - ReplStart);
                if (Existing.trim() != NewText && !AuditMode) {
                  SourceLocation ReplaceStart =
                      FileStart.getLocWithOffset(Offset + ReplStart);
                  Rewriter.ReplaceText(ReplaceStart, ReplEnd - ReplStart,
                                       NewText);
                }
              }
            }
          }
        }
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
TypeCorrectMatcher::~TypeCorrectMatcher() noexcept {}

//------------------------------------------------------------------------------
// ✅ CONSTRUCTOR (REQUIRED)
//------------------------------------------------------------------------------
TypeCorrectMatcher::TypeCorrectMatcher(
    clang::Rewriter &Rewriter, bool UseDecltype, bool ExpandAuto,
    std::string ProjectRoot, std::string ExcludePattern, bool InPlace,
    bool EnableAbiBreakingChanges, bool AuditMode,
    type_correct::Phase CurrentPhase, std::string FactsOutputDir,
    std::string ReportFile) noexcept
    : Rewriter(Rewriter), UseDecltype(UseDecltype), ExpandAuto(ExpandAuto),
      ProjectRoot(std::move(ProjectRoot)),
      ExcludePattern(std::move(ExcludePattern)), InPlace(InPlace),
      AuditMode(AuditMode),
      StructEngine(EnableAbiBreakingChanges, false, this->ProjectRoot),
      CurrentPhase(CurrentPhase), FactsOutputDir(std::move(FactsOutputDir)),
      ReportFile(std::move(ReportFile)) {}

const std::vector<type_correct::ChangeRecord> &
TypeCorrectMatcher::GetChanges() const noexcept {
  return Changes;
}

//------------------------------------------------------------------------------
// ✅ Match callback (REQUIRED VIRTUAL)
//------------------------------------------------------------------------------
void TypeCorrectMatcher::run(const MatchFinder::MatchResult &Result) noexcept {
  if (const auto *Cast =
          Result.Nodes.getNodeAs<ExplicitCastExpr>("explicit_cast")) {
    ExplicitCasts.push_back(Cast);
  }
}

void TypeCorrectMatcher::ProcessRedundantCasts(ASTContext &Ctx) noexcept {
  if (ExplicitCasts.empty())
    return;

  SourceManager &SM = Rewriter.getSourceMgr();
  const LangOptions &LangOpts = Ctx.getLangOpts();
  llvm::SmallPtrSet<const ExplicitCastExpr *, 32> Seen;

  for (const auto *Cast : ExplicitCasts) {
    if (!Cast || !Seen.insert(Cast).second)
      continue;

    const Expr *SubExpr = Cast->getSubExpr();

    SourceLocation Begin = Cast->getBeginLoc();
    SourceLocation End = Cast->getEndLoc();
    if (Begin.isInvalid() || End.isInvalid())
      continue;
    if (Begin.isMacroID() || End.isMacroID())
      continue;
    if (SM.getFileID(Begin) != SM.getMainFileID())
      continue;

    const TypeSourceInfo *CastTypeInfo = Cast->getTypeInfoAsWritten();
    if (!CastTypeInfo)
      continue;
    QualType CastType = NormalizeType(CastTypeInfo->getType());
    QualType SubType = NormalizeType(SubExpr->getType());
    if (!Ctx.hasSameType(CastType, SubType))
      continue;

    SourceLocation SubBegin = SubExpr->getBeginLoc();
    llvm::StringRef Buffer = SM.getBufferData(SM.getFileID(SubBegin));

    std::string SubText;
    if (!Buffer.empty()) {
      SubText = Lexer::getSourceText(
                    CharSourceRange::getTokenRange(SubExpr->getSourceRange()),
                    SM, LangOpts)
                    .str();
    }
    if (SubText.empty())
      continue;

    if (!AuditMode) {
      Rewriter.ReplaceText(
          CharSourceRange::getTokenRange(Cast->getSourceRange()), SubText);
    }
  }

  ExplicitCasts.clear();
}

void TypeCorrectMatcher::ProcessNarrowingSafety(ASTContext &Ctx) noexcept {
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
void TypeCorrectMatcher::onEndOfTranslationUnit(ASTContext &Ctx) noexcept {
  ProcessRedundantCasts(Ctx);
  ProcessNarrowingSafety(Ctx);

  SourceManager &SM = Rewriter.getSourceMgr();

  if (!InPlace) {
    if (const auto *Buf = Rewriter.getRewriteBufferFor(SM.getMainFileID())) {
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
    clang::Rewriter &Rewriter, bool UseDecltype, bool ExpandAuto,
    const std::string &ProjectRoot, const std::string &ExcludePattern,
    bool InPlace, bool EnableAbiBreakingChanges, bool AuditMode,
    type_correct::Phase CurrentPhase, const std::string &FactsOutputDir,
    const std::string &ReportFile) noexcept
    : Handler(Rewriter, UseDecltype, ExpandAuto, ProjectRoot, ExcludePattern,
              InPlace, EnableAbiBreakingChanges, AuditMode, CurrentPhase,
              FactsOutputDir, ReportFile) {

  Finder.addMatcher(explicitCastExpr().bind("explicit_cast"), &Handler);
}

//------------------------------------------------------------------------------
// ✅ ASTConsumer override
//------------------------------------------------------------------------------
void TypeCorrectASTConsumer::HandleTranslationUnit(ASTContext &Ctx) noexcept {
  Finder.matchAST(Ctx);
  Handler.onEndOfTranslationUnit(Ctx);
}

#ifdef TYPE_CORRECT_TEST
namespace type_correct::test_support {

TYPE_CORRECT_EXPORT TypeLoc GetBaseTypeLocForTest(TypeLoc TL) noexcept {
  return GetBaseTypeLoc(TL);
}

TYPE_CORRECT_EXPORT QualType NormalizeTypeForTest(QualType T) noexcept {
  return NormalizeType(T);
}

TYPE_CORRECT_EXPORT QualType GetWiderTypeForTest(QualType A, QualType B,
                                                 ASTContext &Ctx) noexcept {
  return GetWiderType(A, B, Ctx);
}

TYPE_CORRECT_EXPORT std::string TypeToStringForTest(QualType T,
                                                    ASTContext &Ctx) noexcept {
  return TypeToString(T, Ctx);
}

TYPE_CORRECT_EXPORT std::optional<const NamedDecl *>
ResolveNamedDeclForTest(const Expr *E) noexcept {
  return ResolveNamedDecl(E);
}

TYPE_CORRECT_EXPORT bool IsIdentifierForTest(llvm::StringRef Text) noexcept {
  return IsIdentifier(Text);
}

TYPE_CORRECT_EXPORT void CoverVisitorEdgeCases(ASTContext &Ctx) noexcept {
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
      Ctx, Ctx.getTranslationUnitDecl(), SourceLocation(), SourceLocation(),
      &Id, Ctx.IntTy, Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), SC_None);
  Visitor.TestRegisterDecl(VD, false);
  Visitor.TestRegisterDecl(VD, false);

  VarDecl *ImplicitVD = VarDecl::Create(
      Ctx, Ctx.getTranslationUnitDecl(), SourceLocation(), SourceLocation(),
      &Id, Ctx.IntTy, Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), SC_None);
  ImplicitVD->setImplicit(true);
  Visitor.TestVisitVarDecl(ImplicitVD);

  RecordDecl *DummyRec = RecordDecl::Create(
      Ctx, type_correct::clang_compat::StructTagKind(),
      Ctx.getTranslationUnitDecl(), SourceLocation(), SourceLocation(), &Id);
  FieldDecl *ImplicitFD = FieldDecl::Create(
      Ctx, DummyRec, SourceLocation(), SourceLocation(), &Id, Ctx.IntTy,
      Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), nullptr, false, ICIS_NoInit);
  ImplicitFD->setImplicit(true);
  Visitor.TestVisitFieldDecl(ImplicitFD);

  SourceLocation MainLoc = Ctx.getSourceManager().getLocForStartOfFile(
      Ctx.getSourceManager().getMainFileID());
  QualType AutoTy = Ctx.getAutoDeductType();
  TypeSourceInfo *AutoTSI = Ctx.getTrivialTypeSourceInfo(AutoTy, MainLoc);
  VarDecl *AutoVD = VarDecl::Create(Ctx, Ctx.getTranslationUnitDecl(), MainLoc,
                                    MainLoc, &Id, AutoTy, AutoTSI, SC_None);
  TypeCorrectVisitor ExpandAutoVisitor(Engine, true, Ctx, DeclUpdates,
                                       TemplateUpdates);
  ExpandAutoVisitor.TestRegisterDecl(AutoVD, false);

  IdentifierInfo &NoTsiId = Ctx.Idents.get("tc_no_tsi");
  VarDecl *NoTsi =
      VarDecl::Create(Ctx, Ctx.getTranslationUnitDecl(), SourceLocation(),
                      SourceLocation(), &NoTsiId, Ctx.IntTy, nullptr, SC_None);
  NoTsi->setTypeSourceInfo(nullptr);
  Visitor.TestRegisterDecl(NoTsi, false);

  auto *Literal = IntegerLiteral::Create(Ctx, llvm::APInt(32, 1), Ctx.IntTy,
                                         SourceLocation());
  Visitor.TestUpdateDeclType(nullptr, Literal);
  Visitor.TestUpdateDeclType(VD, nullptr);

  IdentifierInfo &NewId = Ctx.Idents.get("tc_update_decl");
  VarDecl *NewDecl = VarDecl::Create(
      Ctx, Ctx.getTranslationUnitDecl(), SourceLocation(), SourceLocation(),
      &NewId, Ctx.IntTy, Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), SC_None);
  Visitor.TestUpdateDeclType(NewDecl, Literal);

  OpaqueValueExpr Opaque(SourceLocation(), Ctx.IntTy,
                         type_correct::clang_compat::PrValueKind());
  Visitor.TestUpdateDeclType(VD, &Opaque);
}

TYPE_CORRECT_EXPORT void
CoverEnsureTemplateArgUpdateEdges(ASTContext &Ctx, const VarDecl *TemplateDecl,
                                  const VarDecl *NonTemplateDecl,
                                  const VarDecl *NonTypeTemplateDecl,
                                  VarDecl *TrivialTemplateDecl) noexcept {
  StructAnalyzer Engine(true, true, "");
  llvm::DenseMap<const NamedDecl *, DeclUpdate> DeclUpdates;
  llvm::DenseMap<const VarDecl *, TemplateArgUpdate> TemplateUpdates;
  TypeCorrectVisitor Visitor(Engine, false, Ctx, DeclUpdates, TemplateUpdates);

  Visitor.TestEnsureTemplateArgUpdate(TemplateDecl);
  Visitor.TestEnsureTemplateArgUpdate(TemplateDecl);

  IdentifierInfo &NoTsiId = Ctx.Idents.get("tc_template_no_tsi");
  VarDecl *NoTsi =
      VarDecl::Create(Ctx, Ctx.getTranslationUnitDecl(), SourceLocation(),
                      SourceLocation(), &NoTsiId, Ctx.IntTy, nullptr, SC_None);
  NoTsi->setTypeSourceInfo(nullptr);
  Visitor.TestEnsureTemplateArgUpdate(NoTsi);

  Visitor.TestEnsureTemplateArgUpdate(NonTemplateDecl);
  Visitor.TestEnsureTemplateArgUpdate(NonTypeTemplateDecl);

  TrivialTemplateDecl->setTypeSourceInfo(
      Ctx.getTrivialTypeSourceInfo(TrivialTemplateDecl->getType()));
  Visitor.TestEnsureTemplateArgUpdate(TrivialTemplateDecl);
}

TYPE_CORRECT_EXPORT void
CoverTemplateUpdateMapEdges(ASTContext &Ctx, Rewriter &Rewriter) noexcept {
  llvm::DenseMap<const NamedDecl *, DeclUpdate> DeclUpdates;
  llvm::DenseMap<const VarDecl *, TemplateArgUpdate> TemplateUpdates;
  std::vector<type_correct::ChangeRecord> Changes;
  SourceManager &SM = Rewriter.getSourceMgr();
  SourceLocation FileLoc = SM.getLocForStartOfFile(SM.getMainFileID());
  SourceLocation MacroLoc = SM.createExpansionLoc(FileLoc, FileLoc, FileLoc, 1);

  auto MakeVar = [&](const char *Name) noexcept {
    IdentifierInfo &Id = Ctx.Idents.get(Name);
    return VarDecl::Create(Ctx, Ctx.getTranslationUnitDecl(), FileLoc, FileLoc,
                           &Id, Ctx.IntTy,
                           Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), SC_None);
  };

  const VarDecl *NullTypes = MakeVar("tc_tpl_null");
  TemplateUpdates[NullTypes] = TemplateArgUpdate{
      NullTypes, QualType(), Ctx.IntTy, SourceRange(FileLoc, FileLoc)};

  const VarDecl *NullDesired = MakeVar("tc_tpl_null_desired");
  TemplateUpdates[NullDesired] = TemplateArgUpdate{
      NullDesired, Ctx.IntTy, QualType(), SourceRange(FileLoc, FileLoc)};

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

  const VarDecl *InvalidEndRange = MakeVar("tc_tpl_invalid_end");
  TemplateUpdates[InvalidEndRange] =
      TemplateArgUpdate{InvalidEndRange, Ctx.IntTy, Ctx.LongTy,
                        SourceRange(FileLoc, SourceLocation())};

  const VarDecl *MacroRange = MakeVar("tc_tpl_macro");
  TemplateUpdates[MacroRange] = TemplateArgUpdate{
      MacroRange, Ctx.IntTy, Ctx.LongTy, SourceRange(MacroLoc, MacroLoc)};

  const VarDecl *MacroEndRange = MakeVar("tc_tpl_macro_end");
  TemplateUpdates[MacroEndRange] = TemplateArgUpdate{
      MacroEndRange, Ctx.IntTy, Ctx.LongTy, SourceRange(FileLoc, MacroLoc)};

  const VarDecl *ValidAudit = MakeVar("tc_tpl_valid_audit");
  TemplateUpdates[ValidAudit] = TemplateArgUpdate{
      ValidAudit, Ctx.IntTy, Ctx.LongTy, SourceRange(FileLoc, FileLoc)};

  ApplyTemplateUpdates(Rewriter, true, Changes, Ctx, DeclUpdates,
                       TemplateUpdates);
}

TYPE_CORRECT_EXPORT void CoverDeclUpdateMapEdges(ASTContext &Ctx,
                                                 Rewriter &Rewriter) noexcept {
  llvm::DenseMap<const NamedDecl *, DeclUpdate> DeclUpdates;
  std::vector<type_correct::ChangeRecord> Changes;
  SourceManager &SM = Rewriter.getSourceMgr();
  SourceLocation FileLoc = SM.getLocForStartOfFile(SM.getMainFileID());
  SourceLocation MacroLoc = SM.createExpansionLoc(FileLoc, FileLoc, FileLoc, 1);

  auto MakeVar = [&](const char *Name) noexcept {
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

  const VarDecl *NullDesired = MakeVar("tc_decl_null_desired");
  DeclUpdate NullDesiredUpdate;
  NullDesiredUpdate.Decl = NullDesired;
  NullDesiredUpdate.OriginalType = Ctx.IntTy;
  NullDesiredUpdate.DesiredType = QualType();
  NullDesiredUpdate.TypeRange = SourceRange(FileLoc, FileLoc);
  DeclUpdates[NullDesired] = NullDesiredUpdate;

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

  const VarDecl *MacroC = MakeVar("tc_macro_c");
  DeclUpdate MacroUpdateC = MacroUpdateA;
  MacroUpdateC.Decl = MacroC;
  MacroUpdateC.DesiredType = Ctx.UnsignedLongLongTy;
  DeclUpdates[MacroC] = MacroUpdateC;

  const VarDecl *InvalidRange = MakeVar("tc_decl_invalid");
  DeclUpdate InvalidUpdate;
  InvalidUpdate.Decl = InvalidRange;
  InvalidUpdate.OriginalType = Ctx.IntTy;
  InvalidUpdate.DesiredType = Ctx.LongTy;
  InvalidUpdate.TypeRange = SourceRange();
  DeclUpdates[InvalidRange] = InvalidUpdate;

  const VarDecl *InvalidEnd = MakeVar("tc_decl_invalid_end");
  DeclUpdate InvalidEndUpdate;
  InvalidEndUpdate.Decl = InvalidEnd;
  InvalidEndUpdate.OriginalType = Ctx.IntTy;
  InvalidEndUpdate.DesiredType = Ctx.LongTy;
  InvalidEndUpdate.TypeRange = SourceRange(FileLoc, SourceLocation());
  DeclUpdates[InvalidEnd] = InvalidEndUpdate;

  const VarDecl *MacroEnd = MakeVar("tc_decl_macro_end");
  DeclUpdate MacroEndUpdate;
  MacroEndUpdate.Decl = MacroEnd;
  MacroEndUpdate.OriginalType = Ctx.IntTy;
  MacroEndUpdate.DesiredType = Ctx.LongTy;
  MacroEndUpdate.TypeRange = SourceRange(FileLoc, MacroLoc);
  DeclUpdates[MacroEnd] = MacroEndUpdate;

  (void)CollectMacroUpdates(Rewriter, true, Changes, Ctx, DeclUpdates);
}

TYPE_CORRECT_EXPORT bool CoverMacroScannerEdges(ASTContext &Ctx,
                                                Rewriter &Rewriter) noexcept {
  SourceManager &SM = Rewriter.getSourceMgr();
  FileID MainFile = SM.getMainFileID();
  auto EntryRef = SM.getFileEntryRefForID(MainFile);

  std::string NewText = TypeToString(Ctx.getSizeType(), Ctx);
  std::string Buffer;
  Buffer += "  #defineFOO 1\n";
  Buffer += "#define\n";
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

TYPE_CORRECT_EXPORT void CoverRecordChangeEdges(ASTContext &Ctx,
                                                Rewriter &Rewriter) noexcept {
  std::vector<type_correct::ChangeRecord> Changes;
  SourceManager &SM = Rewriter.getSourceMgr();

  RecordChange(Changes, SM, Ctx, nullptr, Ctx.IntTy, Ctx.LongTy);

  IdentifierInfo &BadId = Ctx.Idents.get("tc_badloc");
  VarDecl *BadDecl = VarDecl::Create(
      Ctx, Ctx.getTranslationUnitDecl(), SourceLocation(), SourceLocation(),
      &BadId, Ctx.IntTy, Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), SC_None);
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
