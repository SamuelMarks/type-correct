/** 
 * @file test_type_correct.cpp
 * @brief Integration tests for TypeCorrect. 
 * 
 * Runs the tool on string snippets. 
 * Covers: Rewriting, Back-Tracking, Casting, Widest Type, Template Args, and Macro Expansion.
 * 
 * @author SamuelMarks
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
    // Updated Constructor for ABI/CTU support (Defaulting to false/Standalone for tests)
    return std::make_unique<TypeCorrectASTConsumer>(
        Rewriter, 
        UseDecltype, 
        ExpandAuto, 
        "", /* ProjectRoot */
        Exclude, 
        InPlace, 
        false, /* EnableAbiBreakingChanges */
        type_correct::Phase::Standalone,
        "" /* FactsOutputDir */
    ); 
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