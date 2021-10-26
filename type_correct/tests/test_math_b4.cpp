#include <gtest/gtest.h>

#include "math_b4.h"

GTEST_TEST(MathTest, b4) {
    EXPECT_EQ(get_bigger_arg(8L, 9L), LAST_BIGGER_ARG_RESPONSE);
    EXPECT_EQ(LAST_BIGGER_ARG_RESPONSE, 9L);
}
