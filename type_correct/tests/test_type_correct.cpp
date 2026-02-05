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
  TestTypeCorrectAction(bool UseDecltype = false, bool ExpandAuto = false) 
      : UseDecltype(UseDecltype), ExpandAuto(ExpandAuto) {} 

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef) override { 
    Rewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts()); 
    return std::make_unique<TypeCorrectASTConsumer>(Rewriter, UseDecltype, ExpandAuto); 
  } 
private: 
  clang::Rewriter Rewriter; 
  bool UseDecltype; 
  bool ExpandAuto; 
}; 

static std::string runToolOnString(llvm::StringRef Code, bool UseDecltype = false, bool ExpandAuto = false) { 
    testing::internal::CaptureStdout(); 
    runToolOnCode(std::make_unique<TestTypeCorrectAction>(UseDecltype, ExpandAuto), Code); 
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