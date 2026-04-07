#include "TypeCorrectCAPI.h"
#include "TypeCorrectMain.h"

#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Refactoring.h>
#include <clang/Tooling/Tooling.h>
#include <iostream>
#include <vector>

namespace {
class CAPIActionFactory : public clang::tooling::FrontendActionFactory {
    bool AuditMode;
    bool InPlace;
public:
    CAPIActionFactory(bool AuditMode, bool InPlace) 
        : AuditMode(AuditMode), InPlace(InPlace) {}

    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<TypeCorrectPluginAction>(
            "", "", InPlace, false, AuditMode, 
            type_correct::Phase::Standalone, "", "");
    }
};
} // namespace

extern "C" TYPE_CORRECT_EXPORT int type_correct_audit(const char* target_path) noexcept {
    if (!target_path) return -1;

    std::vector<std::string> CommandLine;
    auto Compilations = std::make_unique<clang::tooling::FixedCompilationDatabase>(".", CommandLine);

    std::vector<std::string> SourcePaths = { target_path };
    clang::tooling::RefactoringTool Tool(*Compilations, SourcePaths);

    CAPIActionFactory Factory(true /* AuditMode */, false /* InPlace */);
    return Tool.run(&Factory);
}

extern "C" TYPE_CORRECT_EXPORT int type_correct_fix(const char* target_path, bool dry_run) noexcept {
    if (!target_path) return -1;

    std::vector<std::string> CommandLine;
    auto Compilations = std::make_unique<clang::tooling::FixedCompilationDatabase>(".", CommandLine);

    std::vector<std::string> SourcePaths = { target_path };
    clang::tooling::RefactoringTool Tool(*Compilations, SourcePaths);

    bool InPlace = !dry_run;
    CAPIActionFactory Factory(false /* AuditMode */, InPlace);

    if (dry_run) {
        return Tool.run(&Factory);
    } else {
        return Tool.runAndSave(&Factory);
    }
}
