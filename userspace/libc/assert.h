// userspace/libc/assert.h - Diagnostics

#ifndef ASSERT_H
#define ASSERT_H

// __assert_fail prints a diagnostic to stderr then aborts. Implemented in
// assert.c so the macro stays header-light and the message format is shared.
void __assert_fail(const char* expr, const char* file, int line,
                   const char* func) __attribute__((noreturn));

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    ((expr) ? (void)0 \
            : __assert_fail(#expr, __FILE__, __LINE__, __func__))
#endif

// C11 static assertion.
#ifndef static_assert
#define static_assert _Static_assert
#endif

#endif /* ASSERT_H */
