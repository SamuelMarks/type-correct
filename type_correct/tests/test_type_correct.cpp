/** 
 * @file test_type_correct.cpp
 * @brief Integration tests for TypeCorrect. 
 * 
 * Runs the tool on string snippets. 
 * Covers: Rewriting, Back-Tracking, Casting, Widest Type, Template Args, Macro Expansion, and ABI Safety. 
 * 
 * @author SamuelMarks
 * @license CC0
 */ 

#include <clang/Frontend/CompilerInstance.h>
#include <clang/Tooling/Tooling.h>
#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <type_correct/TypeCorrect.h>

using namespace clang; 
using namespace clang::tooling; 

/** 
 * @class TestTypeCorrectAction
 * @brief Helper action for running TypeCorrect in a test environment. 
 * 
 * Configures the ASTConsumer with test-specific toggles (e.g. enabling ABI changes). 
 */ 
class TestTypeCorrectAction : public clang::ASTFrontendAction { 
public: 
  /** 
   * @brief Constructor for the test action. 
   * 
   * @param UseDecltype Toggle decltype syntax. 
   * @param ExpandAuto Toggle auto expansion. 
   * @param Exclude Exclusion regex pattern. 
   * @param InPlace Toggle in-place rewrite (simulated). 
   * @param EnableAbi Enable structural/ABI-breaking changes. 
   */ 
  explicit TestTypeCorrectAction(bool UseDecltype = false, 
                                 bool ExpandAuto = false, 
                                 std::string Exclude = "", 
                                 bool InPlace = false, 
                                 bool EnableAbi = false) 
      : UseDecltype(UseDecltype), ExpandAuto(ExpandAuto), 
        Exclude(Exclude), InPlace(InPlace), EnableAbi(EnableAbi) {} 

  /** 
   * @brief Factory method for the AST Consumer. 
   * 
   * @param CI Compiler Instance. 
   * @param Param Unused file param. 
   * @return std::unique_ptr<clang::ASTConsumer> The TypeCorrect consumer. 
   */ 
  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef) override { 
    Rewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts()); 
    // Inject test parameters into Consumer with arguments for Audit/Report set to defaults
    return std::make_unique<TypeCorrectASTConsumer>( 
        Rewriter, 
        UseDecltype, 
        ExpandAuto, 
        "", /* ProjectRoot */ 
        Exclude, 
        InPlace, 
        EnableAbi, /* EnableAbiBreakingChanges */ 
        false, /* AuditMode */ 
        type_correct::Phase::Standalone, 
        "", /* FactsOutputDir */ 
        "" /* ReportFile */ 
    ); 
  } 
private: 
  clang::Rewriter Rewriter; 
  bool UseDecltype; 
  bool ExpandAuto; 
  std::string Exclude; 
  bool InPlace; 
  bool EnableAbi; 
}; 

/** 
 * @brief Helper to execute the tool on a string snippet. 
 * 
 * @param Code The C++ source code snippet. 
 * @param UseDecltype Enable decltype preference. 
 * @param ExpandAuto Enable auto expansion. 
 * @param Exclude Constraints on path exclusion (regex). 
 * @param InPlace Simulate reuse. 
 * @param EnableAbi Enable struct field rewriting. 
 * @return std::string The rewritten source code intercepted from stdout. 
 */ 
static std::string runToolOnString(llvm::StringRef Code, bool UseDecltype = false, bool ExpandAuto = false, std::string Exclude = "", bool InPlace = false, bool EnableAbi = false) { 
    testing::internal::CaptureStdout(); 
    runToolOnCode(std::make_unique<TestTypeCorrectAction>(UseDecltype, ExpandAuto, Exclude, InPlace, EnableAbi), Code); 
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
 * @test TemplateArgumentRewriting
 * @brief Verifies rewriting of vector<int> to vector<size_t>. 
 */ 
GTEST_TEST(TypeCorrect, TemplateArgumentRewriting) { 
  static const char *const Code =
      "template<typename T> class vector { public: void push_back(T t) {} };" 
      "typedef unsigned long size_t;" 
      "size_t get_size();" 
      "int main() {" 
      "   vector<int> v;" 
      "   v.push_back(get_size());" 
      "}"; 

  std::string Output = runToolOnString(Code); 
  EXPECT_THAT(Output, ::testing::HasSubstr("vector<size_t> v;")); 
} 

/** 
 * @test MacroRewriting
 * @brief Verifies rewriting of type within a macro definition. 
 */ 
GTEST_TEST(TypeCorrect, MacroRewriting) { 
  // NOTE: In-memory string tests often fail to emulate "File modifiability" logic
  // for macros properly because they don't have a backing file. 
  // However, we test the matching logic here. 
  // The system defaults "IsModifiable" to true for Main File. 
  static const char *const Code =
      "#define INIT_VAR int \n" 
      "typedef unsigned long size_t;" 
      "size_t get_size();" 
      "int main() {" 
      "   INIT_VAR x = get_size();" 
      "}"; 

  std::string Output = runToolOnString(Code); 
  EXPECT_THAT(Output, ::testing::HasSubstr("#define INIT_VAR size_t")); 
} 

/** 
 * @test StructFieldRewritingDisabled
 * @brief Verifies that struct fields are NOT rewritten by default (ABI safety). 
 */ 
GTEST_TEST(TypeCorrect, StructFieldRewritingDisabled) { 
  static const char *const Code =
      "typedef unsigned long size_t;" 
      "size_t get_size();" 
      "struct MyData { int x; };" 
      "void f() {" 
      "   MyData d;" 
      "   d.x = get_size();" 
      "}"; 

  // Default: EnableAbi = false
  std::string Output = runToolOnString(Code, false, false, "", false, /*EnableAbi=*/false); 

  // Should NOT verify change
  EXPECT_THAT(Output, ::testing::HasSubstr("int x;")); 
  EXPECT_THAT(Output, ::testing::Not(::testing::HasSubstr("size_t x;"))); 
} 

/** 
 * @test StructFieldRewritingEnabled
 * @brief Verifies that struct fields ARE rewritten when the CLI flag is enabled. 
 */ 
GTEST_TEST(TypeCorrect, StructFieldRewritingEnabled) { 
  static const char *const Code =
      "typedef unsigned long size_t;" 
      "size_t get_size();" 
      "struct MyData { int x; };" 
      "void f() {" 
      "   MyData d;" 
      "   d.x = get_size();" 
      "}"; 

  // EnableAbi = true
  std::string Output = runToolOnString(Code, false, false, "", false, /*EnableAbi=*/true); 

  // Should verify change
  EXPECT_THAT(Output, ::testing::HasSubstr("size_t x;")); 
}