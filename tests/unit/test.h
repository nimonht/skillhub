// A small test harness for the C core: no dependencies, one executable, `ctest` runs it.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sh_test_failures;
extern const char *sh_test_current;

#define CHECK(cond)                                                                                         \
    do {                                                                                                    \
        if (!(cond)) {                                                                                      \
            fprintf(stderr, "%s:%d: %s: CHECK(%s) failed\n", __FILE__, __LINE__, sh_test_current, #cond);  \
            ++sh_test_failures;                                                                             \
        }                                                                                                   \
    } while (0)

// compares two strings and frees `got` (a malloc'd result of the code under test)
#define CHECK_STR(got_expr, want)                                                                          \
    do {                                                                                                    \
        char *got_ = (got_expr);                                                                            \
        const char *want_ = (want);                                                                         \
        if (!got_ || strcmp(got_, want_) != 0) {                                                            \
            fprintf(stderr, "%s:%d: %s: %s\n    got  \"%s\"\n    want \"%s\"\n", __FILE__, __LINE__,        \
                    sh_test_current, #got_expr, got_ ? got_ : "(null)", want_);                             \
            ++sh_test_failures;                                                                             \
        }                                                                                                   \
        free(got_);                                                                                         \
    } while (0)

#define CHECK_EQ_STR(got, want)                                                                            \
    do {                                                                                                    \
        const char *g_ = (got), *w_ = (want);                                                               \
        if (!g_ || strcmp(g_, w_) != 0) {                                                                   \
            fprintf(stderr, "%s:%d: %s: %s\n    got  \"%s\"\n    want \"%s\"\n", __FILE__, __LINE__,        \
                    sh_test_current, #got, g_ ? g_ : "(null)", w_);                                         \
            ++sh_test_failures;                                                                             \
        }                                                                                                   \
    } while (0)

typedef void (*sh_test_fn)(void);
typedef struct {
    const char *name;
    sh_test_fn fn;
} sh_test_case;

// each test file defines one of these lists, ending with {NULL, NULL}
extern const sh_test_case sh_tests_text[];
extern const sh_test_case sh_tests_paths[];
extern const sh_test_case sh_tests_frontmatter[];
extern const sh_test_case sh_tests_lock[];
