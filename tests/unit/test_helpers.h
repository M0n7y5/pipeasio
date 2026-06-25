/*
 * test_helpers.h - minimal assertion macros for pipeasio unit tests.
 *
 * No external dependencies, no allocations.  Each EXPECT_* increments a
 * static failure counter on mismatch and prints file:line - tests keep
 * running so we see ALL failures in one go, not just the first.
 *
 * Usage:
 *   #include "test_helpers.h"
 *   TEST_GROUP("memfd offsets") {
 *       EXPECT_EQ(pipeasio_memfd_size_bytes(0, 1024, 4), 0u);
 *   }
 *   int main(void) { return test_report(); }
 */
#ifndef PIPEASIO_TEST_HELPERS_H
#define PIPEASIO_TEST_HELPERS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static int   test_failures_         = 0;
static int   test_total_            = 0;
static const char *test_current_group_ = "<unnamed>";

#define TEST_GROUP(name) \
    for (int _once = (test_current_group_ = (name), 1); _once; _once = 0)

#define EXPECT_EQ(got, expected) do {                                          \
    test_total_++;                                                             \
    unsigned long long _g = (unsigned long long)(got);                         \
    unsigned long long _e = (unsigned long long)(expected);                    \
    if (_g != _e) {                                                            \
        test_failures_++;                                                      \
        fprintf(stderr,                                                        \
            "  FAIL %s:%d [%s] EXPECT_EQ(%s, %s): got=%llu expected=%llu\n",   \
            __FILE__, __LINE__, test_current_group_, #got, #expected, _g, _e); \
    }                                                                          \
} while (0)

#define EXPECT_TRUE(cond) do {                                                 \
    test_total_++;                                                             \
    if (!(cond)) {                                                             \
        test_failures_++;                                                      \
        fprintf(stderr, "  FAIL %s:%d [%s] EXPECT_TRUE(%s)\n",                  \
                __FILE__, __LINE__, test_current_group_, #cond);               \
    }                                                                          \
} while (0)

static inline int test_report(void)
{
    fprintf(stderr, "[%s] %d checks, %d failed\n",
            test_failures_ ? "FAIL" : "PASS",
            test_total_, test_failures_);
    return test_failures_ ? 1 : 0;
}

#endif /* PIPEASIO_TEST_HELPERS_H */
