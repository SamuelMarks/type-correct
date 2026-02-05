/** 
 * @file test_type_correct.cpp
 * @brief Integration tests for TypeCorrect. 
 * 
 * Runs the tool on string snippets. 
 * Covers: Rewriting, Back-Tracking, Casting, Widest Type, and Argument Passing. 
 * 
 * @author Samuel Marks
 * @license CC0
 */ 

#include <clang/Tooling/Tooling.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <type_correct/TypeCorrect.h>

using namespace clang; 
using namespace clang::tooling; 

class TestTypeCorrectAction : public clang::ASTFrontendAction { 
public: 
  explicit TestTypeCorrectAction(bool UseDecltype = false, 
                                 bool ExpandAuto = false, 
                                 std::string Exclude = "",
                                 bool InPlace = false) 
      : UseDecltype(UseDecltype), ExpandAuto(ExpandAuto), Exclude(Exclude), InPlace(InPlace) {} 

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef) override { 
    Rewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts()); 
    // Pass empty ProjectRoot to allow default MainFile behaviors for string tests, 
    // unless testing full integration. Passing exclude/in-place pattern here.
    return std::make_unique<TypeCorrectASTConsumer>(Rewriter, UseDecltype, ExpandAuto, "", Exclude, InPlace); 
  } 
private: 
  clang::Rewriter Rewriter; 
  bool UseDecltype; 
  bool ExpandAuto; 
  std::string Exclude;
  bool InPlace;
}; 

static std::string runToolOnString(llvm::StringRef Code, bool UseDecltype = false, bool ExpandAuto = false, std::string Exclude = "", bool InPlace = false) { 
    testing::internal::CaptureStdout(); 
    runToolOnCode(std::make_unique<TestTypeCorrectAction>(UseDecltype, ExpandAuto, Exclude, InPlace), Code); 
    return testing::internal::GetCapturedStdout(); 
} 

/** 
 * @test WidestTypeResolution
 * @brief Verifies that the tool picks the 'widest' type when conflicting assignments exist. 
 */ 
GTEST_TEST(TypeCorrect, WidestTypeResolution) { 
  static const char *const Code =
      "typedef unsigned long size_t;" 
      "size_t get_size();" 
      "int get_int();" 
      "int main(void) {" 
      "   int i;" 
      "   i = get_int();" 
      "   i = get_size();" 
      "}"; 

  std::string Output = runToolOnString(Code); 
  EXPECT_THAT(Output, ::testing::HasSubstr("size_t i;")); 
} 

/** 
 * @test ArgumentPassingMismatch
 * @brief Verifies rewriting of variables passed to functions expecting wider types. 
 * 
 * Scenario: 
 *  void my_memset(void*, int, size_t count); 
 *  int n = 10; 
 *  my_memset(buf, 0, n); 
 * 
 * Outcome: 
 *  'n' is 'int', passed to 'size_t count'. 
 *  CK_IntegralCast (int -> size_t) detected. 
 *  Widest Type logic sees 'size_t' required. 
 *  'int n' rewritten to 'size_t n'. 
 */ 
GTEST_TEST(TypeCorrect, ArgumentPassingMismatch) { 
  static const char *const Code =
      "typedef unsigned long size_t;" 
      "void my_memset(void* p, int v, size_t count);" 
      "int main(void) {" 
      "   int n = 10;" 
      "   void* buf;" 
      "   my_memset(buf, 0, n);" 
      "}"; 

  std::string Output = runToolOnString(Code); 

  // Expect 'int n' to become 'size_t n'. 
  EXPECT_THAT(Output, ::testing::HasSubstr("size_t n = 10;")); 
  EXPECT_THAT(Output, ::testing::Not(::testing::HasSubstr("int n = 10;"))); 
} 

/** 
 * @test ArgumentPassingCastFallback
 * @brief Verifies explicit cast injection when passing unmodifiable variables as args. 
 */ 
GTEST_TEST(TypeCorrect, ArgumentPassingCastFallback) { 
  static const char *const Code =
      "typedef unsigned long size_t;" 
      "void my_memset(void* p, int v, size_t count);\n" 
      "#define DEF_VAR int n = 10\n" 
      "int main(void) {" 
      "   DEF_VAR;" 
      "   void* buf;" 
      "   my_memset(buf, 0, n);" 
      "}"; 

  std::string Output = runToolOnString(Code); 

  // 'n' cannot be rewritten (macro). 
  // Expect static_cast<size_t>(n) in the call. 
  // Note: we specifically look for the cast around 'n'. 
  EXPECT_THAT(Output, ::testing::HasSubstr("my_memset(buf, 0, static_cast<size_t>(n));")); 
} 

/** 
 * @test NegativeLiteralSafeguard
 * @brief Verifies that variables assigned negative literals are NOT promoted to unsigned types.
 */ 
GTEST_TEST(TypeCorrect, NegativeLiteralSafeguard) { 
  static const char *const Code =
      "typedef unsigned long size_t;" 
      "size_t get_size();" 
      "int main(void) {" 
      "   int i = -1;" 
      "   i = get_size();" 
      "}"; 

  std::string Output = runToolOnString(Code); 

  // Expect 'int i' to remain. 
  EXPECT_THAT(Output, ::testing::HasSubstr("int i = -1;")); 
  EXPECT_THAT(Output, ::testing::Not(::testing::HasSubstr("size_t i = -1;"))); 
} 

/** 
 * @test FormatStringUpdater
 * @brief Verifies checking and updating of printf format strings.
 */ 
GTEST_TEST(TypeCorrect, FormatStringUpdater) { 
  static const char *const Code =
      "typedef unsigned long size_t;" 
      "int printf(const char *format, ...);"
      "size_t get_size();" 
      "int main(void) {" 
      "   int i = 0;" 
      "   i = get_size();" 
      "   printf(\"Val: %d\\n\", i);" 
      "}"; 

  std::string Output = runToolOnString(Code); 

  EXPECT_THAT(Output, ::testing::HasSubstr("size_t i = 0;")); 
  EXPECT_THAT(Output, ::testing::HasSubstr("printf(\"Val: %zu\\n\", i);")); 
} 

/** 
 * @test MultiVariableSplitting
 * @brief Verifies that multiple variable declarations are split when types diverge.
 * 
 * Scenario:
 *  int a = get_size(), b = 0;
 *  a needs to be size_t. 
 *  b should remain int.
 * 
 * Outcome:
 *  Rewritten to: size_t a = get_size(); int b = 0; 
 */ 
GTEST_TEST(TypeCorrect, MultiVariableSplitting) { 
  static const char *const Code =
      "typedef unsigned long size_t;" 
      "size_t get_size();" 
      "int main(void) {" 
      "   int a = get_size(), b = 0;" 
      "}"; 

  std::string Output = runToolOnString(Code); 

  // Should split into separate lines/statements
  EXPECT_THAT(Output, ::testing::HasSubstr("size_t a = get_size();")); 
  EXPECT_THAT(Output, ::testing::HasSubstr("int b = 0;")); 
  
  // Ensure the original combined decl is gone
  EXPECT_THAT(Output, ::testing::Not(::testing::HasSubstr("int a = get_size(), b = 0;"))); 
} 

/**
 * @test ExcludePattern
 * @brief Validates that the consumer accepts exclude patterns (API test).
 * 
 * Logic inspection of IsModifiable confirms functionality, but in-memory testing 
 * of file exclusions is limited without virtual file system mocks.
 */
GTEST_TEST(TypeCorrect, ExcludePattern) {
  // Just ensure the API accepts the argument without crashing.
  static const char *const Code = "int main(){}";
  std::string Output = runToolOnString(Code, false, false, ".*vendor.*");
  EXPECT_THAT(Output, ::testing::HasSubstr("main"));
}

/**
 * @test PointerArithmeticPromotion
 * @brief Verifies that pointer subtraction results in promotion to ptrdiff_t (or equivalent width).
 */
GTEST_TEST(TypeCorrect, PointerArithmeticPromotion) {
  static const char *const Code =
      "int main(void) {"
      "   char *p = 0;"
      "   char *q = 0;"
      "   int i = p - q;"
      "}";

  std::string Output = runToolOnString(Code);

  EXPECT_THAT(Output, ::testing::Not(::testing::HasSubstr("int i = p - q;")));
  EXPECT_THAT(Output, ::testing::AnyOf(
                        ::testing::HasSubstr("long i = p - q;"), 
                        ::testing::HasSubstr("long long i = p - q;"),
                        ::testing::HasSubstr("ptrdiff_t i = p - q;")));
}

/**
 * @test NamespaceQualification
 * @brief Verifies that types within namespace std are printed with their prefix.
 * 
 * Scenario:
 *  namespace std { typedef unsigned long size_t; }
 *  std::size_t get_size();
 *  int i = get_size();
 * 
 * Outcome:
 *  Rewrites to: std::size_t i = get_size();
 *  (Without FullyQualifiedName=true, it might skip std:: if SuppressScope was true).
 */
GTEST_TEST(TypeCorrect, NamespaceQualification) {
  static const char *const Code =
      "namespace std { typedef unsigned long size_t; }"
      "std::size_t get_size();" 
      "int main(void) {" 
      "   int i;" 
      "   i = get_size();" 
      "}"; 
  
  // We expect explicit 'std::size_t' in the output, since the 
  // PrintingPolicy config enforces qualification.
  std::string Output = runToolOnString(Code);
  
  EXPECT_THAT(Output, ::testing::HasSubstr("std::size_t i;"));
  EXPECT_THAT(Output, ::testing::Not(::testing::HasSubstr("int i;")));
}