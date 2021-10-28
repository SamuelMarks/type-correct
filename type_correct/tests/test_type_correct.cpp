#include <clang/Tooling/Tooling.h>
#include <llvm/Support/Casting.h>

#include <gtest/gtest.h>

#include <type_correct/TypeCorrectMain.h>

GTEST_TEST(runToolOnCode, FunctionReturnType) {
    // runToolOnCode returns whether the action was correctly run over the
    // given code.

    static const char *const from = "int f(int b) {return b;} static const int c = f(5);",
                      *const want = "int f(int b) {return b;} static const int c = f(/*b=*/5);";
    testing::internal::CaptureStderr();
    testing::internal::CaptureStdout();
    EXPECT_TRUE(clang::tooling::runToolOnCode(std::make_unique<TypeCorrectPluginAction>(), from));
    const std::string output = testing::internal::GetCapturedStderr() + testing::internal::GetCapturedStdout();
    std::cout << "output.ends_with(want): " << std::boolalpha << output.ends_with(want)
              << "\noutput: \"" << output /*output.substr(output.rfind('\n')) */ << "\"\n";
}

GTEST_TEST(runToolOnCode, StringFunctionReturnType) {
    static const char *const from = "\n#include <string.h>\n"
                                    "int main(void) { const int n = strlen(\"FOO\"); }",
                      *const want = "\n#include <string.h>\n"
                                    "int main(void) { const size_t n = strlen(\"FOO\"); }";
    EXPECT_TRUE(clang::tooling::runToolOnCode(std::make_unique<TypeCorrectPluginAction>(), from));
}
