#include <clang/Tooling/Tooling.h>
#include <llvm/Support/Casting.h>

#include <gtest/gtest.h>

#include "tests.h" /* nop currently */

#include <type_correct/TypeCorrectMain.h>

GTEST_TEST(runToolOnCode, TypeCorrect) {
    // runToolOnCode returns whether the action was correctly run over the
    // given code.
    EXPECT_TRUE(clang::tooling::runToolOnCode(std::make_unique<TypeCorrectPluginAction>(), "class X {};"));
}
