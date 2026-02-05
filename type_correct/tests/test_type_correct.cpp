/**
 * @file test_type_correct.cpp
 * @brief Integration tests for TypeCorrect.
 *
 * Runs the tool on string snippets and verifies that the AST Matchers
 * and Rewriter correctly transform incorrectly typed code.
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

/**
 * @class TestTypeCorrectAction
 * @brief A configurable helper Action for testing the ASTConsumer with different flags.
 */
class TestTypeCorrectAction : public clang::ASTFrontendAction {
public:
  TestTypeCorrectAction(bool UseDecltype = false, bool ExpandAuto = false)
      : UseDecltype(UseDecltype), ExpandAuto(ExpandAuto) {}

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI,
                                                        llvm::StringRef) override {
    Rewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    return std::make_unique<TypeCorrectASTConsumer>(Rewriter, UseDecltype, ExpandAuto);
  }

private:
  clang::Rewriter Rewriter;
  bool UseDecltype;
  bool ExpandAuto;
};

/**
 * @brief Wrapper to capture stdout/stderr when running the tool.
 *
 * @param Code The C/C++ source code to analyze.
 * @param UseDecltype Enable decltype optimization flag.
 * @param ExpandAuto Enable auto expansion flag.
 * @return std::string The combined stdout output from the tool.
 */
static std::string runToolOnString(llvm::StringRef Code, 
                                   bool UseDecltype = false, 
                                   bool ExpandAuto = false) {
    testing::internal::CaptureStdout();
    runToolOnCode(std::make_unique<TestTypeCorrectAction>(UseDecltype, ExpandAuto), Code);
    return testing::internal::GetCapturedStdout();
}

/**
 * @test StringFunctionReturnType
 * @brief Verifies rewriting of 'int' to 'size_t' when assigned from 'strlen'.
 */
GTEST_TEST(TypeCorrect, StringFunctionReturnType) {
  static const char *const Code = 
      "typedef unsigned long size_t;" 
      "size_t strlen(const char*);" 
      "int main(void) { const int n = strlen(\"FOO\"); }";
      
  std::string Output = runToolOnString(Code);
  
  // Expect 'int n' to become 'size_t n'.
  EXPECT_THAT(Output, ::testing::HasSubstr("const size_t n")); 
  // Verify 'int' is gone from the variable decl
  EXPECT_THAT(Output, ::testing::Not(::testing::HasSubstr("const int n"))); 
}

/**
 * @test ForLoopComparator
 * @brief Verifies rewriting of 'for(int i...)' to 'for(size_t i...)' based on limit.
 */
GTEST_TEST(TypeCorrect, ForLoopComparator) {
  static const char *const Code = 
      "typedef unsigned long size_t;" 
      "size_t strlen(const char*);" 
      "int main(void) {" 
      "    for(int i=0; i<strlen(\"FOO\"); i++) {}" 
      "}";

  std::string Output = runToolOnString(Code);
  
  // Expect 'int i' to become 'size_t i'
  EXPECT_THAT(Output, ::testing::HasSubstr("for(size_t i=0")); 
}

/**
 * @test FunctionReturnType
 * @brief Verifies rewriting of function return type to match the returned value.
 */
GTEST_TEST(TypeCorrect, FunctionReturnType) {
  static const char *const Code = 
      "int f(long b) { return b; }";

  std::string Output = runToolOnString(Code);
  
  // Expect 'int f' to become 'long f'
  EXPECT_THAT(Output, ::testing::HasSubstr("long f(long b)")); 
}

/**
 * @test CorrectCodeIgnored
 * @brief Verifies that code with matching types is NOT modified.
 */
GTEST_TEST(TypeCorrect, CorrectCodeIgnored) {
  static const char *const Code = 
      "typedef unsigned long size_t;" 
      "size_t strlen(const char*);" 
      "int main(void) { const size_t n = strlen(\"FOO\"); }";
      
  std::string Output = runToolOnString(Code);
  
  // Should match original exactly (no edits)
  EXPECT_THAT(Output, ::testing::HasSubstr("const size_t n")); 
}

//-----------------------------------------------------------------------------
// New C++ Specific Tests
//-----------------------------------------------------------------------------

/**
 * @test VectorSizeMemberCall
 * @brief Verifies rewriting of loop variable when comparing against std::vector::size().
 */
GTEST_TEST(TypeCorrect, VectorSizeMemberCall) {
  static const char *const Code = 
      "namespace std { "
      "  template<typename T> struct vector { "
      "    typedef unsigned long size_type; "
      "    size_type size() const { return 0; } "
      "  }; "
      "} "
      "int main() { "
      "  std::vector<int> numbers; "
      "  for(int i=0; i < numbers.size(); ++i) {} "
      "}";

  std::string Output = runToolOnString(Code);

  // Default behavior without -use-decltype is canonical type size_t (unsigned long)
  // Our matching logic detects the type mismatch (int vs unsigned long).
  // The replacement should be the canonical type string 'unsigned long'
  // because size_type is a nested typedef.
  
  EXPECT_THAT(Output, ::testing::HasSubstr("for(unsigned long i=0")); 
}

/**
 * @test VectorSizeDecltypeStrategy
 * @brief Verifies rewriting using 'decltype(instance)::size_type'.
 */
GTEST_TEST(TypeCorrect, VectorSizeDecltypeStrategy) {
  static const char *const Code = 
      "namespace std { "
      "  template<typename T> struct vector { "
      "    typedef unsigned long size_type; "
      "    size_type size() const { return 0; } "
      "  }; "
      "} "
      "int main() { "
      "  std::vector<int> numbers; "
      "  for(int i=0; i < numbers.size(); ++i) {} "
      "}";

  // Enable UseDecltype = true
  std::string Output = runToolOnString(Code, /*UseDecltype=*/true);

  // Expect 'int i' to become 'decltype(numbers)::size_type i'
  EXPECT_THAT(Output, ::testing::HasSubstr("for(decltype(numbers)::size_type i=0")); 
}

/**
 * @test LoopInitMismatch (Downward Counting)
 * @brief Verifies rewriting when the loop is initialized by the sizing function.
 * Pattern: for(int i = strlen(s); i != 0; --i)
 */
GTEST_TEST(TypeCorrect, LoopInitMismatch) {
  static const char *const Code = 
      "typedef unsigned long size_t;" 
      "size_t strlen(const char*);" 
      "int main(void) {" 
      "    for(int i = strlen(\"FOO\"); i != 0; --i) {}" 
      "}";

  std::string Output = runToolOnString(Code);
  
  // Expect 'int i' to become 'size_t i' (or unsigned long)
  // Because 'i' is initialized by strlen() which returns size_t.
  EXPECT_THAT(Output, ::testing::HasSubstr("for(size_t i = strlen")); 
}

/**
 * @test AutoKeywordResolution
 * @brief Verifies 'auto' is replaced by explicit type when using 'UseDecltype' on mismatch.
 * 
 * Note: 'auto i = 0' infers 'int'. 'i < numbers.size()' is 'int < ulong'.
 * Mismatch detected. 'auto' should be replaced.
 */
GTEST_TEST(TypeCorrect, AutoKeywordResolution) {
  static const char *const Code = 
      "namespace std { "
      "  template<typename T> struct vector { "
      "    typedef unsigned long size_type; "
      "    size_type size() const { return 0; } "
      "  }; "
      "} "
      "int main() { "
      "  std::vector<int> numbers; "
      "  for(auto i=0; i < numbers.size(); ++i) {} "
      "}";

  // Enable UseDecltype = true for specific output string
  std::string Output = runToolOnString(Code, /*UseDecltype=*/true);

  EXPECT_THAT(Output, ::testing::HasSubstr("for(decltype(numbers)::size_type i=0")); 
}

/**
 * @test AutoPreservationOnCall
 * @brief Verifies that 'auto' is PRESERVED if initialized by a function call, 
 * even if there is a mismatch? 
 * 
 * Logic being tested: If ExpandAuto=false, and init is a call, skip rewrite.
 */
GTEST_TEST(TypeCorrect, AutoPreservationOnCall) {
  // Case: function return mismatch in a loop to trigger the matcher.
  static const char *const AutoLoopCode = 
      "long get_limit() { return 100L; }" 
      "int get_start() { return 0; }"
      "int main() {" 
      "  for(auto i = get_start(); i < get_limit(); ++i) {}" 
      "}";
      
  // i is int (from get_start). limit is long. Mismatch loop condition.
  // Default behavior: ExpandAuto = false.
  // 'i' is initialized by CallExpr 'get_start()'.
  // Should NOT rewrite 'auto' to 'long'.
  
  std::string Output = runToolOnString(AutoLoopCode, /*UseDecltype=*/false, /*ExpandAuto=*/false);
  
  // Should still contain 'auto i'
  EXPECT_THAT(Output, ::testing::HasSubstr("for(auto i = get_start")); 
}

/**
 * @test AutoExpansionForced
 * @brief Verifies that 'auto' IS replaced if 'ExpandAuto' is set, even if initialized by call.
 */
GTEST_TEST(TypeCorrect, AutoExpansionForced) {
  static const char *const AutoLoopCode = 
      "long get_limit() { return 100L; }" 
      "int get_start() { return 0; }"
      "int main() {" 
      "  for(auto i = get_start(); i < get_limit(); ++i) {}" 
      "}";

  // Force expansion
  std::string Output = runToolOnString(AutoLoopCode, /*UseDecltype=*/false, /*ExpandAuto=*/true);
  
  // Should now be rewritten to 'long' (canonical type of loop limit)
  EXPECT_THAT(Output, ::testing::HasSubstr("for(long i = get_start")); 
}