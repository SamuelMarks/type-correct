#include "math_type.h"

int LAST_BIGGER_ARG_RESPONSE = 0L;

int get_bigger_arg(long a, long b) {
    LAST_BIGGER_ARG_RESPONSE = a > b ? a : b;
    return LAST_BIGGER_ARG_RESPONSE;
}
