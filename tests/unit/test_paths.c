// Path handling ported from Qt (QDir::cleanPath, relativeFilePath) and the file-system helpers.
#include "test.h"

#include "sh_fs.h"
#include "sh_origin.h"
#include "sh_link.h"
#include "sh_path.h"
#include "sh_str.h"

static void clean(void) {
    CHECK_STR(sh_path_clean(""), "");
    CHECK_STR(sh_path_clean("/"), "/");
    CHECK_STR(sh_path_clean("/a/./b/../c/"), "/a/c");
    CHECK_STR(sh_path_clean("a/.."), ".");
    CHECK_STR(sh_path_clean("../a"), "../a");
    CHECK_STR(sh_path_clean("a//b"), "a/b");
    CHECK_STR(sh_path_clean("/.."), "/");
    CHECK_STR(sh_path_clean("./"), ".");
    CHECK_STR(sh_path_clean("/a/b/../../.."), "/");
    CHECK_STR(sh_path_clean("a/../../b"), "../b");
#ifdef _WIN32
    CHECK_STR(sh_path_clean("C:\\Users\\me\\..\\x"), "C:/Users/x");
    CHECK_STR(sh_path_clean("C:/.."), "C:/");
    CHECK_STR(sh_path_clean("//server/share/a/../b"), "//server/share/b");
#endif
}

static void relative(void) {
    // the link made for Claude Code: tests/smoke.sh checks exactly this text
    CHECK_STR(sh_path_relative("/home/u/.claude/skills", "/home/u/.agents/skills/alpha"), "../../.agents/skills/alpha");
    CHECK_STR(sh_path_relative("/a/b", "/a/b/c/d"), "c/d");
    CHECK_STR(sh_path_relative("/a/b/c", "/a"), "../..");
    CHECK_STR(sh_path_relative("/x", "/y/z"), "../y/z");
}

static void parts(void) {
    CHECK_STR(sh_path_parent("/a/b/c"), "/a/b");
    CHECK_STR(sh_path_parent("/a"), "/");
    CHECK_STR(sh_path_parent("name"), "");
    CHECK_EQ_STR(sh_path_name("/a/b/SKILL.md"), "SKILL.md");
    CHECK(sh_path_within("/home/u/.agents/skills/x", "/home/u/.agents/skills"));
    CHECK(sh_path_within("/home/u/.agents/skills", "/home/u/.agents/skills"));
    CHECK(!sh_path_within("/home/u/.agents/skills-old", "/home/u/.agents/skills"));
    CHECK(sh_path_is_root("/"));
    CHECK(!sh_path_is_root("/home"));
    CHECK_STR(sh_path_join("/a/", "b"), "/a/b");
    CHECK_STR(sh_path_join("/a", "b"), "/a/b");
}

static char *scratch(const char *name) {
    char *tmp = sh_getenv("TMPDIR");
#ifdef _WIN32
    if (!tmp) tmp = sh_getenv("TEMP");
#endif
    char *base = sh_path_clean(tmp && *tmp ? tmp : "/tmp");
    free(tmp);
    char *dir = sh_strfmt("%s/skillhub-unit-%s-%d", base, name, rand());
    free(base);
    sh_remove_tree(dir);
    return dir;
}

static void files(void) {
    char *dir = scratch("files");
    char *a = sh_path_join3(dir, "deep/er", "a.txt");
    CHECK(sh_write_atomic(a, "one", 3));
    CHECK(sh_write_atomic(a, "two", 3));  // replaces, keeps no temporary files behind
    sh_buf b = SH_BUF_INIT;
    CHECK(sh_read_file(a, 0, &b) && strcmp(sh_buf_str(&b), "two") == 0);
    sh_buf_free(&b);
    sh_strv names = SH_STRV_INIT;
    char *deeper = sh_path_join(dir, "deep/er");
    CHECK(sh_list_dir(deeper, SH_LIST_ALL, &names) && names.len == 1);
    sh_strv_free(&names);

    // a link is removed, never what it points to
    char *target = sh_path_join(dir, "target"), *link = sh_path_join(dir, "link"), *keep = sh_path_join(target, "keep.txt");
    sh_mkpath(target);
    CHECK(sh_write_file(keep, "x", 1));
    if (sh_make_dir_link(link, "target")) {
        CHECK(sh_is_link(link));
        CHECK(sh_points_to(link, target));
        char *t = sh_link_target(link);
        CHECK_EQ_STR(t, target);
        free(t);
        CHECK(sh_remove_tree(link));
        CHECK(sh_exists(keep));
    }
    // copy and move keep content and skip links
    char *copy = sh_path_join(dir, "copy");
    CHECK(sh_copy_tree(target, copy));
    char *copied = sh_path_join(copy, "keep.txt");
    CHECK(sh_exists(copied));
    char *moved = sh_path_join(dir, "moved");
    CHECK(sh_move_dir(copy, moved));
    CHECK(!sh_lexists(copy) && sh_is_dir(moved));
    CHECK(sh_remove_tree(dir));
    CHECK(!sh_lexists(dir));
    free(copied); free(moved); free(copy); free(keep); free(link); free(target); free(deeper); free(a); free(dir);
}

// where skills come from: the inputs npx skills accepts (src/source-parser.ts)
static void origins(void) {
    static const struct { const char *in, *type, *source, *url, *ref, *subpath, *host; } cases[] = {
        {"acme/skills", "github", "acme/skills", "https://github.com/acme/skills.git", "", "", "github.com"},
        {"acme/skills.git", "github", "acme/skills", "https://github.com/acme/skills.git", "", "", "github.com"},
        {"acme/skills/tools/lint", "github", "acme/skills", "https://github.com/acme/skills.git", "", "tools/lint", "github.com"},
        {"github:acme/skills#v2", "github", "acme/skills", "https://github.com/acme/skills.git", "v2", "", "github.com"},
        {"https://github.com/acme/skills/tree/dev/skills/a", "github", "acme/skills", "https://github.com/acme/skills.git", "dev", "skills/a", "github.com"},
        {"git@github.com:acme/skills.git", "github", "acme/skills", "git@github.com:acme/skills.git", "", "", "github.com"},
        {"gitlab:group/sub/proj", "gitlab", "https://gitlab.com/group/sub/proj.git", "https://gitlab.com/group/sub/proj.git", "", "", "gitlab.com"},
        {"https://gitlab.com/group/sub/proj", "gitlab", "https://gitlab.com/group/sub/proj.git", "https://gitlab.com/group/sub/proj.git", "", "", "gitlab.com"},
        {"https://code.example.org/team/proj/-/tree/main/skills", "gitlab", "https://code.example.org/team/proj.git", "https://code.example.org/team/proj.git", "main", "skills", "code.example.org"},
        {"https://dev.azure.com/org/proj/_git/repo", "git", "https://dev.azure.com/org/proj/_git/repo", "https://dev.azure.com/org/proj/_git/repo", "", "", "dev.azure.com"},
        {"ssh://git@git.example.org:2222/team/repo.git#v1", "git", "ssh://git@git.example.org:2222/team/repo.git", "ssh://git@git.example.org:2222/team/repo.git", "v1", "", "git.example.org"},
        {"/opt/skills/mine", "local", "/opt/skills/mine", "/opt/skills/mine", "", "", ""},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        sh_origin o;
        char *err = NULL;
        CHECK(sh_origin_parse(cases[i].in, &o, &err));
        if (err) fprintf(stderr, "%s: %s\n", cases[i].in, err);
        free(err);
        CHECK_EQ_STR(sh_origin_type_name(o.type), cases[i].type);
        CHECK_EQ_STR(o.source, cases[i].source);
        CHECK_EQ_STR(o.url, cases[i].url);
        CHECK_EQ_STR(o.ref, cases[i].ref);
        CHECK_EQ_STR(o.subpath, cases[i].subpath);
        CHECK_EQ_STR(o.host, cases[i].host);
        sh_origin_free(&o);
    }
    static const char *const bad[] = {"", "-upload-pack=x", "--", "just-a-word", "a/b c", "://..//22", "https://../x/y.git", "https://./x/y.git", "ftp://example.com/x/y.git",
                                      "file:///etc/x", "ext::sh -c x@y:z", "fd::3", "ssh://-oProxyCommand=x/y", "ssh://-u@example.com/y", "git@-oProxyCommand=x:y"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        sh_origin o;
        char *err = NULL;
        CHECK(!sh_origin_parse(bad[i], &o, &err) && err);
        free(err);
    }
    // cache folders: the older key for plain GitHub, a host folder and a hash for the rest
    sh_origin o;
    CHECK(sh_origin_parse("https://git.Example.com:8443/group/proj/-/tree/dev/sub", &o, NULL));
    CHECK_EQ_STR(o.url, "https://git.Example.com:8443/group/proj.git");  // the port stays, and it is no part of the host
    CHECK_EQ_STR(o.host, "git.example.com");
    CHECK_EQ_STR(o.ref, "dev");
    char *ported = sh_origin_cache_dir(&o, "/c");
    CHECK(sh_starts_with(ported, "/c/repos/git.example.com/group__proj-"));
    free(ported);
    sh_origin_free(&o);
    CHECK(sh_origin_parse("http://gitlab.local/group/proj", &o, NULL));
    CHECK_EQ_STR(o.url, "http://gitlab.local/group/proj.git");  // a server without https keeps working
    sh_origin_free(&o);
    CHECK(sh_origin_parse("acme/skills", &o, NULL));
    CHECK_STR(sh_origin_cache_dir(&o, "/c"), "/c/repos/acme__skills");
    sh_origin_free(&o);
    CHECK(sh_origin_parse("https://GitLab.com/Group/Sub/proj.git/", &o, NULL));
    char *a = sh_origin_cache_dir(&o, "/c");
    sh_origin_free(&o);
    CHECK(sh_origin_parse("gitlab:Group/Sub/proj", &o, NULL));
    char *b = sh_origin_cache_dir(&o, "/c");
    sh_origin_free(&o);
    CHECK(sh_starts_with(a, "/c/repos/gitlab.com/Group__Sub__proj-"));
    CHECK_EQ_STR(a, b);  // the same repository, typed two ways
    free(a);
    free(b);
}

const sh_test_case sh_tests_paths[] = {
    {"paths.clean", clean}, {"paths.relative", relative}, {"paths.parts", parts}, {"paths.files", files}, {"paths.origins", origins}, {NULL, NULL},
};
