#include "math_l8.h"

long LAST_BIGGER_ARG_RESPONSE = 0L;

long get_bigger_arg(long a, long b) {
    LAST_BIGGER_ARG_RESPONSE = a > b ? a : b;
    return LAST_BIGGER_ARG_RESPONSE;
}
