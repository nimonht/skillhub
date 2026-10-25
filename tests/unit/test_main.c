// Runs every C unit test, or only those whose name contains the first argument.
//   build/skillhub_unit_tests [filter]
#include "test.h"

int sh_test_failures = 0;
const char *sh_test_current = "";

static const sh_test_case *const suites[] = {sh_tests_text, sh_tests_paths, sh_tests_frontmatter, sh_tests_lock, sh_tests_git};

int main(int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : "";
    int ran = 0;
    for (size_t s = 0; s < sizeof suites / sizeof suites[0]; ++s) {
        for (const sh_test_case *t = suites[s]; t->name; ++t) {
            if (*filter && !strstr(t->name, filter)) continue;
            sh_test_current = t->name;
            const int before = sh_test_failures;
            t->fn();
            printf("%s %s\n", sh_test_failures == before ? "ok  " : "FAIL", t->name);
            ++ran;
        }
    }
    printf("%d tests, %d failed checks\n", ran, sh_test_failures);
    return sh_test_failures ? 1 : 0;
}
