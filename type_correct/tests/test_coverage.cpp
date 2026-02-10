#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/LangOptions.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>

#include <cstdlib>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <clang/AST/ASTTypeTraits.h>
#include <clang/ASTMatchers/ASTMatchersInternal.h>

#define private public
#include <type_correct/StructAnalyzer.h>
#include <type_correct/TypeSolver.h>
#include <type_correct/TypeCorrect.h>
#undef private

#include <type_correct/ClangCompat.h>
#include <type_correct/CTU/FactManager.h>
#include <type_correct/TypeCorrectMain.h>

using namespace clang;
using namespace clang::tooling;
using ::testing::HasSubstr;

namespace type_correct::test_support {
TYPE_CORRECT_EXPORT clang::TypeLoc GetBaseTypeLocForTest(clang::TypeLoc TL);
TYPE_CORRECT_EXPORT clang::QualType NormalizeTypeForTest(clang::QualType T);
TYPE_CORRECT_EXPORT clang::QualType
GetWiderTypeForTest(clang::QualType A, clang::QualType B,
                    clang::ASTContext &Ctx);
TYPE_CORRECT_EXPORT std::string TypeToStringForTest(clang::QualType T,
                                                    clang::ASTContext &Ctx);
TYPE_CORRECT_EXPORT const clang::NamedDecl *
ResolveNamedDeclForTest(const clang::Expr *E);
TYPE_CORRECT_EXPORT bool IsIdentifierForTest(llvm::StringRef Text);
TYPE_CORRECT_EXPORT void CoverVisitorEdgeCases(clang::ASTContext &Ctx);
TYPE_CORRECT_EXPORT void CoverEnsureTemplateArgUpdateEdges(
    clang::ASTContext &Ctx, const clang::VarDecl *TemplateDecl,
    const clang::VarDecl *NonTemplateDecl,
    const clang::VarDecl *NonTypeTemplateDecl,
    clang::VarDecl *TrivialTemplateDecl);
TYPE_CORRECT_EXPORT void CoverTemplateUpdateMapEdges(
    clang::ASTContext &Ctx, clang::Rewriter &Rewriter);
TYPE_CORRECT_EXPORT void CoverDeclUpdateMapEdges(clang::ASTContext &Ctx,
                                                 clang::Rewriter &Rewriter);
TYPE_CORRECT_EXPORT bool CoverMacroScannerEdges(clang::ASTContext &Ctx,
                                                clang::Rewriter &Rewriter);
TYPE_CORRECT_EXPORT void CoverRecordChangeEdges(clang::ASTContext &Ctx,
                                                clang::Rewriter &Rewriter);
} // namespace type_correct::test_support

namespace {

class LambdaASTConsumer : public clang::ASTConsumer {
public:
  explicit LambdaASTConsumer(std::function<void(clang::ASTContext &)> Fn)
      : Fn(std::move(Fn)) {}

  void HandleTranslationUnit(clang::ASTContext &Ctx) override { Fn(Ctx); }

private:
  std::function<void(clang::ASTContext &)> Fn;
};

class LambdaFrontendAction : public clang::ASTFrontendAction {
public:
  explicit LambdaFrontendAction(std::function<void(clang::ASTContext &)> Fn)
      : Fn(std::move(Fn)) {}

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef) override {
    return std::make_unique<LambdaASTConsumer>(Fn);
  }

private:
  std::function<void(clang::ASTContext &)> Fn;
};

bool RunWithAST(const std::string &Code,
                std::function<void(clang::ASTContext &)> Fn) {
  std::vector<std::string> Args = {"-std=c++17"};
  return clang::tooling::runToolOnCodeWithArgs(
      std::make_unique<LambdaFrontendAction>(std::move(Fn)), Code, Args,
      "input.cc");
}

struct FindVisitor : public clang::RecursiveASTVisitor<FindVisitor> {
  std::string VarName;
  std::string FieldName;
  std::string TypedefName;
  std::string RecordName;

  const VarDecl *FoundVar = nullptr;
  const FieldDecl *FoundField = nullptr;
  const TypedefNameDecl *FoundTypedef = nullptr;
  const RecordDecl *FoundRecord = nullptr;
  const ForStmt *FoundFor = nullptr;
  const ExplicitCastExpr *FoundCast = nullptr;
  const CallExpr *FoundCall = nullptr;
  const MemberExpr *FoundMember = nullptr;
  const DeclRefExpr *FoundDeclRef = nullptr;

  bool VisitVarDecl(VarDecl *VD) {
    if (!FoundVar && VD && VD->getName() == VarName)
      FoundVar = VD;
    return true;
  }

  bool VisitFieldDecl(FieldDecl *FD) {
    if (!FoundField && FD && FD->getName() == FieldName)
      FoundField = FD;
    return true;
  }

  bool VisitTypedefNameDecl(TypedefNameDecl *TD) {
    if (!FoundTypedef && TD && TD->getName() == TypedefName)
      FoundTypedef = TD;
    return true;
  }

  bool VisitRecordDecl(RecordDecl *RD) {
    if (!FoundRecord && RD && RD->getName() == RecordName)
      FoundRecord = RD;
    return true;
  }

  bool VisitForStmt(ForStmt *FS) {
    if (!FoundFor)
      FoundFor = FS;
    return true;
  }

  bool VisitExplicitCastExpr(ExplicitCastExpr *CE) {
    if (!FoundCast)
      FoundCast = CE;
    return true;
  }

  bool VisitCallExpr(CallExpr *CE) {
    if (!FoundCall)
      FoundCall = CE;
    return true;
  }

  bool VisitMemberExpr(MemberExpr *ME) {
    if (!FoundMember)
      FoundMember = ME;
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *DRE) {
    if (!FoundDeclRef)
      FoundDeclRef = DRE;
    return true;
  }
};

const VarDecl *FindVarDecl(ASTContext &Ctx, const std::string &Name) {
  FindVisitor Visitor;
  Visitor.VarName = Name;
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
  return Visitor.FoundVar;
}

const FieldDecl *FindFieldDecl(ASTContext &Ctx, const std::string &Name) {
  FindVisitor Visitor;
  Visitor.FieldName = Name;
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
  return Visitor.FoundField;
}

const TypedefNameDecl *FindTypedefDecl(ASTContext &Ctx,
                                       const std::string &Name) {
  FindVisitor Visitor;
  Visitor.TypedefName = Name;
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
  return Visitor.FoundTypedef;
}

const RecordDecl *FindRecordDecl(ASTContext &Ctx, const std::string &Name) {
  FindVisitor Visitor;
  Visitor.RecordName = Name;
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
  return Visitor.FoundRecord;
}

const ForStmt *FindFirstFor(ASTContext &Ctx) {
  FindVisitor Visitor;
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
  return Visitor.FoundFor;
}

const ExplicitCastExpr *FindFirstExplicitCast(ASTContext &Ctx) {
  FindVisitor Visitor;
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
  return Visitor.FoundCast;
}

const CallExpr *FindFirstCall(ASTContext &Ctx) {
  FindVisitor Visitor;
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
  return Visitor.FoundCall;
}

const MemberExpr *FindFirstMemberExpr(ASTContext &Ctx) {
  FindVisitor Visitor;
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
  return Visitor.FoundMember;
}

const DeclRefExpr *FindFirstDeclRef(ASTContext &Ctx) {
  FindVisitor Visitor;
  Visitor.TraverseDecl(Ctx.getTranslationUnitDecl());
  return Visitor.FoundDeclRef;
}

int RunCommand(const std::string &Command) {
  int Result = std::system(Command.c_str());
#ifdef _WIN32
  return Result;
#else
  if (Result == -1)
    return Result;
  if (WIFEXITED(Result))
    return WEXITSTATUS(Result);
  return Result;
#endif
}

std::string MakeTempDir(const std::string &Prefix) {
  llvm::SmallString<128> TempDir;
  std::error_code EC =
      llvm::sys::fs::createUniqueDirectory(Prefix, TempDir);
  EXPECT_FALSE(EC);
  return TempDir.str().str();
}

} // namespace

GTEST_TEST(Coverage, TypeSolverPaths) {
  static const char *const Code =
      "typedef unsigned long size_t;"
      "int get_int();"
      "long get_long();"
      "size_t get_size();"
      "struct Incomplete;"
      "struct Complete { int x; };"
      "void test() {"
      "  int a = get_int();"
      "  long b = get_long();"
      "  unsigned int c = 0u;"
      "  int n = 3;"
      "  for (int i = 0; i < n; ++i) { (void)i; }"
      "  int casted = (int)get_int();"
      "  int literal = 42;"
      "  (void)casted; (void)literal;"
      "}";

  ASSERT_TRUE(RunWithAST(Code, [](ASTContext &Ctx) {
    type_correct::TypeSolver Solver;

    const VarDecl *A = FindVarDecl(Ctx, "a");
    const VarDecl *B = FindVarDecl(Ctx, "b");
    const VarDecl *C = FindVarDecl(Ctx, "c");
    const VarDecl *N = FindVarDecl(Ctx, "n");
    const VarDecl *I = FindVarDecl(Ctx, "i");
    const VarDecl *Literal = FindVarDecl(Ctx, "literal");
    const RecordDecl *Incomplete = FindRecordDecl(Ctx, "Incomplete");
    const RecordDecl *Complete = FindRecordDecl(Ctx, "Complete");
    ASSERT_NE(A, nullptr);
    ASSERT_NE(B, nullptr);
    ASSERT_NE(C, nullptr);
    ASSERT_NE(N, nullptr);
    ASSERT_NE(I, nullptr);
    ASSERT_NE(Literal, nullptr);
    ASSERT_NE(Incomplete, nullptr);
    ASSERT_NE(Complete, nullptr);

    Solver.AddNode(A, A->getType(), false);
    Solver.AddNode(B, B->getType(), false);
    Solver.AddNode(C, C->getType(), false);
    Solver.AddNode(A, A->getType(), true);
    Solver.AddNode(A, A->getType(), false, true);

    Solver.AddGlobalConstraint(N, Ctx.getSizeType(), &Ctx);
    Solver.AddGlobalConstraint(A, Ctx.getSizeType(), &Ctx);

    Solver.AddPointerOffsetUsage(nullptr);
    Solver.AddPointerOffsetUsage(A);

    Solver.AddEdge(nullptr, nullptr);
    Solver.AddEdge(A, A);
    Solver.AddEdge(N, B);
    Solver.AddEdge(A, B);

    Solver.AddLoopComparisonConstraint(nullptr, nullptr, &Ctx);

    Solver.AddConstraint(nullptr, Ctx.IntTy, nullptr, &Ctx);
    Solver.AddConstraint(A, Ctx.getSizeType(), nullptr, &Ctx);

    const ForStmt *Loop = FindFirstFor(Ctx);
    ASSERT_NE(Loop, nullptr);
    const auto *Cond = dyn_cast_or_null<BinaryOperator>(Loop->getCond());
    ASSERT_NE(Cond, nullptr);
    const Expr *BoundExpr = Cond->getRHS();
    Solver.AddLoopComparisonConstraint(I, BoundExpr, &Ctx);

    const CallExpr *Call = FindFirstCall(Ctx);
    ASSERT_NE(Call, nullptr);
    Solver.AddLoopComparisonConstraint(I, Call, &Ctx);

    const ExplicitCastExpr *Cast = FindFirstExplicitCast(Ctx);
    ASSERT_NE(Cast, nullptr);
    Solver.AddLoopComparisonConstraint(I, Cast, &Ctx);

    const Expr *LiteralExpr = Literal->getInit();
    ASSERT_NE(LiteralExpr, nullptr);
    Solver.AddLoopComparisonConstraint(I, LiteralExpr, &Ctx);
    Solver.AddConstraint(A, Ctx.getSizeType(), LiteralExpr, &Ctx);

    type_correct::ValueRange Positive(0, 10);
    type_correct::ValueRange Negative(-5, 5);
    type_correct::ValueRange Single(7);
    EXPECT_TRUE(Single.HasMin);
    EXPECT_TRUE(Single.HasMax);
    EXPECT_EQ(Single.Min, 7);
    EXPECT_EQ(Single.Max, 7);
    Solver.AddRangeConstraint(A, Positive);
    Solver.AddRangeConstraint(B, Negative);

    Solver.AddSymbolicConstraint(nullptr, type_correct::OpKind::Add, A, B);
    Solver.AddSymbolicConstraint(C, type_correct::OpKind::Add, A, B);

    auto Updates = Solver.Solve(&Ctx);
    EXPECT_FALSE(Updates.empty());

    EXPECT_TRUE(Solver.GetResolvedType(nullptr).isNull());
    EXPECT_FALSE(Solver.GetResolvedType(A).isNull());

    QualType IncompleteTy = Ctx.getRecordType(Incomplete);
    QualType CompleteTy = Ctx.getRecordType(Complete);
    EXPECT_FALSE(Solver.GetWider(QualType(), Ctx.IntTy, &Ctx).isNull());
    Solver.GetWider(Ctx.IntTy, QualType(), &Ctx);
    Solver.GetWider(Ctx.IntTy, Ctx.IntTy, &Ctx);
    Solver.GetWider(IncompleteTy, CompleteTy, &Ctx);
    Solver.GetWider(CompleteTy, Ctx.getPointerDiffType(), &Ctx);
    Solver.GetWider(Ctx.IntTy, Ctx.LongLongTy, &Ctx);
    Solver.GetWider(Ctx.LongLongTy, Ctx.IntTy, &Ctx);
    Solver.GetWider(Ctx.IntTy, Ctx.UnsignedIntTy, &Ctx);
  }));
}

GTEST_TEST(Coverage, TypeSolverExtraPaths) {
  static const char *const Code =
      "int main() {"
      "  int a = 0;"
      "  int b = 0;"
      "  int c = 0;"
      "  int d = 0;"
      "  return a + b + c + d;"
      "}";

  ASSERT_TRUE(RunWithAST(Code, [](ASTContext &Ctx) {
    type_correct::TypeSolver Solver;

    const VarDecl *A = FindVarDecl(Ctx, "a");
    const VarDecl *B = FindVarDecl(Ctx, "b");
    const VarDecl *C = FindVarDecl(Ctx, "c");
    const VarDecl *D = FindVarDecl(Ctx, "d");
    ASSERT_NE(A, nullptr);
    ASSERT_NE(B, nullptr);
    ASSERT_NE(C, nullptr);
    ASSERT_NE(D, nullptr);

    type_correct::ValueRange Range(0, 10);
    Range.Union(type_correct::ValueRange(5, 15));

    type_correct::ValueRange EmptyUnionTarget;
    type_correct::ValueRange EmptyUnionSource;
    EmptyUnionTarget.Union(EmptyUnionSource);

    type_correct::ValueRange OnlyMin;
    OnlyMin.Min = 1;
    OnlyMin.HasMin = true;

    type_correct::ValueRange OnlyMax;
    OnlyMax.Max = 5;
    OnlyMax.HasMax = true;

    type_correct::ValueRange Partial;
    Partial.Union(OnlyMin);
    Partial.Union(OnlyMin);
    Partial.Union(OnlyMax);
    Partial.Union(OnlyMax);

    Solver.AddNode(nullptr, QualType(), false);
    Solver.AddGlobalConstraint(nullptr, Ctx.IntTy, &Ctx);
    Solver.AddRangeConstraint(nullptr, type_correct::ValueRange(1));
    Solver.AddRangeConstraint(D, type_correct::ValueRange(1));

    Solver.AddNode(A, A->getType(), false);
    Solver.AddNode(B, B->getType(), false);
    Solver.AddNode(C, C->getType(), false);

    Solver.AddPointerOffsetUsage(B);

    Solver.AddEdge(A, B);
    Solver.AddEdge(B, A);

    Solver.TarjanStack.push(A);

    auto *Opaque = new (Ctx)
        OpaqueValueExpr(SourceLocation(), Ctx.IntTy,
                        type_correct::clang_compat::PrValueKind());
    Solver.AddLoopComparisonConstraint(A, Opaque, &Ctx);
    EXPECT_TRUE(Solver.HelperGetType(nullptr, &Ctx).isNull());

    EXPECT_FALSE(Solver.GetResolvedType(D).isNull());

    type_correct::ValueRange EmptyRange;
    Solver.GetOptimalTypeForRange(EmptyRange, Ctx.IntTy, &Ctx);
    Solver.GetOptimalTypeForRange(OnlyMin, Ctx.IntTy, &Ctx);
    Solver.GetOptimalTypeForRange(type_correct::ValueRange(0, 200), Ctx.IntTy,
                                  &Ctx);
    Solver.GetOptimalTypeForRange(type_correct::ValueRange(0, 70000), Ctx.IntTy,
                                  &Ctx);
    Solver.GetOptimalTypeForRange(
        type_correct::ValueRange(0,
                                 static_cast<int64_t>(
                                     std::numeric_limits<uint32_t>::max())),
        Ctx.IntTy, &Ctx);
    Solver.GetOptimalTypeForRange(
        type_correct::ValueRange(
            0, static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) + 1),
        Ctx.IntTy, &Ctx);
    Solver.GetOptimalTypeForRange(type_correct::ValueRange(-200, 200),
                                  Ctx.IntTy, &Ctx);
    Solver.GetOptimalTypeForRange(type_correct::ValueRange(-40000, 40000),
                                  Ctx.IntTy, &Ctx);
    Solver.GetOptimalTypeForRange(
        type_correct::ValueRange(-3000000000LL, 3000000000LL), Ctx.IntTy,
        &Ctx);

    auto Updates = Solver.Solve(&Ctx);
    EXPECT_FALSE(Updates.empty());
  }));
}

GTEST_TEST(Coverage, StructAnalyzerPaths) {
  static const char *const Code =
      "#define MACRO_FIELD int macro_field;\n"
      "struct Macro { MACRO_FIELD };\n"
      "struct Packed { __attribute__((packed)) int packed_field; };\n"
      "struct __attribute__((packed)) PackedRecord { int packed_record_field; };\n"
      "struct Bits { int bit_field : 1; };\n"
      "union Uni { int union_field; };\n"
      "struct Normal { int normal_field; };\n"
      "typedef int Alias;\n";

  ASSERT_TRUE(RunWithAST(Code, [](ASTContext &Ctx) {
    SourceManager &SM = Ctx.getSourceManager();

    const FieldDecl *MacroField = FindFieldDecl(Ctx, "macro_field");
    const FieldDecl *PackedField = FindFieldDecl(Ctx, "packed_field");
    const FieldDecl *PackedRecordField =
        FindFieldDecl(Ctx, "packed_record_field");
    const FieldDecl *BitField = FindFieldDecl(Ctx, "bit_field");
    const FieldDecl *UnionField = FindFieldDecl(Ctx, "union_field");
    const FieldDecl *NormalField = FindFieldDecl(Ctx, "normal_field");
    const TypedefNameDecl *Alias = FindTypedefDecl(Ctx, "Alias");

    ASSERT_NE(MacroField, nullptr);
    ASSERT_NE(PackedField, nullptr);
    ASSERT_NE(PackedRecordField, nullptr);
    ASSERT_NE(BitField, nullptr);
    ASSERT_NE(UnionField, nullptr);
    ASSERT_NE(NormalField, nullptr);
    ASSERT_NE(Alias, nullptr);

    StructAnalyzer ForceRewrite(true, true, "");
    EXPECT_TRUE(ForceRewrite.CanRewriteField(NormalField, SM));
    EXPECT_FALSE(ForceRewrite.IsBoundaryFixed(NormalField, SM));

    StructAnalyzer NoAbi(false, false, "");
    EXPECT_FALSE(NoAbi.CanRewriteField(NormalField, SM));

    StructAnalyzer AllowAbi(true, false, "");
    EXPECT_FALSE(AllowAbi.CanRewriteField(BitField, SM));
    EXPECT_FALSE(AllowAbi.CanRewriteField(PackedField, SM));
    EXPECT_TRUE(AllowAbi.IsPacked(PackedRecordField));
    EXPECT_FALSE(AllowAbi.CanRewriteField(UnionField, SM));

    StructAnalyzer OutsideRoot(true, false, "/tmp/does_not_exist");
    EXPECT_TRUE(OutsideRoot.CanRewriteField(NormalField, SM));

    AllowAbi.TruncationUnsafeFields.insert(NormalField);
    EXPECT_FALSE(AllowAbi.CanRewriteField(NormalField, SM));

    EXPECT_FALSE(AllowAbi.CanRewriteField(nullptr, SM));
    EXPECT_FALSE(AllowAbi.IsPacked(nullptr));
    EXPECT_FALSE(AllowAbi.CanRewriteTypedef(nullptr, SM));
    EXPECT_TRUE(AllowAbi.CanRewriteTypedef(Alias, SM));

    EXPECT_TRUE(AllowAbi.IsBoundaryFixed(nullptr, SM));

    auto &Idents = Ctx.Idents;
    IdentifierInfo &GhostId = Idents.get("ghost");
    VarDecl *Ghost = VarDecl::Create(Ctx, Ctx.getTranslationUnitDecl(),
                                     SourceLocation(), SourceLocation(),
                                     &GhostId, Ctx.IntTy, nullptr, SC_None);
    EXPECT_TRUE(AllowAbi.IsBoundaryFixed(Ghost, SM));

    SourceLocation FakeLoc = SourceLocation::getFromRawEncoding(1);
    IdentifierInfo &FakeId = Idents.get("fake");
    VarDecl *Fake = VarDecl::Create(Ctx, Ctx.getTranslationUnitDecl(), FakeLoc,
                                    FakeLoc, &FakeId, Ctx.IntTy, nullptr,
                                    SC_None);
    AllowAbi.IsBoundaryFixed(Fake, SM);

    AllowAbi.IsBoundaryFixed(MacroField, SM);

    IdentifierInfo &BadFieldId = Idents.get("bad_field");
    RecordDecl *BadRecord =
        RecordDecl::Create(Ctx, type_correct::clang_compat::StructTagKind(),
                           Ctx.getTranslationUnitDecl(), SourceLocation(),
                           SourceLocation(), &BadFieldId);
    FieldDecl *BadField =
        FieldDecl::Create(Ctx, BadRecord, SourceLocation(), SourceLocation(),
                          &BadFieldId, Ctx.IntTy,
                          Ctx.getTrivialTypeSourceInfo(Ctx.IntTy), nullptr,
                          false, ICIS_NoInit);
    EXPECT_FALSE(AllowAbi.CanRewriteField(BadField, SM));

    // Exercise CheckFileBoundary paths directly.
    BoundaryStatus MainStatus =
        AllowAbi.CheckFileBoundary(SM.getMainFileID(), SM);
    EXPECT_EQ(MainStatus, BoundaryStatus::Modifiable);
    EXPECT_EQ(AllowAbi.CheckFileBoundary(SM.getMainFileID(), SM),
              BoundaryStatus::Modifiable);

    auto SysBuffer = llvm::MemoryBuffer::getMemBuffer("int sys;", "sys.h");
    FileID SysFile = SM.createFileID(std::move(SysBuffer),
                                     SrcMgr::C_System);
    EXPECT_EQ(AllowAbi.CheckFileBoundary(SysFile, SM), BoundaryStatus::Fixed);

    auto MemBuffer = llvm::MemoryBuffer::getMemBuffer("int mem;", "mem.h");
    FileID MemFile = SM.createFileID(std::move(MemBuffer), SrcMgr::C_User);
    EXPECT_EQ(AllowAbi.CheckFileBoundary(MemFile, SM), BoundaryStatus::Fixed);

    std::string TempDir = MakeTempDir("type_correct_ext");
    std::string ThirdPartyDir = TempDir + "/third_party";
    llvm::sys::fs::create_directories(ThirdPartyDir);
    std::string ThirdPartyFile = ThirdPartyDir + "/file.h";
    {
      std::ofstream Out(ThirdPartyFile);
      Out << "int t;\n";
    }
    auto ThirdPartyRef =
        SM.getFileManager().getFileRef(ThirdPartyFile);
    bool HasThirdParty = static_cast<bool>(ThirdPartyRef);
    ASSERT_TRUE(HasThirdParty);
    FileID ThirdPartyID =
        SM.createFileID(*ThirdPartyRef, SourceLocation(), SrcMgr::C_User);
    EXPECT_EQ(AllowAbi.CheckFileBoundary(ThirdPartyID, SM),
              BoundaryStatus::Fixed);

    std::string ProjectDir = TempDir + "/project";
    llvm::sys::fs::create_directories(ProjectDir);
    {
      std::ofstream Out(ProjectDir + "/CMakeLists.txt");
      Out << "project(Coverage)\n";
    }
    std::string ProjectFile = ProjectDir + "/file.h";
    {
      std::ofstream Out(ProjectFile);
      Out << "int p;\n";
    }
    StructAnalyzer ProjectAnalyzer(true, false, ProjectDir);
    auto ProjectRef =
        SM.getFileManager().getFileRef(ProjectFile);
    bool HasProject = static_cast<bool>(ProjectRef);
    ASSERT_TRUE(HasProject);
    FileID ProjectID =
        SM.createFileID(*ProjectRef, SourceLocation(), SrcMgr::C_User);
    EXPECT_EQ(ProjectAnalyzer.CheckFileBoundary(ProjectID, SM),
              BoundaryStatus::Modifiable);
    EXPECT_TRUE(ProjectAnalyzer.IsExternalPath(ThirdPartyFile));

    SourceLocation IncludeLoc = SM.getLocForStartOfFile(SysFile);
    FileID IncludedID =
        SM.createFileID(*ProjectRef, IncludeLoc, SrcMgr::C_User);
    EXPECT_EQ(AllowAbi.CheckFileBoundary(IncludedID, SM),
              BoundaryStatus::Fixed);

    EXPECT_TRUE(AllowAbi.IsExternalPath("/usr/include/stdio.h"));
    EXPECT_TRUE(AllowAbi.IsExternalPath("/opt/vendor/lib.h"));
    EXPECT_TRUE(AllowAbi.IsExternalPath(ThirdPartyFile));

    StructAnalyzer ForceExternal(true, true, "");
    EXPECT_FALSE(ForceExternal.IsExternalPath(ThirdPartyFile));

    EXPECT_FALSE(AllowAbi.AnalyzeCMakeDependency(""));
    EXPECT_FALSE(AllowAbi.AnalyzeCMakeDependency("."));

    std::string CMakeDir = TempDir + "/cmake_dep";
    llvm::sys::fs::create_directories(CMakeDir);
    {
      std::ofstream Out(CMakeDir + "/CMakeLists.txt");
      Out << "FetchContent_Declare(foo)\n";
    }
    EXPECT_TRUE(AllowAbi.AnalyzeCMakeDependency(CMakeDir));
    EXPECT_TRUE(AllowAbi.AnalyzeCMakeDependency(CMakeDir));
    EXPECT_TRUE(AllowAbi.IsExternalPath(CMakeDir + "/file.h"));

    std::string CleanDir = TempDir + "/cmake_clean";
    llvm::sys::fs::create_directories(CleanDir);
    EXPECT_FALSE(AllowAbi.AnalyzeCMakeDependency(CleanDir));

    std::string UnreadableDir = TempDir + "/cmake_unreadable";
    llvm::sys::fs::create_directories(UnreadableDir);
    std::string UnreadableFile = UnreadableDir + "/CMakeLists.txt";
    {
      std::ofstream Out(UnreadableFile);
      Out << "FetchContent_Declare(bar)\n";
    }
    llvm::sys::fs::setPermissions(UnreadableFile, llvm::sys::fs::no_perms);
    EXPECT_FALSE(AllowAbi.AnalyzeCMakeDependency(UnreadableDir));
    llvm::sys::fs::setPermissions(UnreadableFile,
                                  llvm::sys::fs::perms::all_all);

    std::string OutsideDir = TempDir + "/outside_dir/child";
    EXPECT_TRUE(ProjectAnalyzer.IsExternalPath(TempDir + "/outside_dir/file.h"));
    EXPECT_TRUE(ProjectAnalyzer.AnalyzeCMakeDependency(OutsideDir));

    AllowAbi.AnalyzeTruncationSafety(NormalField, nullptr, &Ctx);
    EXPECT_TRUE(AllowAbi.GetLikelyUnsafeFields().count(NormalField));
  }));
}

GTEST_TEST(Coverage, FactManagerPaths) {
  using type_correct::ctu::FactManager;
  using type_correct::ctu::SymbolFact;

  std::string TempDir = MakeTempDir("type_correct_facts");
  std::string FactsFile = TempDir + "/sample.facts";

  std::map<std::string, SymbolFact> Facts;
  Facts["A"] = SymbolFact("A", "int", false);
  Facts["B"] = SymbolFact("B", "size_t", true, true);

  EXPECT_FALSE(FactManager::WriteFacts(TempDir + "/missing/dir.facts", Facts));
  EXPECT_TRUE(FactManager::WriteFacts(FactsFile, Facts));

  std::ofstream Extra(FactsFile, std::ios::app);
  Extra << "# comment\n";
  Extra << "C\tunsigned long\t1\t0\n";
  Extra << "LEGACY\tint\t1\n";
  Extra << "BROKEN\tint\n";
  Extra << "\n";
  Extra.close();

  std::vector<SymbolFact> ReadFacts;
  EXPECT_TRUE(FactManager::ReadFacts(FactsFile, ReadFacts));
  EXPECT_FALSE(ReadFacts.empty());
  bool FoundLegacy = false;
  for (const auto &Fact : ReadFacts) {
    if (Fact.USR == "LEGACY") {
      FoundLegacy = true;
      EXPECT_FALSE(Fact.IsTypedef);
      break;
    }
  }
  EXPECT_TRUE(FoundLegacy);
  std::vector<SymbolFact> Missing;
  EXPECT_FALSE(FactManager::ReadFacts(TempDir + "/missing.facts", Missing));

  std::vector<SymbolFact> RawFacts = {
      SymbolFact("X", "unsigned char", false),
      SymbolFact("X", "short", false),
      SymbolFact("Y", "int", false),
      SymbolFact("Z", "long", false),
      SymbolFact("W", "size_t", false),
      SymbolFact("V", "long long", false),
      SymbolFact("U", "ptrdiff_t", false),
      SymbolFact("U", "unknown", false),
      SymbolFact("T", "unsigned", false, true),
      SymbolFact("R", "unsigned", false),
      SymbolFact("R", "long", false),
      SymbolFact("R", "size_t", false),
      SymbolFact("R", "long long", false),
      SymbolFact("TD", "int", false),
      SymbolFact("TD", "long", false, true),
  };
  auto Merged = FactManager::MergeFacts(RawFacts);
  EXPECT_EQ(Merged["X"].TypeName, "short");
  EXPECT_TRUE(Merged["T"].IsTypedef);

  SymbolFact Base("ID", "int", false, false);
  SymbolFact Same("ID", "int", false, false);
  SymbolFact DiffUsr("ID2", "int", false, false);
  SymbolFact DiffType("ID", "long", false, false);
  SymbolFact DiffField("ID", "int", true, false);
  SymbolFact DiffTypedef("ID", "int", false, true);

  EXPECT_TRUE(Base == Same);
  EXPECT_FALSE(Base != Same);
  EXPECT_TRUE(Base != DiffUsr);
  EXPECT_TRUE(Base != DiffType);
  EXPECT_TRUE(Base != DiffField);
  EXPECT_TRUE(Base != DiffTypedef);
  EXPECT_FALSE(Base == DiffUsr);
  EXPECT_FALSE(Base == DiffType);
  EXPECT_FALSE(Base == DiffField);
  EXPECT_FALSE(Base == DiffTypedef);

  std::string GlobalFacts = TempDir + "/global.facts";
  EXPECT_FALSE(FactManager::IsConvergenceReached(GlobalFacts, Facts));
  EXPECT_TRUE(FactManager::WriteFacts(GlobalFacts, Facts));
  EXPECT_TRUE(FactManager::IsConvergenceReached(GlobalFacts, Facts));
  Facts["C"] = SymbolFact("C", "long", false);
  EXPECT_FALSE(FactManager::IsConvergenceReached(GlobalFacts, Facts));
}

GTEST_TEST(Coverage, TypeCorrectExtraPaths) {
  static const char *const Code =
      "#define TYPE_OF(x) int\n"
      "#define INIT_VAR int\n"
      "#define CAST_INT(v) ((int)(v))\n"
      "typedef unsigned long size_t;"
      "size_t get_size();"
      "int main() {"
      "  TYPE_OF(1) y;"
      "  y = get_size();"
      "  INIT_VAR x = get_size();"
      "  auto z = get_size();"
      "  int w = (int)1;"
      "  int q = CAST_INT(2);"
      "  (void)z; (void)w; (void)q;"
      "}";

  // Use the actual TypeCorrect AST action.
  class TestTypeCorrectAction : public clang::ASTFrontendAction {
  public:
    std::unique_ptr<clang::ASTConsumer>
    CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef) override {
      Rewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
      return std::make_unique<TypeCorrectASTConsumer>(
          Rewriter, false, false, "", "", false, false, false,
          type_correct::Phase::Standalone, "", "");
    }

  private:
    clang::Rewriter Rewriter;
  };

  testing::internal::CaptureStdout();
  runToolOnCode(std::make_unique<TestTypeCorrectAction>(), Code);
  std::string Output = testing::internal::GetCapturedStdout();

  EXPECT_THAT(Output, HasSubstr("#define INIT_VAR size_t"));
  EXPECT_THAT(Output, HasSubstr("auto z"));
  EXPECT_THAT(Output, ::testing::Not(HasSubstr("(int)1")));
}

GTEST_TEST(Coverage, CliPaths) {
  std::string CliPath = TYPE_CORRECT_CLI_PATH;
  std::string BuildDir = TYPE_CORRECT_BUILD_DIR;
  std::string SourceDir = TYPE_CORRECT_SOURCE_DIR;

  std::string Input =
      SourceDir + "/type_correct/tests/math_type/b4/math_type.c";

  ASSERT_EQ(RunCommand("\"" + CliPath + "\""), 1);

  std::string CmdBase = "\"" + CliPath + "\" -p \"" + BuildDir +
                        "\" \"" + Input + "\"";

  EXPECT_EQ(RunCommand(CmdBase), 0);
  EXPECT_EQ(RunCommand(CmdBase + " --audit"), 0);
  EXPECT_EQ(RunCommand(CmdBase + " --phase=map"), 0);
  EXPECT_EQ(RunCommand(CmdBase + " --phase=apply"), 0);

  std::string MissingDir = MakeTempDir("type_correct_missing");
  std::string MissingPath = MissingDir + "/does_not_exist";
  EXPECT_EQ(RunCommand(CmdBase + " --phase=reduce --facts-dir=\"" +
                       MissingPath + "\""),
            0);

  EXPECT_EQ(RunCommand(CmdBase + " --phase=reduce"), 0);

  std::string FactsDir = MakeTempDir("type_correct_facts_cli");
  std::string FactsFile = FactsDir + "/chunk.facts";
  {
    std::ofstream Out(FactsFile);
    Out << "X\tint\t0\t0\n";
  }

  std::string GlobalFacts = FactsDir + "/global.facts";
  {
    std::ofstream Out(GlobalFacts);
    Out << "X\tint\t0\t0\n";
  }

  EXPECT_EQ(RunCommand(CmdBase + " --phase=reduce --facts-dir=\"" +
                       FactsDir + "\""),
            0);

  std::string NoReadFile = FactsDir + "/unreadable.facts";
  {
    std::ofstream Out(NoReadFile);
    Out << "Y\tlong\t0\t0\n";
  }
  llvm::sys::fs::setPermissions(NoReadFile, llvm::sys::fs::no_perms);

  EXPECT_EQ(RunCommand(CmdBase + " --phase=reduce --facts-dir=\"" +
                       FactsDir + "\""),
            0);

  llvm::sys::fs::setPermissions(NoReadFile, llvm::sys::fs::perms::all_all);

  std::string ReduceChangeDir = MakeTempDir("type_correct_reduce_change");
  {
    std::ofstream Out(ReduceChangeDir + "/chunk.facts");
    Out << "Z\tint\t0\t0\n";
  }
  EXPECT_EQ(RunCommand(CmdBase + " --phase=reduce --facts-dir=\"" +
                       ReduceChangeDir + "\""),
            1);

  std::string IterChangeDir = MakeTempDir("type_correct_iter_change");
  EXPECT_EQ(RunCommand(CmdBase + " --phase=iterative --facts-dir=\"" +
                       IterChangeDir + "\" --max-iterations=1"),
            0);

  EXPECT_EQ(RunCommand(CmdBase + " --phase=iterative"), 1);
  EXPECT_EQ(RunCommand(CmdBase + " --phase=iterative --facts-dir=\"" +
                       FactsDir + "\" --max-iterations=1"),
            0);

  std::string IterFactsDir = MakeTempDir("type_correct_iterative");
  {
    std::ofstream Out(IterFactsDir + "/chunk.facts");
    Out << "X\tint\t0\t0\n";
  }
  {
    std::ofstream Out(IterFactsDir + "/global.facts");
    Out << "X\tint\t0\t0\n";
  }
  EXPECT_EQ(RunCommand(CmdBase + " --phase=iterative --facts-dir=\"" +
                       IterFactsDir + "\" --max-iterations=1"),
            0);

  std::string ReadOnlyDir = MakeTempDir("type_correct_readonly");
  {
    std::ofstream Out(ReadOnlyDir + "/chunk.facts");
    Out << "X\tint\t0\t0\n";
  }
  llvm::sys::fs::setPermissions(
      ReadOnlyDir,
      llvm::sys::fs::perms::owner_read | llvm::sys::fs::perms::owner_exe);
  EXPECT_EQ(RunCommand(CmdBase + " --phase=reduce --facts-dir=\"" +
                       ReadOnlyDir + "\""),
            0);
  llvm::sys::fs::setPermissions(ReadOnlyDir,
                                llvm::sys::fs::perms::all_all);

  std::string MissingInput = ReadOnlyDir + "/missing.cpp";
  EXPECT_NE(RunCommand("\"" + CliPath + "\" -p \"" + BuildDir + "\" \"" +
                       MissingInput +
                       "\" --phase=iterative --facts-dir=\"" + FactsDir +
                       "\" --max-iterations=1"),
            0);
}

GTEST_TEST(Coverage, PluginParseArgs) {
  TypeCorrectPluginAction Action;
  clang::CompilerInstance CI;
  std::vector<std::string> Args;
  EXPECT_TRUE(Action.ParseArgs(CI, Args));
}

GTEST_TEST(Coverage, TypeCorrectHelperFunctions) {
  static const char *const Code =
      "struct Fwd;"
      "struct Foo { int x; };"
      "struct __attribute__((packed)) PackedRec { int y; };"
      "typedef int __attribute__((aligned(4))) AlignedInt;"
      "typedef int (ParenInt);"
      "int (*fp)(int) = nullptr;"
      "const int qualified = 0;"
      "struct Foo foo;"
      "AlignedInt aligned = 0;"
      "int *ptr = nullptr;"
      "int &ref = *ptr;"
      "int arr[3];"
      "int Foo::*member_ptr = &Foo::x;"
      "ParenInt paren_var = 0;"
      "int main() { return foo.x + qualified + aligned + arr[0]; }";

  ASSERT_TRUE(RunWithAST(Code, [](ASTContext &Ctx) {
    const VarDecl *Qualified = FindVarDecl(Ctx, "qualified");
    const VarDecl *Aligned = FindVarDecl(Ctx, "aligned");
    const VarDecl *Ptr = FindVarDecl(Ctx, "ptr");
    const VarDecl *Ref = FindVarDecl(Ctx, "ref");
    const VarDecl *Arr = FindVarDecl(Ctx, "arr");
    const VarDecl *MemberPtr = FindVarDecl(Ctx, "member_ptr");
    const VarDecl *ParenVar = FindVarDecl(Ctx, "paren_var");
    const VarDecl *Fp = FindVarDecl(Ctx, "fp");
    const VarDecl *FooVar = FindVarDecl(Ctx, "foo");
    ASSERT_NE(Qualified, nullptr);
    ASSERT_NE(Aligned, nullptr);
    ASSERT_NE(Ptr, nullptr);
    ASSERT_NE(Ref, nullptr);
    ASSERT_NE(Arr, nullptr);
    ASSERT_NE(MemberPtr, nullptr);
    ASSERT_NE(ParenVar, nullptr);
    ASSERT_NE(Fp, nullptr);
    ASSERT_NE(FooVar, nullptr);

    if (Qualified->getTypeSourceInfo())
      type_correct::test_support::GetBaseTypeLocForTest(
          Qualified->getTypeSourceInfo()->getTypeLoc());
    if (FooVar->getTypeSourceInfo())
      type_correct::test_support::GetBaseTypeLocForTest(
          FooVar->getTypeSourceInfo()->getTypeLoc());
    if (Aligned->getTypeSourceInfo())
      type_correct::test_support::GetBaseTypeLocForTest(
          Aligned->getTypeSourceInfo()->getTypeLoc());
    if (Ptr->getTypeSourceInfo())
      type_correct::test_support::GetBaseTypeLocForTest(
          Ptr->getTypeSourceInfo()->getTypeLoc());
    if (Ref->getTypeSourceInfo())
      type_correct::test_support::GetBaseTypeLocForTest(
          Ref->getTypeSourceInfo()->getTypeLoc());
    if (Arr->getTypeSourceInfo())
      type_correct::test_support::GetBaseTypeLocForTest(
          Arr->getTypeSourceInfo()->getTypeLoc());
    if (MemberPtr->getTypeSourceInfo())
      type_correct::test_support::GetBaseTypeLocForTest(
          MemberPtr->getTypeSourceInfo()->getTypeLoc());
    if (ParenVar->getTypeSourceInfo())
      type_correct::test_support::GetBaseTypeLocForTest(
          ParenVar->getTypeSourceInfo()->getTypeLoc());
    if (Fp->getTypeSourceInfo())
      type_correct::test_support::GetBaseTypeLocForTest(
          Fp->getTypeSourceInfo()->getTypeLoc());

    type_correct::test_support::NormalizeTypeForTest(QualType());
    type_correct::test_support::TypeToStringForTest(QualType(), Ctx);

    const RecordDecl *Fwd = FindRecordDecl(Ctx, "Fwd");
    const RecordDecl *FooRec = FindRecordDecl(Ctx, "Foo");
    ASSERT_NE(Fwd, nullptr);
    ASSERT_NE(FooRec, nullptr);
    QualType IncompleteTy = Ctx.getRecordType(Fwd);
    QualType CompleteTy = Ctx.getRecordType(FooRec);

    type_correct::test_support::GetWiderTypeForTest(QualType(), Ctx.IntTy, Ctx);
    type_correct::test_support::GetWiderTypeForTest(Ctx.IntTy, QualType(), Ctx);
    type_correct::test_support::GetWiderTypeForTest(IncompleteTy, Ctx.IntTy,
                                                    Ctx);
    type_correct::test_support::GetWiderTypeForTest(CompleteTy, Ctx.IntTy, Ctx);
    type_correct::test_support::GetWiderTypeForTest(Ctx.LongLongTy, Ctx.IntTy,
                                                    Ctx);
    type_correct::test_support::GetWiderTypeForTest(Ctx.IntTy,
                                                    Ctx.UnsignedIntTy, Ctx);
    type_correct::test_support::GetWiderTypeForTest(Ctx.UnsignedIntTy,
                                                    Ctx.IntTy, Ctx);

    const MemberExpr *Member = FindFirstMemberExpr(Ctx);
    const DeclRefExpr *DeclRef = FindFirstDeclRef(Ctx);
    const Expr *Literal =
        IntegerLiteral::Create(Ctx, llvm::APInt(32, 7), Ctx.IntTy,
                               SourceLocation());
    EXPECT_EQ(type_correct::test_support::ResolveNamedDeclForTest(nullptr),
              nullptr);
    EXPECT_NE(type_correct::test_support::ResolveNamedDeclForTest(DeclRef),
              nullptr);
    EXPECT_NE(type_correct::test_support::ResolveNamedDeclForTest(Member),
              nullptr);
    EXPECT_EQ(type_correct::test_support::ResolveNamedDeclForTest(Literal),
              nullptr);

    EXPECT_FALSE(type_correct::test_support::IsIdentifierForTest(""));
    EXPECT_FALSE(type_correct::test_support::IsIdentifierForTest("1bad"));
    EXPECT_FALSE(type_correct::test_support::IsIdentifierForTest("bad!"));
    EXPECT_TRUE(type_correct::test_support::IsIdentifierForTest("good_name"));

    clang::Rewriter Rewriter;
    Rewriter.setSourceMgr(Ctx.getSourceManager(), Ctx.getLangOpts());
    TypeCorrectMatcher Matcher(Rewriter, false, false, "", "", false, false,
                               false, type_correct::Phase::Standalone, "", "");
    (void)Matcher.GetChanges();
  }));
}

GTEST_TEST(Coverage, TypeCorrectVisitorEdgeCases) {
  static const char *const Code = "int main() { return 0; }";
  ASSERT_TRUE(RunWithAST(Code, [](ASTContext &Ctx) {
    type_correct::test_support::CoverVisitorEdgeCases(Ctx);
  }));
}

GTEST_TEST(Coverage, TypeCorrectEnsureTemplateArgUpdateEdges) {
  static const char *const Code =
      "template <typename T> struct Vec { void push_back(T); };"
      "template <int N> struct Num { void push_back(int); };"
      "struct Plain { void push_back(int); };"
      "int main() {"
      "  Vec<int> vec;"
      "  Num<3> num;"
      "  Plain plain;"
      "  vec.push_back(1);"
      "  num.push_back(2);"
      "  plain.push_back(3);"
      "  return 0;"
      "}";

  ASSERT_TRUE(RunWithAST(Code, [](ASTContext &Ctx) {
    const VarDecl *VecDecl = FindVarDecl(Ctx, "vec");
    const VarDecl *NumDecl = FindVarDecl(Ctx, "num");
    const VarDecl *PlainDecl = FindVarDecl(Ctx, "plain");
    ASSERT_NE(VecDecl, nullptr);
    ASSERT_NE(NumDecl, nullptr);
    ASSERT_NE(PlainDecl, nullptr);

    type_correct::test_support::CoverEnsureTemplateArgUpdateEdges(
        Ctx, VecDecl, PlainDecl, NumDecl,
        const_cast<VarDecl *>(VecDecl));
  }));
}

GTEST_TEST(Coverage, TypeCorrectMapEdgeCases) {
  static const char *const Code = "int main() { return 0; }";
  ASSERT_TRUE(RunWithAST(Code, [](ASTContext &Ctx) {
    clang::Rewriter Rewriter;
    Rewriter.setSourceMgr(Ctx.getSourceManager(), Ctx.getLangOpts());
    type_correct::test_support::CoverTemplateUpdateMapEdges(Ctx, Rewriter);
    type_correct::test_support::CoverDeclUpdateMapEdges(Ctx, Rewriter);
    EXPECT_TRUE(type_correct::test_support::CoverMacroScannerEdges(Ctx, Rewriter));
    type_correct::test_support::CoverRecordChangeEdges(Ctx, Rewriter);
  }));
}

GTEST_TEST(Coverage, TypeCorrectRedundantCastEdges) {
  static const char *const Code = "int main() { return 0; }";
  ASSERT_TRUE(RunWithAST(Code, [](ASTContext &Ctx) {
    clang::Rewriter Rewriter;
    Rewriter.setSourceMgr(Ctx.getSourceManager(), Ctx.getLangOpts());
    TypeCorrectMatcher Matcher(Rewriter, false, false, "", "", false, false,
                               false, type_correct::Phase::Standalone, "", "");
    SourceManager &SM = Ctx.getSourceManager();
    SourceLocation MainLoc = SM.getLocForStartOfFile(SM.getMainFileID());
    SourceLocation MacroLoc =
        SM.createExpansionLoc(MainLoc, MainLoc, MainLoc, 1);

    auto MakeCast = [&](Expr *SubExpr, SourceLocation Begin,
                        SourceLocation End, TypeSourceInfo *TSI) {
      const CXXCastPath *BasePath = nullptr;
#if LLVM_VERSION_MAJOR >= 19
      return CStyleCastExpr::Create(
          Ctx, Ctx.IntTy, type_correct::clang_compat::PrValueKind(), CK_NoOp,
          SubExpr, BasePath, clang::FPOptionsOverride(), TSI, Begin, End);
#else
      return CStyleCastExpr::Create(
          Ctx, Ctx.IntTy, type_correct::clang_compat::PrValueKind(), CK_NoOp,
          SubExpr, BasePath, TSI, Begin, End);
#endif
    };

    Matcher.ExplicitCasts.push_back(nullptr);

    auto *LiteralMain =
        IntegerLiteral::Create(Ctx, llvm::APInt(32, 1), Ctx.IntTy, MainLoc);
    auto *CastDuplicate = MakeCast(
        LiteralMain, MainLoc, MainLoc, Ctx.getTrivialTypeSourceInfo(Ctx.IntTy));
    Matcher.ExplicitCasts.push_back(CastDuplicate);
    Matcher.ExplicitCasts.push_back(CastDuplicate);

    auto *LiteralInvalid = IntegerLiteral::Create(Ctx, llvm::APInt(32, 2),
                                                  Ctx.IntTy, SourceLocation());
    auto *CastInvalid =
        MakeCast(LiteralInvalid, SourceLocation(), SourceLocation(),
                 Ctx.getTrivialTypeSourceInfo(Ctx.IntTy));
    Matcher.ExplicitCasts.push_back(CastInvalid);

    auto OtherBuffer = llvm::MemoryBuffer::getMemBuffer("int x;", "other.h");
    FileID OtherFile =
        SM.createFileID(std::move(OtherBuffer), SrcMgr::C_User);
    SourceLocation OtherLoc = SM.getLocForStartOfFile(OtherFile);
    auto *LiteralOther =
        IntegerLiteral::Create(Ctx, llvm::APInt(32, 3), Ctx.IntTy, OtherLoc);
    auto *CastOther =
        MakeCast(LiteralOther, OtherLoc, OtherLoc,
                 Ctx.getTrivialTypeSourceInfo(Ctx.IntTy));
    Matcher.ExplicitCasts.push_back(CastOther);

    auto *CastNoSub = MakeCast(
        LiteralMain, MainLoc, MainLoc, Ctx.getTrivialTypeSourceInfo(Ctx.IntTy));
    Matcher.ExplicitCasts.push_back(CastNoSub);

    auto *LiteralSubInvalid = IntegerLiteral::Create(
        Ctx, llvm::APInt(32, 4), Ctx.IntTy, SourceLocation());
    auto *CastSubInvalid =
        MakeCast(LiteralSubInvalid, MainLoc, MainLoc,
                 Ctx.getTrivialTypeSourceInfo(Ctx.IntTy));
    Matcher.ExplicitCasts.push_back(CastSubInvalid);

    auto *LiteralSubMacro =
        IntegerLiteral::Create(Ctx, llvm::APInt(32, 5), Ctx.IntTy, MacroLoc);
    auto *CastSubMacro =
        MakeCast(LiteralSubMacro, MainLoc, MainLoc,
                 Ctx.getTrivialTypeSourceInfo(Ctx.IntTy));
    Matcher.ExplicitCasts.push_back(CastSubMacro);

    auto *Opaque = new (Ctx)
        OpaqueValueExpr(SourceLocation(), Ctx.IntTy,
                        type_correct::clang_compat::PrValueKind());
    auto *CastNullType =
        MakeCast(Opaque, MainLoc, MainLoc,
                 Ctx.getTrivialTypeSourceInfo(Ctx.IntTy));
    CastNullType->setTypeInfoAsWritten(nullptr);
    Matcher.ExplicitCasts.push_back(CastNullType);

    auto EmptyBuffer = llvm::MemoryBuffer::getMemBuffer("", "empty.h");
    FileID EmptyFile =
        SM.createFileID(std::move(EmptyBuffer), SrcMgr::C_User);
    SourceLocation EmptyLoc = SM.getLocForStartOfFile(EmptyFile);
    auto *LiteralEmpty =
        IntegerLiteral::Create(Ctx, llvm::APInt(32, 6), Ctx.IntTy, EmptyLoc);
    auto *CastEmptyText =
        MakeCast(LiteralEmpty, MainLoc, MainLoc,
                 Ctx.getTrivialTypeSourceInfo(Ctx.IntTy));
    Matcher.ExplicitCasts.push_back(CastEmptyText);

    Matcher.ProcessRedundantCasts(Ctx);
  }));
}

GTEST_TEST(Coverage, TypeCorrectNullMatchResult) {
  static const char *const Code = "int x = 0;";
  ASSERT_TRUE(RunWithAST(Code, [](ASTContext &Ctx) {
    using namespace clang::ast_matchers;
    const VarDecl *Var = FindVarDecl(Ctx, "x");
    ASSERT_NE(Var, nullptr);
    auto Matches = match(varDecl().bind("v"), *Var, Ctx);
    ASSERT_FALSE(Matches.empty());

    clang::Rewriter Rewriter;
    TypeCorrectMatcher Matcher(Rewriter, false, false, "", "", false, false,
                               false, type_correct::Phase::Standalone, "", "");
    MatchFinder::MatchResult Result(Matches.front(), &Ctx);
    Matcher.run(Result);
  }));
}

GTEST_TEST(Coverage, TypeCorrectInPlacePaths) {
  static const char *const Code = "int main() { int a = 0; return a; }";
  ASSERT_TRUE(RunWithAST(Code, [](ASTContext &Ctx) {
    clang::Rewriter Rewriter;
    Rewriter.setSourceMgr(Ctx.getSourceManager(), Ctx.getLangOpts());
    TypeCorrectMatcher Matcher(Rewriter, false, false, "", "", true, false,
                               false, type_correct::Phase::Standalone, "", "");
    Matcher.onEndOfTranslationUnit(Ctx);
  }));
}
