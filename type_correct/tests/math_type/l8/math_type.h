#ifndef TYPECORRECT_MATH_H
#define TYPECORRECT_MATH_H

#include "math_type_export.h"

#ifdef __cplusplus
#define MATH_TYPE_NOEXCEPT noexcept
extern "C" {
#else
#define MATH_TYPE_NOEXCEPT
#endif /* __cplusplus */

extern MATH_TYPE_EXPORT long get_bigger_arg(long, long) MATH_TYPE_NOEXCEPT;

extern MATH_TYPE_EXPORT long LAST_BIGGER_ARG_RESPONSE;

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* TYPECORRECT_MATH_H */
