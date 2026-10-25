// Recovery after a process died while changing skills (docs/formats.md, section 8).
#include "test.h"

#include <stdlib.h>

#include "sh_fs.h"
#include "sh_path.h"
#include "sh_store.h"
#include "sh_str.h"

static char *root;

static char *at(const char *rel) { return sh_path_join(root, rel); }

static void write_text(const char *rel, const char *text) {
    char *p = at(rel);
    char *parent = sh_path_parent(p);
    sh_mkpath(parent);
    sh_write_file(p, text, strlen(text));
    free(parent);
    free(p);
}

static void setup(sh_store *st) {
#ifdef _WIN32
    char *tmp = sh_getenv("TEMP");
#else
    char *tmp = sh_getenv("TMPDIR");
#endif
    char *base = sh_path_clean(tmp && *tmp ? tmp : "/tmp");
    free(tmp);
    root = sh_strfmt("%s/skillhub-unit-store-%d", base, rand());
    free(base);
    sh_remove_tree(root);
    sh_mkpath(root);
    memset(st, 0, sizeof *st);
    st->central = at("skills");
    st->backup = at("backup");
    st->cache = at("cache");
    st->state = at("state");
}

static void teardown(sh_store *st) {
    free(st->central);
    free(st->backup);
    free(st->cache);
    free(st->state);
    sh_remove_tree(root);
    free(root);
}

static void recover(void) {
    sh_store st;
    setup(&st);
    // killed after moving the old version aside and before placing the new one
    write_text("backup/one/20261025-101010/SKILL.md", "---\nname: one\ndescription: old\n---\n");
    write_text("cache/stage/one-1-1/SKILL.md", "---\nname: one\ndescription: new\n---\n");
    char *dest = at("skills/one"), *bk = at("backup/one/20261025-101010"), *stage = at("cache/stage/one-1-1");
    char *journal = sh_strfmt("{\"version\":1,\"name\":\"one\",\"dest\":\"%s\",\"backup\":\"%s\",\"stage\":\"%s\"}", dest, bk, stage);
    write_text("state/journal/one-1-1.json", journal);
    // a journal that points outside the folders it may touch is ignored
    char *outside = sh_strfmt("{\"version\":1,\"name\":\"x\",\"dest\":\"%s/elsewhere\",\"backup\":\"%s\",\"stage\":\"\"}", root, bk);
    write_text("state/journal/x-1-2.json", outside);
    write_text("cache/stage/young-2-1/SKILL.md", "in use\n");  // a stage still being written by someone
    CHECK(sh_store_recover(&st, false) == 1);
    char *md = at("skills/one/SKILL.md");
    sh_buf b = SH_BUF_INIT;
    CHECK(sh_read_file(md, 0, &b) && strstr(sh_buf_str(&b), "description: old"));
    sh_buf_free(&b);
    CHECK(!sh_lexists(bk));
    CHECK(!sh_lexists(stage));  // named in the journal: removed
    char *young = at("cache/stage/young-2-1");
    CHECK(sh_exists(young));    // too recent to be a leftover
    char *jdir = at("state/journal");
    sh_strv left = SH_STRV_INIT;
    sh_list_dir(jdir, SH_LIST_ALL, &left);
    CHECK(left.len == 0);
    char *elsewhere = sh_strfmt("%s/elsewhere", root);
    CHECK(!sh_lexists(elsewhere));
    free(elsewhere);
    sh_strv_free(&left);
    free(jdir);
    free(young);
    free(md);
    free(outside);
    free(journal);
    free(dest);
    free(bk);
    free(stage);
    teardown(&st);
}

const sh_test_case sh_tests_store[] = {{"store.recover", recover}, {NULL, NULL}};
