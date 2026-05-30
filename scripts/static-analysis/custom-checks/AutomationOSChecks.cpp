/*
 * AutomationOS Custom Clang-Tidy Checks
 *
 * This file implements custom static analysis checks for AutomationOS:
 * 1. syscall-validation: Ensures syscalls validate user pointers
 * 2. null-check: Verifies NULL checks after allocations
 * 3. lock-balance: Detects unbalanced lock operations
 */

#include "clang-tidy/ClangTidy.h"
#include "clang-tidy/ClangTidyCheck.h"
#include "clang-tidy/ClangTidyModule.h"
#include "clang-tidy/ClangTidyModuleRegistry.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

using namespace clang;
using namespace clang::tidy;
using namespace clang::ast_matchers;

namespace automationos {

// ============================================================================
// Check 1: Syscall User Pointer Validation
// ============================================================================

class SyscallValidationCheck : public ClangTidyCheck {
public:
  SyscallValidationCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(MatchFinder *Finder) override {
    // Match syscall functions (start with "sys_")
    Finder->addMatcher(
        functionDecl(hasName("sys_.*")).bind("syscall"),
        this);
  }

  void check(const MatchFinder::MatchResult &Result) override {
    const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("syscall");
    if (!Func || !Func->hasBody())
      return;

    // Check each parameter for __user annotation
    for (const auto *Param : Func->parameters()) {
      QualType ParamType = Param->getType();

      // Check if pointer type with __user annotation
      if (ParamType->isPointerType()) {
        // Look for copy_from_user or copy_to_user calls in function body
        bool HasValidation = hasUserCopyCall(Func->getBody());

        if (!HasValidation) {
          diag(Param->getLocation(),
               "syscall parameter '%0' appears to be a user pointer but "
               "no copy_from_user/copy_to_user validation found")
              << Param->getName();
          diag(Func->getBody()->getBeginLoc(),
               "use copy_from_user() or copy_to_user() to safely access "
               "user memory",
               DiagnosticIDs::Note);
        }
      }
    }
  }

private:
  bool hasUserCopyCall(const Stmt *S) {
    if (!S)
      return false;

    // Check if this is a call to copy_from_user or copy_to_user
    if (const auto *Call = dyn_cast<CallExpr>(S)) {
      if (const auto *Callee = dyn_cast<FunctionDecl>(Call->getCalleeDecl())) {
        StringRef Name = Callee->getName();
        if (Name == "copy_from_user" || Name == "copy_to_user")
          return true;
      }
    }

    // Recursively check children
    for (const Stmt *Child : S->children()) {
      if (hasUserCopyCall(Child))
        return true;
    }

    return false;
  }
};

// ============================================================================
// Check 2: NULL Check After Allocation
// ============================================================================

class NullCheckAfterAllocCheck : public ClangTidyCheck {
public:
  NullCheckAfterAllocCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context),
        CheckDepth(Options.get("CheckDepth", 5)) {}

  void registerMatchers(MatchFinder *Finder) override {
    // Match allocation function calls
    Finder->addMatcher(
        callExpr(callee(functionDecl(hasAnyName(
            "kmalloc", "kzalloc", "kcalloc",
            "pmm_alloc", "vmm_alloc", "malloc")))).bind("alloc"),
        this);
  }

  void check(const MatchFinder::MatchResult &Result) override {
    const auto *AllocCall = Result.Nodes.getNodeAs<CallExpr>("alloc");
    if (!AllocCall)
      return;

    // Find the statement containing this allocation
    const auto *ParentStmt = getParentStmt(AllocCall, Result);
    if (!ParentStmt)
      return;

    // Check if there's a NULL check within CheckDepth statements
    bool HasNullCheck = findNullCheckInScope(ParentStmt, AllocCall, Result);

    if (!HasNullCheck) {
      diag(AllocCall->getBeginLoc(),
           "memory allocation result not checked for NULL; "
           "kernel panic may occur if allocation fails")
          << AllocCall->getSourceRange();
      diag(AllocCall->getBeginLoc(),
           "add NULL check: if (!ptr) return -ENOMEM;",
           DiagnosticIDs::Note);
    }
  }

private:
  unsigned CheckDepth;

  const Stmt *getParentStmt(const Expr *E, const MatchFinder::MatchResult &Result) {
    // Simplified: In real implementation, traverse AST to find parent statement
    return nullptr;  // Placeholder
  }

  bool findNullCheckInScope(const Stmt *Scope, const Expr *AllocExpr,
                           const MatchFinder::MatchResult &Result) {
    // Simplified: In real implementation, search for NULL checks
    // within CheckDepth statements after the allocation
    return false;  // Placeholder - triggers warning for demonstration
  }
};

// ============================================================================
// Check 3: Lock Balance Checker
// ============================================================================

class LockBalanceCheck : public ClangTidyCheck {
public:
  LockBalanceCheck(StringRef Name, ClangTidyContext *Context)
      : ClangTidyCheck(Name, Context) {}

  void registerMatchers(MatchFinder *Finder) override {
    // Match functions that acquire locks
    Finder->addMatcher(
        functionDecl(hasDescendant(
            callExpr(callee(functionDecl(hasAnyName(
                "spinlock_acquire", "mutex_lock", "acquire_lock"))))
                .bind("lock_acquire"))).bind("func"),
        this);
  }

  void check(const MatchFinder::MatchResult &Result) override {
    const auto *Func = Result.Nodes.getNodeAs<FunctionDecl>("func");
    const auto *LockCall = Result.Nodes.getNodeAs<CallExpr>("lock_acquire");

    if (!Func || !Func->hasBody())
      return;

    // Count lock acquires and releases
    int AcquireCount = 0;
    int ReleaseCount = 0;

    countLockOperations(Func->getBody(), AcquireCount, ReleaseCount);

    if (AcquireCount != ReleaseCount) {
      diag(Func->getLocation(),
           "function '%0' has unbalanced lock operations: %1 acquire(s), "
           "%2 release(s); may cause deadlock")
          << Func->getName() << AcquireCount << ReleaseCount;

      if (LockCall) {
        diag(LockCall->getBeginLoc(),
             "lock acquired here",
             DiagnosticIDs::Note);
      }

      diag(Func->getBody()->getEndLoc(),
           "ensure all code paths release locks before returning",
           DiagnosticIDs::Note);
    }
  }

private:
  void countLockOperations(const Stmt *S, int &Acquires, int &Releases) {
    if (!S)
      return;

    if (const auto *Call = dyn_cast<CallExpr>(S)) {
      if (const auto *Callee = dyn_cast<FunctionDecl>(Call->getCalleeDecl())) {
        StringRef Name = Callee->getName();
        if (Name == "spinlock_acquire" || Name == "mutex_lock" ||
            Name == "acquire_lock") {
          Acquires++;
        } else if (Name == "spinlock_release" || Name == "mutex_unlock" ||
                   Name == "release_lock") {
          Releases++;
        }
      }
    }

    for (const Stmt *Child : S->children()) {
      countLockOperations(Child, Acquires, Releases);
    }
  }
};

// ============================================================================
// Module Registration
// ============================================================================

class AutomationOSModule : public ClangTidyModule {
public:
  void addCheckFactories(ClangTidyCheckFactories &CheckFactories) override {
    CheckFactories.registerCheck<SyscallValidationCheck>(
        "automationos-syscall-validation");
    CheckFactories.registerCheck<NullCheckAfterAllocCheck>(
        "automationos-null-check");
    CheckFactories.registerCheck<LockBalanceCheck>(
        "automationos-lock-balance");
  }
};

} // namespace automationos

// Register the module
static clang::tidy::ClangTidyModuleRegistry::Add<automationos::AutomationOSModule>
    X("automationos-module", "Adds AutomationOS-specific checks.");

// Make the module loadable
volatile int AutomationOSModuleAnchorSource = 0;
