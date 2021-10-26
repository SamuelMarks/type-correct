#include <gtest/gtest.h>

#include "math_l8.h"

GTEST_TEST(MathTest, l8) {
    EXPECT_EQ(get_bigger_arg(5L, 6L), LAST_BIGGER_ARG_RESPONSE);
    EXPECT_EQ(LAST_BIGGER_ARG_RESPONSE, 6L);
}
