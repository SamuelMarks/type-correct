#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "../TypeCorrectCAPI.h"
#include <fstream>
#include <llvm/Support/FileSystem.h>

TEST(TypeCorrectCAPI, AuditTest) {
    // Create a dummy file
    llvm::SmallString<128> tempPath;
    llvm::sys::fs::createTemporaryFile("test_audit", "cpp", tempPath);
    
    std::ofstream out(tempPath.c_str());
    out << "int main() { return 0; }\n";
    out.close();

    // Run audit
    int res = type_correct_audit(tempPath.c_str());
    EXPECT_EQ(res, 0);

    // Run audit with null
    res = type_correct_audit(nullptr);
    EXPECT_EQ(res, -1);

    llvm::sys::fs::remove(tempPath);
}

TEST(TypeCorrectCAPI, FixTest) {
    // Create a dummy file
    llvm::SmallString<128> tempPath;
    llvm::sys::fs::createTemporaryFile("test_fix", "cpp", tempPath);
    
    std::ofstream out(tempPath.c_str());
    out << "int main() { return 0; }\n";
    out.close();

    // Run fix dry_run
    int res = type_correct_fix(tempPath.c_str(), true);
    EXPECT_EQ(res, 0);

    // Run fix in_place
    res = type_correct_fix(tempPath.c_str(), false);
    EXPECT_EQ(res, 0);

    // Run fix with null
    res = type_correct_fix(nullptr, false);
    EXPECT_EQ(res, -1);

    llvm::sys::fs::remove(tempPath);
}
