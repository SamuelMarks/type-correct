/** 
 * @file TypeSolver.cpp
 * @brief Implementation of the constraint-based solver. 
 * 
 * Implements connected component analysis to propagate type constraints
 * across assignment chains. 
 * 
 * @author SamuelMarks
 * @license CC0
 */ 

#include "TypeSolver.h" 
#include <queue>
#include <algorithm>

namespace type_correct { 

TypeSolver::TypeSolver() {} 

void TypeSolver::AddNode(const clang::NamedDecl *Decl, clang::QualType CurrentType, bool IsFixed) { 
    if (!Decl) return; 
    if (Nodes.find(Decl) == Nodes.end()) { 
        Nodes[Decl] = NodeState(Decl, CurrentType, IsFixed); 
    } else { 
        // If re-adding, merge Fixed status (once fixed, always fixed) 
        if (IsFixed) Nodes[Decl].IsFixed = true; 
    } 
} 

void TypeSolver::AddEdge(const clang::NamedDecl *User, const clang::NamedDecl *Used) { 
    if (!User || !Used || User == Used) return; 
    
    // Ensure nodes exist (with placeholders/unknown types if not seen yet) 
    // We assume AddNode is called during Matcher visitation, but safety check: 
    if (Nodes.find(User) == Nodes.end()) return; // Must exist
    if (Nodes.find(Used) == Nodes.end()) return; // Must exist

    // Undirected graph for Equality constraint. 
    // Even if assignment is A = B, if A changes to size_t, B should ideally be size_t 
    // to avoid casting, and vice versa. 
    Adjacency[User].push_back(Used); 
    Adjacency[Used].push_back(User); 
} 

void TypeSolver::AddConstraint(const clang::NamedDecl *Decl, clang::QualType Candidate, 
                               const clang::Expr *BaseExpr, clang::ASTContext *Ctx) { 
    if (!Decl) return; 
    auto It = Nodes.find(Decl); 
    if (It == Nodes.end()) return; // Should have been added by matcher logic
    
    NodeState &NS = It->second; 
    NS.ConstraintType = GetWider(NS.ConstraintType, Candidate, Ctx); 
    if (BaseExpr) NS.BaseExpr = BaseExpr; // Keep track of latest source eq
} 

clang::QualType TypeSolver::GetWider(clang::QualType A, clang::QualType B, clang::ASTContext *Ctx) { 
    if (A.isNull()) return B; 
    if (B.isNull()) return A; 

    // Same type check
    if (Ctx->hasSameType(A, B)) return A;

    // Incomplete type check
    if (A->isIncompleteType() || B->isIncompleteType()) { 
        return B; 
    } 

    // SegFault Fix: 
    // Synthesized template types (like vector<size_t>) may not have layout info derived 
    // if they were just created in AST but not instantiated semantically.
    // Querying getTypeSize() on them can crash.
    // Logic: If we are comparing non-scalars (classes/templates), we unconditionally 
    // prefer the incoming constraint (B) because we assume the tool suggests a Better type.
    if (!A->isScalarType() || !B->isScalarType()) {
        return B;
    }

    // Now safe to query size on scalars
    uint64_t SizeA = Ctx->getTypeSize(A); 
    uint64_t SizeB = Ctx->getTypeSize(B); 

    if (SizeB > SizeA) return B; 
    if (SizeA > SizeB) return A; 

    // Prefer Unsigned
    if (B->isUnsignedIntegerType() && A->isSignedIntegerType()) return B; 

    return A; 
} 

std::map<const clang::NamedDecl *, NodeState> TypeSolver::Solve(clang::ASTContext *Ctx) { 
    std::map<const clang::NamedDecl *, NodeState> Updates; 
    std::set<const clang::NamedDecl *> Visited; 

    for (auto &Pair : Nodes) { 
        const clang::NamedDecl *StartNode = Pair.first; 
        if (Visited.count(StartNode)) continue; 

        // BFS to find Connected Component
        std::vector<const clang::NamedDecl *> Component; 
        std::queue<const clang::NamedDecl *> Q; 
        Q.push(StartNode); 
        Visited.insert(StartNode); 

        clang::QualType WidestInComponent = Pair.second.OriginalType; 
        const clang::Expr *BestExpr = nullptr; 
        bool ComponentHasFixedNode = false; 
        clang::QualType FixedConstraint; 

        while (!Q.empty()) { 
            const clang::NamedDecl *Curr = Q.front(); 
            Q.pop(); 
            Component.push_back(Curr); 

            NodeState &State = Nodes[Curr]; 

            // 1. Ingest constraints from this node
            clang::QualType PrevWidest = WidestInComponent; 
            WidestInComponent = GetWider(WidestInComponent, State.ConstraintType, Ctx); 
            if (WidestInComponent != PrevWidest) { 
                BestExpr = State.BaseExpr; 
            } 

            // 2. Check Locking
            if (State.IsFixed) { 
                ComponentHasFixedNode = true; 
                // If we have multiple fixed nodes with different types, we take the widest
                // but this signals a potential Conflict unless we insert casts. 
                FixedConstraint = GetWider(FixedConstraint, State.OriginalType, Ctx); 
            } 

            // 3. Traverse Edges
            for (const auto *Neighbor : Adjacency[Curr]) { 
                if (!Visited.count(Neighbor)) { 
                    Visited.insert(Neighbor); 
                    Q.push(Neighbor); 
                } 
            } 
        } 

        // Resolution Logic for Component
        
        // If a Fixed Node forces a type, we might still want to widen the rest 
        // IF the Fixed Node type is compatible or if we accept casts at the boundary. 
        // For this specific tool 'TypeCorrect', the goal is to widen 'int' chains to 'size_t'. 
        
        // Strategy: 
        // If WidestInComponent > FixedConstraint, and we have a Fixed Node: 
        //   - We cannot upgrade the Fixed Node. 
        //   - Upgrading the others causes mismatch at the edge. 
        //   - HOWEVER: The original code likely had mismatch anyway (implicit cast). 
        //   - Solution: Upgrade what we can (Movable nodes). 
        
        for (const auto *Node : Component) { 
            NodeState &State = Nodes[Node]; 
            
            if (State.IsFixed) { 
                // Cannot change. Use Original. 
                continue; 
            } 

            // If not fixed, we upgrade to the component max. 
            if (WidestInComponent != State.OriginalType) { 
                // Ensure we don't downgrade
                if (GetWider(State.OriginalType, WidestInComponent, Ctx) == WidestInComponent) { 
                    State.ConstraintType = WidestInComponent; // Update logic target
                    State.BaseExpr = BestExpr; 
                    Updates[Node] = State; 
                } 
            } 
        } 
    } 

    return Updates; 
} 

} // namespace type_correct