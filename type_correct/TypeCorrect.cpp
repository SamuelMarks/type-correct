/** 
 * @file TypeCorrect.cpp
 * @brief Implementation of TypeCorrect logic including Argument Passing Matchers. 
 * 
 * This implementation includes strategies for identifying implicit casts, 
 * correcting standard library usages (memset, etc.), and ensuring type consistency
 * across initialization, assignments, loops, arithmetic recursion, and IO calls. 
 * 
 * @author Samuel Marks
 * @license CC0
 */ 

#include <optional>

#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendPluginRegistry.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Regex.h>
#include <llvm/Support/raw_ostream.h>

#include "TypeCorrect.h" 

using namespace clang; 
using namespace clang::ast_matchers; 

//----------------------------------------------------------------------------- 
// TypeCorrectMatcher - Helpers
//----------------------------------------------------------------------------- 

TypeCorrectMatcher::TypeCorrectMatcher(clang::Rewriter &Rewriter, 
                                       bool UseDecltype, 
                                       bool ExpandAuto, 
                                       std::string ProjectRoot, 
                                       std::string ExcludePattern, 
                                       bool InPlace) 
    : Rewriter(Rewriter), UseDecltype(UseDecltype), ExpandAuto(ExpandAuto), 
      ProjectRoot(std::move(ProjectRoot)), ExcludePattern(std::move(ExcludePattern)), 
      InPlace(InPlace) {} 

bool TypeCorrectMatcher::IsModifiable(SourceLocation DeclLoc, const SourceManager &SM) { 
    if (DeclLoc.isInvalid()) return false; 
    if (DeclLoc.isMacroID()) return false; 
    
    // 1. If it's the main file, it's always modifiable (by default logic). 
    if (SM.isWrittenInMainFile(DeclLoc)) return true; 

    // 2. If no project root is set, we restrict to main file only. 
    if (ProjectRoot.empty()) return false; 

    // 3. Project-Local Header rewriting logic. 
    FileID FID = SM.getFileID(DeclLoc); 
    auto EntryRef = SM.getFileEntryRefForID(FID); 
    if (!EntryRef) return false; 

    // Use RealPath if available. 
    StringRef FileName = EntryRef->getName(); 
    auto &Entry = EntryRef->getFileEntry(); 
    if (!Entry.tryGetRealPathName().empty()) { 
        FileName = Entry.tryGetRealPathName(); 
    } 

    llvm::SmallString<128> AbsPath(FileName); 
    // Best effort normalization
    SM.getFileManager().makeAbsolutePath(AbsPath); 
    llvm::sys::path::native(AbsPath); 
    
    // Check Exclude Pattern
    if (!ExcludePattern.empty()) { 
        llvm::Regex R(ExcludePattern); 
        if (R.match(AbsPath)) { 
            return false; 
        } 
    } 

    // Simple prefix check: 
    return StringRef(AbsPath).starts_with(ProjectRoot); 
} 

bool TypeCorrectMatcher::IsWiderType(QualType Current, QualType Candidate, ASTContext *Ctx) { 
    if (Current.isNull()) return true; 

    QualType CanCurr = Current.getCanonicalType(); 
    QualType CanCand = Candidate.getCanonicalType(); 

    uint64_t SizeCurr = Ctx->getTypeSize(CanCurr); 
    uint64_t SizeCand = Ctx->getTypeSize(CanCand); 

    // Prefer wider types
    if (SizeCand > SizeCurr) return true; 

    // If same size, prefer Unsigned (e.g. size_t) over Signed (int) 
    if (SizeCand == SizeCurr) { 
        if (CanCand->isUnsignedIntegerType() && CanCurr->isSignedIntegerType()) { 
            return true; 
        } 
    } 
    return false; 
} 

void TypeCorrectMatcher::RegisterUpdate(const NamedDecl* Decl, 
                                        QualType CandidateType, 
                                        const Expr* BaseExpr, 
                                        ASTContext* Ctx) { 
    QualType CurrentBestType; 

    if (WidestTypeMap.find(Decl) != WidestTypeMap.end()) { 
        CurrentBestType = WidestTypeMap[Decl].Type; 
    } else { 
        if (const auto *V = dyn_cast<ValueDecl>(Decl)) { 
            CurrentBestType = V->getType(); 
        } 
    } 

    if (IsWiderType(CurrentBestType, CandidateType, Ctx)) { 
        WidestTypeState State; 
        State.Type = CandidateType; 
        State.BaseExpr = BaseExpr; 
        WidestTypeMap[Decl] = State; 
    } 
} 

QualType TypeCorrectMatcher::GetTypeFromExpression(const Expr *E, ASTContext *Ctx) { 
    if (!E) return {}; 
    
    // Case A: Function Call
    if (const auto *CE = dyn_cast<CallExpr>(E)) { 
        return CE->getCallReturnType(*Ctx); 
    } 

    // Case B: Sizeof / Alignof
    if (const auto *UE = dyn_cast<UnaryExprOrTypeTraitExpr>(E)) { 
        return Ctx->getSizeType(); 
    } 

    // Case C: Ternary
    if (const auto *CO = dyn_cast<ConditionalOperator>(E)) { 
        return CO->getType(); 
    } 

    // Case D: Explicit Cast
    if (const auto *Cast = dyn_cast<ExplicitCastExpr>(E)) { 
        return GetTypeFromExpression(Cast->getSubExpr()->IgnoreParenImpCasts(), Ctx); 
    } 

    // Case E: Variables, Members (References) 
    if (const auto *DR = dyn_cast<DeclRefExpr>(E)) { 
        if (const auto *V = dyn_cast<ValueDecl>(DR->getDecl())) { 
             return V->getType(); 
        } 
    } 
    if (const auto *ME = dyn_cast<MemberExpr>(E)) { 
        if (const auto *V = dyn_cast<ValueDecl>(ME->getMemberDecl())) { 
            return V->getType(); 
        } 
    } 

    // Case F: Arithmetic Propagation
    // Check +, -, * 
    if (const auto *BO = dyn_cast<BinaryOperator>(E)) { 
        auto Op = BO->getOpcode(); 
        if (Op == BO_Add || Op == BO_Sub || Op == BO_Mul) { 
            QualType LHS = GetTypeFromExpression(BO->getLHS()->IgnoreParenImpCasts(), Ctx); 
            QualType RHS = GetTypeFromExpression(BO->getRHS()->IgnoreParenImpCasts(), Ctx); 
            
            // Safety: Only propagate IntegerTypes to avoid pointer confusion
            if (!LHS.isNull() && !RHS.isNull() && LHS->isIntegerType() && RHS->isIntegerType()) { 
                // Return validity check using widest logic components
                uint64_t SizeL = Ctx->getTypeSize(LHS); 
                uint64_t SizeR = Ctx->getTypeSize(RHS); 
                
                if (SizeL > SizeR) return LHS; 
                if (SizeR > SizeL) return RHS; 
                // E.g. unsigned int vs int -> unsigned int
                if (LHS->isUnsignedIntegerType()) return LHS; 
                return RHS; 
            } 
        } 
    } 

    return {}; 
} 

void TypeCorrectMatcher::ScanPrintfArgs(const clang::StringLiteral *FormatStr, 
                                        const std::vector<const clang::Expr*> &Args, 
                                        clang::ASTContext *Ctx) { 
    if (!FormatStr) return; 

    StringRef Data = FormatStr->getString(); 
    unsigned ArgIndex = 0; 
    
    size_t Cursor = 0; 
    size_t Max = Data.size(); 

    while (Cursor < Max) { 
        size_t Percent = Data.find('%', Cursor); 
        if (Percent == StringRef::npos) break; 
        
        Cursor = Percent + 1; 
        if (Cursor >= Max) break; 

        if (Data[Cursor] == '%') { 
            Cursor++; 
            continue; 
        } 

        // Skip Flags
        while (Cursor < Max && StringRef("-+ #0").contains(Data[Cursor])) Cursor++; 

        // Skip Width
        if (Cursor < Max && Data[Cursor] == '*') { 
            Cursor++; 
            ArgIndex++; 
        } else { 
            while (Cursor < Max && isdigit(Data[Cursor])) Cursor++; 
        } 

        // Skip Precision
        if (Cursor < Max && Data[Cursor] == '.') { 
            Cursor++; 
            if (Cursor < Max && Data[Cursor] == '*') { 
                Cursor++; 
                ArgIndex++; 
            } else { 
                while (Cursor < Max && isdigit(Data[Cursor])) Cursor++; 
            } 
        } 

        // Length
        size_t ReplaceStart = Cursor; 
        if (Cursor + 1 < Max && (Data.substr(Cursor, 2) == "ll" || Data.substr(Cursor, 2) == "hh")) { 
             Cursor += 2; 
        } else if (Cursor < Max && StringRef("hljztL").contains(Data[Cursor])) { 
             Cursor++; 
        } 

        // Specifier
        if (Cursor >= Max) break; 
        char Spec = Data[Cursor]; 
        Cursor++; 

        if (StringRef("diuoxXpn").contains(Spec)) { 
            if (ArgIndex < Args.size()) { 
                const Expr *Arg = Args[ArgIndex]->IgnoreParenImpCasts(); 
                
                const VarDecl *V = nullptr; 
                if (const auto *DR = dyn_cast<DeclRefExpr>(Arg)) { 
                    V = dyn_cast<VarDecl>(DR->getDecl()); 
                } else if (const auto *IC = dyn_cast<ImplicitCastExpr>(Arg)) { 
                    if (const auto *DR_sub = dyn_cast<DeclRefExpr>(IC->getSubExpr()->IgnoreParenImpCasts())) { 
                        V = dyn_cast<VarDecl>(DR_sub->getDecl()); 
                    } 
                } 

                if (V) { 
                    SourceLocation Loc = FormatStr->getLocationOfByte(ReplaceStart, 
                                                                      Rewriter.getSourceMgr(), 
                                                                      Ctx->getLangOpts(), 
                                                                      Ctx->getTargetInfo()); 
                    if (Loc.isValid()) { 
                        FormatUsage Usage; 
                        Usage.SpecifierLoc = Loc; 
                        Usage.Length = (Cursor - ReplaceStart); 
                        FormatUsageMap[V].push_back(Usage); 
                    } 
                } 
            } 
        } 
        ArgIndex++; 
    } 
} 

void TypeCorrectMatcher::ScanScanfArgs(const clang::StringLiteral *FormatStr, 
                                       const std::vector<const clang::Expr*> &Args, 
                                       clang::ASTContext *Ctx) { 
    if (!FormatStr) return; 

    StringRef Data = FormatStr->getString(); 
    unsigned ArgIndex = 0; 
    
    size_t Cursor = 0; 
    size_t Max = Data.size(); 

    while (Cursor < Max) { 
        size_t Percent = Data.find('%', Cursor); 
        if (Percent == StringRef::npos) break; 
        
        Cursor = Percent + 1; 
        if (Cursor >= Max) break; 

        if (Data[Cursor] == '%') { 
            Cursor++; 
            continue; 
        } 

        bool Suppress = false; 
        if (Data[Cursor] == '*') { 
            Suppress = true; 
            Cursor++; 
        } 

        // Width
        while (Cursor < Max && isdigit(Data[Cursor])) Cursor++; 

        // Length
        size_t ReplaceStart = Cursor; 
        if (Cursor + 1 < Max && (Data.substr(Cursor, 2) == "ll" || Data.substr(Cursor, 2) == "hh")) { 
             Cursor += 2; 
        } else if (Cursor < Max && StringRef("hljztL").contains(Data[Cursor])) { 
             Cursor++; 
        } 

        // Specifier
        if (Cursor >= Max) break; 
        char Spec = Data[Cursor]; 
        Cursor++; 

        if (Suppress) continue; 

        if (StringRef("diuoxXn").contains(Spec)) { 
            if (ArgIndex < Args.size()) { 
                const Expr *Arg = Args[ArgIndex]->IgnoreParenImpCasts(); 
                
                const VarDecl *V = nullptr; 
                // Scanf arguments are typically &Var (AddressOf) 
                if (const auto *UO = dyn_cast<UnaryOperator>(Arg)) { 
                    if (UO->getOpcode() == UO_AddrOf) { 
                        if (const auto *DR = dyn_cast<DeclRefExpr>(UO->getSubExpr()->IgnoreParenImpCasts())) { 
                            V = dyn_cast<VarDecl>(DR->getDecl()); 
                        } 
                    } 
                } 
                else if (const auto *DR = dyn_cast<DeclRefExpr>(Arg)) { 
                    V = dyn_cast<VarDecl>(DR->getDecl()); 
                } 

                if (V) { 
                    SourceLocation Loc = FormatStr->getLocationOfByte(ReplaceStart, 
                                                                      Rewriter.getSourceMgr(), 
                                                                      Ctx->getLangOpts(), 
                                                                      Ctx->getTargetInfo()); 
                    if (Loc.isValid()) { 
                        FormatUsage Usage; 
                        Usage.SpecifierLoc = Loc; 
                        Usage.Length = (Cursor - ReplaceStart); 
                        FormatUsageMap[V].push_back(Usage); 
                    } 
                } 
            } 
            ArgIndex++; 
        } 
        else if (StringRef("sefgacCSp").contains(Spec) || Spec == '[') { 
            ArgIndex++; 
        } 
    } 
} 

void TypeCorrectMatcher::UpdateFormatSpecifiers(const clang::NamedDecl *Decl, 
                                                const clang::QualType &NewType, 
                                                clang::ASTContext *Ctx) { 
    auto It = FormatUsageMap.find(Decl); 
    if (It == FormatUsageMap.end()) return; 

    QualType Canonical = NewType.getCanonicalType(); 
    std::string NewSpecifier; 

    if (const auto *BT = dyn_cast<BuiltinType>(Canonical)) { 
        switch (BT->getKind()) { 
            case BuiltinType::Bool: NewSpecifier = "d"; break; 
            case BuiltinType::Int: NewSpecifier = "d"; break; 
            case BuiltinType::UInt: NewSpecifier = "u"; break; 
            case BuiltinType::Long: NewSpecifier = "ld"; break; 
            case BuiltinType::ULong: NewSpecifier = "lu"; break; 
            case BuiltinType::LongLong: NewSpecifier = "lld"; break; 
            case BuiltinType::ULongLong: NewSpecifier = "llu"; break; 
            default: break; 
        } 
    } 
    
    if (Ctx->hasSameType(Canonical, Ctx->getSizeType())) { 
        NewSpecifier = "zu"; 
    } else if (Ctx->hasSameType(Canonical, Ctx->getPointerDiffType())) { 
        NewSpecifier = "td"; // C99
    } 

    if (NewSpecifier.empty()) return; 

    for (const auto &Usage : It->second) { 
        if (IsModifiable(Usage.SpecifierLoc, Rewriter.getSourceMgr())) { 
             Rewriter.ReplaceText(Usage.SpecifierLoc, Usage.Length, NewSpecifier); 
        } 
    } 
} 

//----------------------------------------------------------------------------- 
// TypeCorrectMatcher - Main Logic
//----------------------------------------------------------------------------- 

void TypeCorrectMatcher::run(const MatchFinder::MatchResult &Result) { 
  ASTContext *Ctx = Result.Context; 
  SourceManager &SM = Rewriter.getSourceMgr(); 

  //--------------------------------------------------------------------------- 
  // Case 1: Variable Initialization
  //--------------------------------------------------------------------------- 
  if (const auto *Var = Result.Nodes.getNodeAs<VarDecl>("bound_var_decl")) { 
    const auto *InitExpr = Result.Nodes.getNodeAs<Expr>("bound_init_expr"); 

    if (Var && InitExpr) { 
        QualType InitType = GetTypeFromExpression(InitExpr, Ctx); 
        
        if (const auto *Cast = dyn_cast<ExplicitCastExpr>(InitExpr)) { 
            CastsToRemove[Var].push_back(Cast); 
        } 

        if (IsModifiable(Var->getLocation(), SM)) { 
            RegisterUpdate(Var, InitType, nullptr, Ctx); 
        } else { 
             QualType VarType = Var->getType(); 
             if (VarType.getCanonicalType() != InitType.getCanonicalType()) { 
                 InjectCast(InitExpr, VarType, Ctx); 
             } 
        } 
    } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 2: Function Return Type
  //--------------------------------------------------------------------------- 
  else if (const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("bound_func_decl")) { 
    const auto *RetVal = Result.Nodes.getNodeAs<Expr>("bound_ret_val"); 
    if (Func && RetVal) { 
        QualType ActualRetType = RetVal->getType(); 
        if (IsModifiable(Func->getLocation(), SM)) { 
             RegisterUpdate(Func, ActualRetType, nullptr, Ctx); 
        } else { 
             QualType DeclaredRetType = Func->getReturnType(); 
             if (DeclaredRetType.getCanonicalType() != ActualRetType.getCanonicalType()) { 
                 InjectCast(RetVal, DeclaredRetType, Ctx); 
             } 
        } 
    } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 3: For Loop Initialization
  //--------------------------------------------------------------------------- 
  else if (const auto *LoopVar = Result.Nodes.getNodeAs<VarDecl>("bound_loop_var")) { 
      const auto *LoopLimit = Result.Nodes.getNodeAs<Expr>("bound_loop_limit"); 
      const auto *LoopInstance = Result.Nodes.getNodeAs<Expr>("bound_call_instance"); 

      if (LoopVar && LoopLimit) { 
          QualType LimitType = GetTypeFromExpression(LoopLimit, Ctx); 

          if (IsModifiable(LoopVar->getLocation(), SM)) { 
              RegisterUpdate(LoopVar, LimitType, LoopInstance, Ctx); 
          } 
      } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 4: Assignments
  //--------------------------------------------------------------------------- 
  else if (const auto *AssignVar = Result.Nodes.getNodeAs<VarDecl>("bound_assign_var")) { 
      const auto *AssignExpr = Result.Nodes.getNodeAs<Expr>("bound_assign_expr"); 

      if (AssignVar && AssignExpr) { 
          QualType ExprType = GetTypeFromExpression(AssignExpr, Ctx); 
          
          if (const auto *Cast = dyn_cast<ExplicitCastExpr>(AssignExpr)) { 
             CastsToRemove[AssignVar].push_back(Cast); 
          } 

          if (IsModifiable(AssignVar->getLocation(), SM)) { 
              RegisterUpdate(AssignVar, ExprType, nullptr, Ctx); 
          } else { 
              QualType VarType = AssignVar->getType(); 
              if (VarType.getCanonicalType() != ExprType.getCanonicalType()) { 
                   InjectCast(AssignExpr, VarType, Ctx); 
              } 
          } 
      } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 5: Argument Passing
  //--------------------------------------------------------------------------- 
  else if (const auto* ArgVar = Result.Nodes.getNodeAs<VarDecl>("bound_arg_var")) { 
      const auto* ParamDecl = Result.Nodes.getNodeAs<ParmVarDecl>("bound_param_decl"); 
      const auto* ArgExpr = Result.Nodes.getNodeAs<Expr>("bound_arg_expr"); 

      if (ArgVar && ParamDecl && ArgExpr) { 
          QualType ParamType = ParamDecl->getType(); 
          if (IsModifiable(ArgVar->getLocation(), SM)) { 
              RegisterUpdate(ArgVar, ParamType, nullptr, Ctx); 
          } else { 
              QualType VarType = ArgVar->getType(); 
              if (VarType.getCanonicalType() != ParamType.getCanonicalType()) { 
                  InjectCast(ArgExpr, ParamType, Ctx); 
              } 
          } 
      } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 6/7: Pointer Subtraction
  //--------------------------------------------------------------------------- 
  else if (const auto *PtrInitVar = Result.Nodes.getNodeAs<VarDecl>("bound_ptr_diff_var_init")) { 
    const auto *Op = Result.Nodes.getNodeAs<BinaryOperator>("bound_ptr_diff_op_init"); 
    if (PtrInitVar && Op) { 
        QualType DiffType = Ctx->getPointerDiffType(); 
        if (IsModifiable(PtrInitVar->getLocation(), SM)) { 
            RegisterUpdate(PtrInitVar, DiffType, nullptr, Ctx); 
        } else { 
             QualType VarType = PtrInitVar->getType(); 
             if (VarType.getCanonicalType() != DiffType.getCanonicalType()) { 
                 InjectCast(Op, VarType, Ctx); 
             } 
        } 
    } 
  } 
  else if (const auto *PtrAssignVar = Result.Nodes.getNodeAs<VarDecl>("bound_ptr_diff_var_assign")) { 
    const auto *Op = Result.Nodes.getNodeAs<BinaryOperator>("bound_ptr_diff_op_assign"); 
    if (PtrAssignVar && Op) { 
        QualType DiffType = Ctx->getPointerDiffType(); 
        QualType VarType = PtrAssignVar->getType(); 
        if (IsModifiable(PtrAssignVar->getLocation(), SM)) { 
            RegisterUpdate(PtrAssignVar, DiffType, nullptr, Ctx); 
        } else { 
            if (VarType.getCanonicalType() != DiffType.getCanonicalType()) { 
                InjectCast(Op, VarType, Ctx); 
            } 
        } 
    } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 8/9: Negative Literal
  //--------------------------------------------------------------------------- 
  else if (const auto *NegInitVar = Result.Nodes.getNodeAs<VarDecl>("neg_init_var")) { 
      VariablesWithNegativeValues.insert(NegInitVar); 
  } 
  else if (const auto *NegAssignVar = Result.Nodes.getNodeAs<VarDecl>("neg_assign_var")) { 
      VariablesWithNegativeValues.insert(NegAssignVar); 
  } 

  //--------------------------------------------------------------------------- 
  // Case 10: Printf Analysis
  //--------------------------------------------------------------------------- 
  else if (const auto *PrintfCall = Result.Nodes.getNodeAs<CallExpr>("bound_printf_call")) { 
      const FunctionDecl *FD = PrintfCall->getDirectCallee(); 
      if (!FD) return; 

      StringRef Name = FD->getName(); 
      unsigned FormatIndex = 0; 
      if (Name == "printf") FormatIndex = 0; 
      else if (Name == "fprintf") FormatIndex = 1; 
      else if (Name == "sprintf") FormatIndex = 1; 
      else if (Name == "snprintf") FormatIndex = 2; 
      else return; 

      if (FormatIndex < PrintfCall->getNumArgs()) { 
          const Expr *FmtArg = PrintfCall->getArg(FormatIndex)->IgnoreParenImpCasts(); 
          if (const auto *SL = dyn_cast<StringLiteral>(FmtArg)) { 
              std::vector<const Expr*> Args; 
              for (unsigned i = FormatIndex + 1; i < PrintfCall->getNumArgs(); ++i) { 
                  Args.push_back(PrintfCall->getArg(i)); 
              } 
              ScanPrintfArgs(SL, Args, Ctx); 
          } 
      } 
  } 
  
  //--------------------------------------------------------------------------- 
  // Case 11: Scanf Analysis
  //--------------------------------------------------------------------------- 
  else if (const auto *ScanfCall = Result.Nodes.getNodeAs<CallExpr>("bound_scanf_call")) { 
      const FunctionDecl *FD = ScanfCall->getDirectCallee(); 
      if (!FD) return; 
      
      StringRef Name = FD->getName(); 
      unsigned FormatIndex = 0; 
      if (Name == "scanf") FormatIndex = 0; 
      else if (Name == "fscanf") FormatIndex = 1; 
      else if (Name == "sscanf") FormatIndex = 1; 
      else return; 

      if (FormatIndex < ScanfCall->getNumArgs()) { 
          const Expr *FmtArg = ScanfCall->getArg(FormatIndex)->IgnoreParenImpCasts(); 
          if (const auto *SL = dyn_cast<StringLiteral>(FmtArg)) { 
              std::vector<const Expr*> Args; 
              for (unsigned i = FormatIndex + 1; i < ScanfCall->getNumArgs(); ++i) { 
                  Args.push_back(ScanfCall->getArg(i)); 
              } 
              ScanScanfArgs(SL, Args, Ctx); 
          } 
      } 
  } 

  //--------------------------------------------------------------------------- 
  // Case 12: General Relational Comparisons
  //--------------------------------------------------------------------------- 
  else if (const auto *RelOp = Result.Nodes.getNodeAs<BinaryOperator>("bound_relational_op")) { 
      const auto *VarRef = Result.Nodes.getNodeAs<DeclRefExpr>("rel_var_ref"); 
      const auto *SourceExpr = Result.Nodes.getNodeAs<Expr>("rel_source_expr"); 
      
      if (VarRef && SourceExpr) { 
          if (const auto *Var = dyn_cast<VarDecl>(VarRef->getDecl())) { 
              QualType SourceType = GetTypeFromExpression(SourceExpr->IgnoreParenImpCasts(), Ctx); 
              
              if (IsModifiable(Var->getLocation(), SM)) { 
                  RegisterUpdate(Var, SourceType, nullptr, Ctx); 
              } 
          } 
      } 
  } 
} 

void TypeCorrectMatcher::onEndOfTranslationUnit(ASTContext &Ctx) { 
    for (const auto &Pair : WidestTypeMap) { 
        const NamedDecl *Decl = Pair.first; 
        const WidestTypeState &State = Pair.second; 

        bool IsUnsignedCandidate = State.Type->isUnsignedIntegerType(); 
        if (VariablesWithNegativeValues.count(Decl) && IsUnsignedCandidate) { 
            continue; 
        } 

        const VarDecl *V = dyn_cast<VarDecl>(Decl); 
        const FunctionDecl *F = dyn_cast<FunctionDecl>(Decl); 

        if (V) { 
             if (V->getTypeSourceInfo() && V->getType().getCanonicalType() != State.Type.getCanonicalType()) { 
                ResolveType(V->getTypeSourceInfo()->getTypeLoc(), State.Type, &Ctx, V, State.BaseExpr); 

                if (CastsToRemove.count(V)) { 
                    for (const auto *Cast : CastsToRemove[V]) { 
                         RemoveExplicitCast(Cast, &Ctx); 
                    } 
                } 
             } 
             UpdateFormatSpecifiers(V, State.Type, &Ctx); 
        } 
        else if (F && F->getTypeSourceInfo()) { 
             QualType Ret = F->getReturnType(); 
             if (Ret.getCanonicalType() != State.Type.getCanonicalType()) { 
                 TypeLoc TL = F->getTypeSourceInfo()->getTypeLoc(); 
                 if (auto FTL = TL.getAs<FunctionTypeLoc>()) { 
                     ResolveType(FTL.getReturnLoc(), State.Type, &Ctx, nullptr, State.BaseExpr); 
                 } 
             } 
        } 
    } 

    SourceManager &SM = Rewriter.getSourceMgr(); 
    FileID MainFileID = SM.getMainFileID(); 

    if (InPlace && !ProjectRoot.empty()) { 
        Rewriter.overwriteChangedFiles(); 
    } else { 
        if (const llvm::RewriteBuffer *Buffer = Rewriter.getRewriteBufferFor(MainFileID)) { 
            Buffer->write(llvm::outs()); 
        } else { 
            const llvm::MemoryBufferRef MainBuf = SM.getBufferOrFake(MainFileID); 
            llvm::outs() << MainBuf.getBuffer(); 
        } 
        llvm::outs().flush(); 
    } 
} 

void TypeCorrectMatcher::RemoveExplicitCast(const ExplicitCastExpr *CastExpr, ASTContext *Ctx) { 
     if (!CastExpr) return; 
     const Expr *SubExpr = CastExpr->getSubExpr(); 
     if (!SubExpr) return; 

     SourceRange CastRange = CastExpr->getSourceRange(); 
     SourceRange SubExprRange = SubExpr->getSourceRange(); 
     
     if (CastRange.isValid() && SubExprRange.isValid() && IsModifiable(CastRange.getBegin(), Rewriter.getSourceMgr())) { 
         StringRef SubExprText = Lexer::getSourceText(CharSourceRange::getTokenRange(SubExprRange), 
                                                      Rewriter.getSourceMgr(), 
                                                      Ctx->getLangOpts()); 
         if (!SubExprText.empty()) { 
             Rewriter.ReplaceText(CastRange, SubExprText); 
         } 
     } 
} 

void TypeCorrectMatcher::InjectCast(const Expr *ExprToCast, 
                                    const QualType &TargetType, 
                                    ASTContext *Ctx) { 
    if (!ExprToCast) return; 

    clang::PrintingPolicy Policy = Ctx->getPrintingPolicy(); 
    Policy.SuppressScope = false; 
    Policy.FullyQualifiedName = true; 

    std::string TypeName = TargetType.getAsString(Policy); 
    
    std::string CastPrefix = Ctx->getLangOpts().CPlusPlus
                                 ? ("static_cast<" + TypeName + ">(") 
                                 : ("(" + TypeName + ")("); 
    std::string CastSuffix = ")"; 

    SourceRange SR = ExprToCast->getSourceRange(); 
    if (SR.isValid()) { 
         Rewriter.InsertTextBefore(SR.getBegin(), CastPrefix); 
         Rewriter.InsertTextAfterToken(SR.getEnd(), CastSuffix); 
    } 
} 

void TypeCorrectMatcher::HandleMultiDecl(const DeclStmt *DS, ASTContext *Ctx) { 
    if (RewrittenStmts.count(DS)) return; 

    std::string ReplacementBlock; 
    llvm::raw_string_ostream Stream(ReplacementBlock); 

    for (const auto *D : DS->decls()) { 
        const VarDecl *VD = dyn_cast<VarDecl>(D); 
        if (!VD) continue; 

        QualType TargetType = VD->getType(); 
        
        if (WidestTypeMap.find(VD) != WidestTypeMap.end()) { 
            QualType Cand = WidestTypeMap[VD].Type; 
            bool IsUnsignedCandidate = Cand->isUnsignedIntegerType(); 
            
            if (!VariablesWithNegativeValues.count(VD) || !IsUnsignedCandidate) { 
                TargetType = Cand; 
            } 
        } 

        clang::PrintingPolicy Policy = Ctx->getPrintingPolicy(); 
        Policy.SuppressScope = false; 
        Policy.FullyQualifiedName = true; 
        
        Stream << TargetType.getAsString(Policy) << " " << VD->getNameAsString(); 
        
        if (VD->hasInit()) { 
            const Expr *Init = VD->getInit(); 
            SourceRange SR = Init->getSourceRange(); 
            StringRef InitText = Lexer::getSourceText(CharSourceRange::getTokenRange(SR), 
                                                      Rewriter.getSourceMgr(), Ctx->getLangOpts()); 
            if (!InitText.empty()) { 
                Stream << " = " << InitText; 
            } 
        } 
        Stream << "; "; 
    } 
    
    Rewriter.ReplaceText(DS->getSourceRange(), Stream.str()); 
    RewrittenStmts.insert(DS); 
} 

void TypeCorrectMatcher::ResolveType(const TypeLoc &OldLoc, 
                                     const QualType &NewType, 
                                     ASTContext *Ctx, 
                                     const VarDecl *BoundVar, 
                                     const Expr *BaseExpr) { 
  if (BoundVar) { 
      if (!Ctx->getParents(*BoundVar).empty()) { 
          const Stmt *ParentStmt = Ctx->getParents(*BoundVar)[0].get<Stmt>(); 
          if (const auto *DS = dyn_cast_or_null<DeclStmt>(ParentStmt)) { 
              if (!DS->isSingleDecl()) { 
                  bool IsInLoopHeader = false; 
                  auto DSParents = Ctx->getParents(*DS); 
                  if (!DSParents.empty()) { 
                      if (DSParents[0].get<ForStmt>()) IsInLoopHeader = true; 
                  } 

                  if (!IsInLoopHeader) { 
                      HandleMultiDecl(DS, Ctx); 
                      return; 
                  } 
              } 
          } 
      } 
  } 

  Qualifiers Quals = OldLoc.getType().getQualifiers(); 

  TypeLoc TL = OldLoc; 
  while (true) { 
    if (auto QTL = TL.getAs<QualifiedTypeLoc>()) TL = QTL.getUnqualifiedLoc(); 
    else break; 
  } 

  if (auto ATL = TL.getAs<AutoTypeLoc>()) { 
      if (!ExpandAuto && BoundVar && BoundVar->getInit()) { 
          const Expr *Init = BoundVar->getInit()->IgnoreParenImpCasts(); 
          if (isa<CallExpr>(Init) || isa<CXXMemberCallExpr>(Init)) return; 
      } 
  } 

  std::string NewTypeStr; 
  bool DecltypeStrategySuccess = false; 

  if (UseDecltype && BaseExpr) { 
     SourceManager &SM = Rewriter.getSourceMgr(); 
     CharSourceRange Range = CharSourceRange::getTokenRange(BaseExpr->getSourceRange()); 
     StringRef BaseText = Lexer::getSourceText(Range, SM, Ctx->getLangOpts()); 
     if (!BaseText.empty()) { 
         std::string DeclTypeStr = (llvm::Twine("decltype(") + BaseText + ")::size_type").str(); 
         std::string QualPrefix; 
         if (Quals.hasConst()) QualPrefix += "const "; 
         if (Quals.hasVolatile()) QualPrefix += "volatile "; 
         
         NewTypeStr = QualPrefix + DeclTypeStr; 
         DecltypeStrategySuccess = true; 
     } 
  } 

  if (!DecltypeStrategySuccess) { 
      QualType T = NewType.getUnqualifiedType(); 
      QualType FinalType = Ctx->getQualifiedType(T, Quals); 

      bool IsNestedTypedef = false; 
      if (const auto *TT = FinalType->getAs<TypedefType>()) { 
          if (const auto *Decl = TT->getDecl()) { 
              if (isa<RecordDecl>(Decl->getDeclContext())) IsNestedTypedef = true; 
          } 
      } 
      
      clang::PrintingPolicy Policy = Ctx->getPrintingPolicy(); 
      Policy.SuppressScope = false; 
      Policy.FullyQualifiedName = true; 

      NewTypeStr = IsNestedTypedef
                       ? FinalType.getCanonicalType().getAsString(Policy) 
                       : FinalType.getAsString(Policy); 
  } 

  if (TL.getSourceRange().isValid()) { 
    if (IsModifiable(OldLoc.getBeginLoc(), Rewriter.getSourceMgr())) { 
        Rewriter.ReplaceText(OldLoc.getSourceRange(), NewTypeStr); 
    } 
  } 
} 

//----------------------------------------------------------------------------- 
// ASTConsumer & Registration
//----------------------------------------------------------------------------- 

TypeCorrectASTConsumer::TypeCorrectASTConsumer(clang::Rewriter &R, 
                                               bool UseDecltype, 
                                               bool ExpandAuto, 
                                               std::string ProjectRoot, 
                                               std::string ExcludePattern, 
                                               bool InPlace) 
    : Handler(R, UseDecltype, ExpandAuto, std::move(ProjectRoot), std::move(ExcludePattern), InPlace) { 

  // Source Definitions
  // ------------------ 
  // 1. Unary Size/Align
  auto SizeOrAlign = unaryExprOrTypeTraitExpr(anyOf(ofKind(UETT_SizeOf), ofKind(UETT_AlignOf))); 
  auto BaseSource = anyOf(callExpr(), SizeOrAlign); 
  
  // 2. Ternary Recursion (cond ? Source : Source) 
  auto TernarySource = conditionalOperator(
      anyOf(
          hasTrueExpression(ignoringParenImpCasts(BaseSource)), 
          hasFalseExpression(ignoringParenImpCasts(BaseSource))
      )
  ); 
  
  // 3. Recursive Arithmetic Source (Source + Source) 
  // Recursively matches binary operators where at least one operand is a known source. 
  auto ArithmeticSource = binaryOperator( 
      hasAnyOperatorName("+", "-", "*"), 
      hasEitherOperand(ignoringParenImpCasts(BaseSource)) 
  ); 

  auto RawSource = anyOf(BaseSource, TernarySource, ArithmeticSource); 

  // 4. Explicit Casts wrapping Sources
  auto CastSource = explicitCastExpr(has(ignoringParenImpCasts(RawSource))); 
  
  // Final Source Definition
  // Wrap in expr() to ensure it is a BindableMatcher
  auto TypeSource = expr(anyOf(RawSource, CastSource)); 

  // Matcher 1: Variable Init
  auto IsLoopInitVar = hasParent(declStmt(hasParent(forStmt()))); 
  Finder.addMatcher( 
      varDecl( 
          hasInitializer(ignoringParenImpCasts(TypeSource.bind("bound_init_expr"))), 
          unless(parmVarDecl()), unless(IsLoopInitVar)).bind("bound_var_decl"), 
      &Handler); 

  // Matcher 2: Return Type
  Finder.addMatcher( 
      functionDecl(isDefinition(), 
                   hasBody(forEachDescendant( 
                       returnStmt(hasReturnValue(ignoringParenImpCasts(expr().bind("bound_ret_val")))) 
                           .bind("bound_ret_stmt")))) 
          .bind("bound_func_decl"), 
      &Handler); 
      
  // Matcher 3: For Loop Init
  auto MemberCallM = cxxMemberCallExpr(on(ignoringParenImpCasts(expr().bind("bound_call_instance")))); 
  auto FreeCallM = callExpr(unless(cxxMemberCallExpr())); 
  
  auto LoopLimitInner = anyOf(MemberCallM, FreeCallM, SizeOrAlign, TernarySource, CastSource, ArithmeticSource); 
  // Wrap to ensure binding capability
  auto LoopLimitM = expr(LoopLimitInner); 

  Finder.addMatcher(forStmt(hasLoopInit(declStmt(hasSingleDecl(varDecl(hasType(isInteger()), hasInitializer(ignoringParenImpCasts(LoopLimitM.bind("bound_loop_limit")))).bind("bound_loop_var"))))).bind("bound_for_loop-init"), &Handler); 

  // Matcher 4: Assignments
  Finder.addMatcher( 
      binaryOperator(hasOperatorName("="), 
          hasLHS(ignoringParenImpCasts(anyOf(declRefExpr(to(varDecl().bind("bound_assign_var"))), memberExpr(member(fieldDecl().bind("bound_assign_field")))))), 
          hasRHS(ignoringParenImpCasts(TypeSource.bind("bound_assign_expr")))) 
      .bind("bound_assign_op"), 
      &Handler); 

  // Matcher 5: Argument Passing
  Finder.addMatcher( 
      callExpr( 
          forEachArgumentWithParam( 
            expr(ignoringParenImpCasts(declRefExpr(to(varDecl().bind("bound_arg_var"))))) 
                .bind("bound_arg_expr"), 
            parmVarDecl().bind("bound_param_decl") 
          ) 
      ).bind("bound_arg_call"), 
      &Handler); 

  // Matcher 6: Pointer Subtraction Init
  Finder.addMatcher( 
      varDecl( 
          hasInitializer(ignoringParenImpCasts( 
              binaryOperator( 
                  hasOperatorName("-"), 
                  hasLHS(hasType(pointerType())), 
                  hasRHS(hasType(pointerType())) 
              ).bind("bound_ptr_diff_op_init") 
          )) 
      ).bind("bound_ptr_diff_var_init"), 
      &Handler
  ); 

  // Matcher 7: Pointer Subtraction Assign
  Finder.addMatcher( 
      binaryOperator( 
          hasOperatorName("="), 
          hasLHS(ignoringParenImpCasts(declRefExpr(to(varDecl().bind("bound_ptr_diff_var_assign"))))), 
          hasRHS(ignoringParenImpCasts( 
              binaryOperator( 
                  hasOperatorName("-"), 
                  hasLHS(hasType(pointerType())), 
                  hasRHS(hasType(pointerType())) 
              ).bind("bound_ptr_diff_op_assign") 
          )) 
      ).bind("bound_ptr_diff_assign_stmt"), 
      &Handler
  ); 

  // Matcher 8/9: Negative Literal Check
  Finder.addMatcher( 
      varDecl(hasInitializer(ignoringParenImpCasts( 
          unaryOperator(hasOperatorName("-"), 
            hasUnaryOperand(ignoringParenImpCasts(integerLiteral()))) 
      ))).bind("neg_init_var"), 
      &Handler); 

  Finder.addMatcher( 
      binaryOperator(hasOperatorName("="), 
        hasLHS(ignoringParenImpCasts(declRefExpr(to(varDecl().bind("neg_assign_var"))))), 
        hasRHS(ignoringParenImpCasts( 
            unaryOperator(hasOperatorName("-"), 
              hasUnaryOperand(ignoringParenImpCasts(integerLiteral()))) 
        )) 
      ).bind("neg_assign_op"), 
      &Handler); 

  // Matcher 10: Printf
  Finder.addMatcher( 
      callExpr(callee(functionDecl(hasAnyName("printf", "fprintf", "sprintf", "snprintf")))) 
      .bind("bound_printf_call"), 
      &Handler); 

  // Matcher 11: Scanf
  Finder.addMatcher( 
      callExpr(callee(functionDecl(hasAnyName("scanf", "fscanf", "sscanf")))) 
      .bind("bound_scanf_call"), 
      &Handler); 
      
  // Matcher 12: General Relational Comparisons
  auto RelVarM = declRefExpr(to(varDecl().bind("rel_var_ref"))); 
  auto ComparisonOp = anyOf( 
      hasOperatorName("<"), hasOperatorName("<="), 
      hasOperatorName(">"), hasOperatorName(">="), 
      hasOperatorName("=="), hasOperatorName("!=") 
  ); 

  auto RelationalOp = binaryOperator( 
      ComparisonOp, 
      hasEitherOperand(ignoringParenImpCasts(RelVarM)), 
      hasEitherOperand(ignoringParenImpCasts(TypeSource.bind("rel_source_expr"))) 
  ).bind("bound_relational_op"); 

  StatementMatcher ControlFlow = anyOf( 
      ifStmt(hasCondition(ignoringParenImpCasts(RelationalOp))), 
      whileStmt(hasCondition(ignoringParenImpCasts(RelationalOp))), 
      doStmt(hasCondition(ignoringParenImpCasts(RelationalOp))), 
      forStmt(hasCondition(ignoringParenImpCasts(RelationalOp))) 
  ); 

  Finder.addMatcher(ControlFlow, &Handler); 
} 

//----------------------------------------------------------------------------- 
// Plugin Action
//----------------------------------------------------------------------------- 

class TCPluginAction : public clang::PluginASTAction { 
public: 
  bool UseDecltype = false; 
  bool ExpandAuto = false; 
  std::string ProjectRoot = ""; 
  std::string ExcludePattern = ""; 
  bool InPlace = false; 

  bool ParseArgs(const clang::CompilerInstance &CI, const std::vector<std::string> &args) override { 
    for (size_t i = 0; i < args.size(); ++i) { 
        if (args[i] == "-use-decltype") UseDecltype = true; 
        else if (args[i] == "-expand-auto") ExpandAuto = true; 
        else if (args[i] == "-project-root" && i + 1 < args.size()) { 
            ProjectRoot = args[++i]; 
        } 
        else if (args[i] == "-exclude" && i + 1 < args.size()) { 
            ExcludePattern = args[++i]; 
        } 
        else if (args[i] == "-in-place") { 
            InPlace = true; 
        } 
    } 
    return true; 
  } 

  std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef file) override { 
    RewriterForTC.setSourceMgr(CI.getSourceManager(), CI.getLangOpts()); 
    return std::make_unique<TypeCorrectASTConsumer>(RewriterForTC, UseDecltype, ExpandAuto, ProjectRoot, ExcludePattern, InPlace); 
  } 
private: 
  clang::Rewriter RewriterForTC; 
}; 

static clang::FrontendPluginRegistry::Add<TCPluginAction> X("TypeCorrect", "Type Correction Plugin");