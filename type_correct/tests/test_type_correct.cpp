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

#include <type_correct/TypeCorrectMain.h>

using namespace clang::tooling;

/**
 * @brief Wrapper to capture stdout/stderr when running the tool.
 *
 * @param Code The C/C++ source code to analyze.
 * @return std::string The combined stdout output from the tool.
 */
static std::string runToolOnString(llvm::StringRef Code) {
    testing::internal::CaptureStdout();
    runToolOnCode(std::make_unique<TypeCorrectPluginAction>(), Code);
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