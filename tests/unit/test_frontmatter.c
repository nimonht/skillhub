// The SKILL.md front matter parser must read files exactly as 1.x did (tests/diff_vs_1_0.py checks the same cases end to end).
#include "test.h"

#include "sh_frontmatter.h"
#include "sh_str.h"

static char *description_of(const char *text) {
    sh_frontmatter fm;
    sh_fm_parse(text, strlen(text), &fm);
    char *d = sh_strdup(sh_fm_get(&fm, "description") ? sh_fm_get(&fm, "description") : "(none)");
    sh_fm_free(&fm);
    return d;
}

static void cases(void) {
    CHECK_STR(description_of("---\nname: a\ndescription: plain\n---\nbody\n"), "plain");
    CHECK_STR(description_of("---\ndescription: |\n  first line\n    indented   more\n  last line\nmetadata:\n  nested: map\n---\n"),
              "first line indented more last line");
    CHECK_STR(description_of("---\ndescription: >-\n  folded\n  text\n---\n"), "folded text");
    CHECK_STR(description_of("---\ndescription: \"a: b, 'c'\"\n---\n"), "a: b, 'c'");
    CHECK_STR(description_of("---\ndescription: 'single'\n---\n"), "single");
    CHECK_STR(description_of("---\r\nname: crlf\r\ndescription: windows line ends\r\n---\r\n"), "windows line ends");
    CHECK_STR(description_of("---\ndescription: never closed\n"), "(none)");
    CHECK_STR(description_of("no front matter at all\n"), "(none)");
    CHECK_STR(description_of("\xEF\xBB\xBF---\ndescription: starts with a byte order mark\n---\n"), "starts with a byte order mark");
    CHECK_STR(description_of("---\ndescription:\xC2\xA0non-breaking spaces around\xC2\xA0\t\n---\n"), "non-breaking spaces around");
    CHECK_STR(description_of("---\ndescription: one\ndescription: two\n---\n"), "two");  // the last one wins
    CHECK_STR(description_of("---\n# description: comment\n  description: indented\ndescription: real\n---\n"), "real");
    CHECK_STR(description_of("---\n:no key\ndescription: x\n---\n"), "x");
    CHECK_STR(description_of("  ---  \ndescription: padded fences\n---\n"), "padded fences");
    CHECK_STR(description_of("---\ndescription: \"\n---\n"), "\"");  // one quote is not a quoted value
}

const sh_test_case sh_tests_frontmatter[] = {{"frontmatter.cases", cases}, {NULL, NULL}};
