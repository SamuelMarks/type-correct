/**
 * @file TypeCorrect.h
 * @brief Header for the TypeCorrect library.
 *
 * This file declares the AST Consumer and Matcher Callback used to identify
 * and correct C/C++ type inconsistencies in variable initializations,
 * for-loops, and function return values.
 *
 * @author Samuel Marks
 * @license CC0
 */

#ifndef TYPE_CORRECT_H
#define TYPE_CORRECT_H

#include <clang/AST/ASTConsumer.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/ASTMatchers/ASTMatchers.h>
#include <clang/Rewrite/Core/Rewriter.h>

#include "type_correct_export.h"

/**
 * @class TypeCorrectMatcher
 * @brief Callback class executed when AST Matchers find a target pattern.
 *
 * This class handles the logic of extracting AST nodes (VarDecls,
 * FunctionDecls, CallExprs) bound by the matchers, determining if a type
 * mismatch exists (e.g., assigning a size_t return to an int), and applying the
 * type fix.
 */
class TYPE_CORRECT_EXPORT TypeCorrectMatcher
    : public clang::ast_matchers::MatchFinder::MatchCallback {
public:
  /**
   * @brief Construct a new Type Correct Matcher object.
   *
   * @param Rewriter Reference to the Clang Rewriter used to modify the source
   * code.
   */
  explicit TypeCorrectMatcher(clang::Rewriter &Rewriter);

  /**
   * @brief Callback executed whenever a registered AST matcher succeeds.
   *
   * This function dispatches to specific handlers based on the bound specific
   * ID (e.g., "var_init_match", "func_return_match").
   *
   * @param Result Container for the AST nodes matching the pattern.
   */
  void
  run(const clang::ast_matchers::MatchFinder::MatchResult &Result) override;

  /**
   * @brief Callback executed at the end of the translation unit.
   *
   * Used to flush changes to stdout or files.
   */
  void onEndOfTranslationUnit() override;

private:
  /**
   * @brief The rewriter engine handling text buffer modifications.
   */
  clang::Rewriter &Rewriter;

  /**
   * @brief Helper to resolve and rewrite a type location.
   *
   * Drills down into specific type locations (skipping const qualifiers)
   * to replace the base type specifier with the new correct type.
   *
   * @param OldLoc The location of the existing type in source (e.g., 'int').
   * @param NewType The qualified type that should be there (e.g., 'size_t').
   * @param Ctx The AST Context for printing policies.
   */
  void ResolveType(const clang::TypeLoc &OldLoc, const clang::QualType &NewType,
                   clang::ASTContext *Ctx);
};

/**
 * @class TypeCorrectASTConsumer
 * @brief Consumes the Abstract Syntax Tree (AST) produced by the Clang parser.
 *
 * Registers the specific AST Matchers for type correction strategies:
 * 1. Variable Initialization Mismatches.
 * 2. Function Return Type Mismatches.
 * 3. Loop Iterator Mismatches.
 */
class TYPE_CORRECT_EXPORT TypeCorrectASTConsumer : public clang::ASTConsumer {
public:
  /**
   * @brief Construct a new Type Correct AST Consumer.
   *
   * Sets up the MatchFinder with the predefined matchers.
   *
   * @param Rewriter Reference to the Clang Rewriter passed to the Matcher
   * callback.
   */
  explicit TypeCorrectASTConsumer(clang::Rewriter &Rewriter);

  /**
   * @brief Entry point for the consumer.
   *
   * Called by the Clang frontend to process the Translation Unit.
   *
   * @param Ctx The AST Context providing source manager and type system access.
   */
  void HandleTranslationUnit(clang::ASTContext &Ctx) override {
    Finder.matchAST(Ctx);
    // Explicitly invoke the end-of-unit handler to write output
    Handler.onEndOfTranslationUnit();
  }

private:
  /**
   * @brief The engine that runs the matchers against the AST.
   */
  clang::ast_matchers::MatchFinder Finder;

  /**
   * @brief The callback handler instance.
   */
  TypeCorrectMatcher Handler;
};

#endif /* TYPE_CORRECT_H */