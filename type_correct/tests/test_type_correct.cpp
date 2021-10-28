#include <clang/Tooling/Tooling.h>
#include <llvm/Support/Casting.h>

#include <gtest/gtest.h>

#include <type_correct/TypeCorrectMain.h>

GTEST_TEST(runToolOnCode, StringFunctionReturnType) {
    /* Test that var type being assigned to function call is rewritten to match function return type  */
    static const char *const from = "\n#include <string.h>\n"
                                    "int main(void) { const int n = strlen(\"FOO\"); }\n",
                      *const want = "\n#include <string.h>\n"
                                    "int main(void) { const size_t n = strlen(\"FOO\"); }\n";
    EXPECT_TRUE(clang::tooling::runToolOnCode(std::make_unique<TypeCorrectPluginAction>(), from));
}

GTEST_TEST(runToolOnCode, ForLoopComparator) {
    /* Test that for loop iterator type is rewritten to match comparator's type */
    static const char *const from = "\n#include <string.h>\n"
                                    "int main(void) {\n"
                                    "    for(int i=0; i<strlen(\"FOO\"); i++) {}\n"
                                    "}\n",
                      *const want = "\n#include <string.h>\n"
                                    "int main(void) {\n"
                                    "    for(size_t i=0; i<strlen(\"FOO\"); i++) {}\n"
                                    "}\n";
    EXPECT_TRUE(clang::tooling::runToolOnCode(std::make_unique<TypeCorrectPluginAction>(), from));
}

GTEST_TEST(runToolOnCode, FunctionReturnType) {
    /* Test that function return type is rewritten to match type being returned */

    static const char *const from = "\nint f(long b) { return b; }\n",
                      *const want = "\nlong f(long b) { return b; }\n";
    EXPECT_TRUE(clang::tooling::runToolOnCode(std::make_unique<TypeCorrectPluginAction>(), from));
}

GTEST_TEST(runToolOnCode, FunctionReturnAndAssignementType) {
    /* Test that function return type and global assignment type are rewritten to match type being returned */

    static const char *const from = "\nint f(long b) { return b; } static const int c = f(5);\n",
                      *const want = "\nlong f(long b) { return b; } static const long c = f(/*b=*/5);\n";
    testing::internal::CaptureStderr();
    testing::internal::CaptureStdout();
    EXPECT_TRUE(clang::tooling::runToolOnCode(std::make_unique<TypeCorrectPluginAction>(), from));
    const std::string output = testing::internal::GetCapturedStderr() + testing::internal::GetCapturedStdout();
    llvm::outs() << "\noutput: \"" << output /*output.substr(output.rfind('\n')) */
                 << "\"\noutput.ends_with(want): " << (output.ends_with(want) ? "true" : "false");
}
