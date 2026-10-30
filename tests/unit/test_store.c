// Recovery after a process died while changing skills (docs/formats.md, section 8).
#include "test.h"

#include <stdlib.h>

#include "sh_collate.h"
#include "sh_fs.h"
#include "sh_json.h"
#include "sh_link.h"
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
    st->lock_path = at("lock.json");
}

static void teardown(sh_store *st) {
    free(st->central);
    free(st->backup);
    free(st->cache);
    free(st->state);
    free(st->lock_path);
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

static bool count_file(const char *rel, const char *full, bool is_link, void *user) {
    (void)rel;
    (void)full;
    (void)is_link;
    ++*(size_t *)user;
    return true;
}

// computedHash and its file order against vectors produced by real npx (tests/vectors/computed-hash.json)
static void folder_hash(void) {
    sh_store st;
    setup(&st);
    yyjson_doc *doc = NULL;
    CHECK(sh_json_read(SKILLHUB_SOURCE_DIR "/tests/vectors/computed-hash.json", 0, &doc) == SH_JSON_OK);
    yyjson_val *cases = sh_json_arr(yyjson_doc_get_root(doc), "cases");
    size_t i, n;
    yyjson_val *c;
    yyjson_arr_foreach(cases, i, n, c) {
        char *dir = sh_strfmt("case-%zu", i);
        size_t j, m;
        yyjson_val *f;
        yyjson_arr_foreach(sh_json_arr(c, "files"), j, m, f) {
            char *rel = sh_strfmt("%s/%s", dir, sh_json_str(f, "path"));
            write_text(rel, sh_json_str(f, "content"));
            free(rel);
        }
        yyjson_arr_foreach(sh_json_arr(c, "skipped"), j, m, f) {
            char *rel = sh_strfmt("%s/%s", dir, yyjson_get_str(f));
            write_text(rel, "skipped\n");
            free(rel);
        }
        sh_strv order = SH_STRV_INIT;  // the order alone, so a mismatch says which paths
        yyjson_arr_foreach(sh_json_arr(c, "files"), j, m, f) sh_strv_push(&order, sh_json_str(f, "path"));
        sh_strv_sort(&order, sh_locale_compare);
        yyjson_arr_foreach(sh_json_arr(c, "order"), j, m, f) {
            if (j < order.len && strcmp(order.items[j], yyjson_get_str(f)) != 0) {
                fprintf(stderr, "case %s: position %zu is %s, npx has %s\n", sh_json_str(c, "name"), j, order.items[j], yyjson_get_str(f));
                ++sh_test_failures;
                break;
            }
        }
        sh_strv_free(&order);
        char *full = at(dir);
        char *hash = sh_folder_hash(full);
        // where the file system folds case, or Unicode forms, some files of a case land on one another; the order above still counts
        size_t on_disk = 0;
        sh_walk_files(full, count_file, &on_disk);
        if (on_disk == yyjson_arr_size(sh_json_arr(c, "files")) + yyjson_arr_size(sh_json_arr(c, "skipped")) &&
            strcmp(hash, sh_json_str(c, "computedHash")) != 0) {
            fprintf(stderr, "case %s: hash %s, npx has %s\n", sh_json_str(c, "name"), hash, sh_json_str(c, "computedHash"));
            ++sh_test_failures;
        }
        free(hash);
        free(full);
        free(dir);
    }
    yyjson_doc_free(doc);
    teardown(&st);
}

static void tree_record(void) {
    sh_store st;
    setup(&st);
    sh_trees *t = sh_trees_load(&st);
    CHECK(strcmp(sh_trees_get(t, "one"), "") == 0);  // nothing on record yet
    sh_trees_free(t);
    const char *names[] = {"one", "two"}, *ids[] = {"aaa", "bbb"};
    sh_trees_put_many(&st, names, ids, 2);
    const char *more[] = {"two"}, *newer[] = {"ccc"};
    sh_trees_put_many(&st, more, newer, 1);
    t = sh_trees_load(&st);
    CHECK(strcmp(sh_trees_get(t, "one"), "aaa") == 0);
    CHECK(strcmp(sh_trees_get(t, "two"), "ccc") == 0);
    sh_trees_free(t);
    write_text("cache/sources.json", "not json");  // derived data: a damaged file is the same as none
    t = sh_trees_load(&st);
    CHECK(strcmp(sh_trees_get(t, "one"), "") == 0);
    sh_trees_free(t);
    sh_trees_put_many(&st, more, newer, 1);
    t = sh_trees_load(&st);
    CHECK(strcmp(sh_trees_get(t, "two"), "ccc") == 0);
    sh_trees_free(t);
    teardown(&st);
}

static void live_links(void) {
    sh_store st;
    setup(&st);
    sh_mkpath(st.central);
    const char *const skill = "---\nname: one\ndescription: mine\n---\n";
    write_text("work/one/SKILL.md", skill);
    write_text("work/other/SKILL.md", skill);
    write_text("work/empty/README.md", "no skill here\n");
    char *one = at("work/one"), *other = at("work/other"), *empty = at("work/empty"), *dest = at("skills/one");
    CHECK(!sh_store_place_link(&st, "one", empty));
    CHECK(!sh_lexists(dest));
    CHECK(sh_store_place_link(&st, "one", one));
    CHECK(sh_is_link(dest) && sh_points_to(dest, one));
    CHECK(sh_store_place_link(&st, "one", other));  // linked again, elsewhere: the first folder stays as it was
    CHECK(sh_is_link(dest) && sh_points_to(dest, other));
    char *kept = at("work/one/SKILL.md");
    CHECK(sh_exists(kept));
    CHECK(sh_store_remove_skill(&st, "one"));
    CHECK(!sh_lexists(dest) && sh_exists(kept));
    write_text("skills/one/SKILL.md", skill);  // a real folder in the way goes to the backups, as with any update
    CHECK(sh_store_place_link(&st, "one", one));
    CHECK(sh_is_link(dest));
    sh_strv backups = SH_STRV_INIT;
    char *dir = at("backup/one");
    sh_list_dir(dir, SH_LIST_ALL, &backups);
    CHECK(backups.len == 1);
    sh_strv_free(&backups);
    free(dir);
    free(kept);
    free(one);
    free(other);
    free(empty);
    free(dest);
    teardown(&st);
}

const sh_test_case sh_tests_store[] = {
    {"store.recover", recover}, {"store.folder_hash", folder_hash}, {"store.tree_record", tree_record}, {"store.live_links", live_links}, {NULL, NULL},
};
