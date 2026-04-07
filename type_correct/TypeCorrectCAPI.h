/**
 * @file TypeCorrectCAPI.h
 * @brief Foreign Function Interface (FFI) for type-correct.
 *
 * Exposes core logic to external tooling (e.g., bridle-ctl) over C ABI.
 */

#ifndef TYPE_CORRECT_CAPI_H
#define TYPE_CORRECT_CAPI_H

#include "type_correct_export.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audits a target file using type-correct without applying changes.
 *
 * @param target_path The absolute or relative path to the C/C++ file to audit.
 * @return 0 on success, non-zero if issues were found or if an error occurred.
 */
TYPE_CORRECT_EXPORT int type_correct_audit(const char *target_path) noexcept;

/**
 * @brief Fixes a target file using type-correct.
 *
 * @param target_path The absolute or relative path to the C/C++ file to fix.
 * @param dry_run If true, outputs the would-be changes instead of writing them
 * to disk.
 * @return 0 on success, non-zero if an error occurred.
 */
TYPE_CORRECT_EXPORT int type_correct_fix(const char *target_path,
                                         bool dry_run) noexcept;

#ifdef __cplusplus
}
#endif

#endif /* TYPE_CORRECT_CAPI_H */
