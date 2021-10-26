#include <clang/Tooling/Tooling.h>
#include <llvm/Support/Casting.h>

#include <gtest/gtest.h>

#include <type_correct/TypeCorrectMain.h>

GTEST_TEST(runToolOnCode, TypeCorrect) {
    // runToolOnCode returns whether the action was correctly run over the
    // given code.

    static const char *from = "int a(int b, int c) {return b;} int c = a(5,6);",
                      *want = "int a(int b, int c) {return b;} int c = a(/*b=*/5,/*c=*/6);";
    testing::internal::CaptureStderr();
    testing::internal::CaptureStdout();
    EXPECT_TRUE(clang::tooling::runToolOnCode(std::make_unique<TypeCorrectPluginAction>(), from));
    const std::string output = testing::internal::GetCapturedStderr() + testing::internal::GetCapturedStdout();
    std::cout << "output.ends_with(want): " << std::boolalpha << output.ends_with(want)
              << "\noutput: \"" << output /*output.substr(output.rfind('\n')) */ << "\"\n";
}
