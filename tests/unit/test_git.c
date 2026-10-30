// git as the engine uses it (sh_git.c), and the pieces its jobs rely on: lock files, the lookup table for big JSON objects
// and batched lock file writes. Runs git against a throw-away local repository.
#include "test.h"

#include <stdlib.h>

#include "sh_fs.h"
#include "sh_git.h"
#include "sh_json.h"
#include "sh_path.h"
#include "sh_proc.h"
#include "sh_store.h"
#include "sh_str.h"
#include "sh_utf.h"

static char *root;

static char *at(const char *rel) { return sh_path_join(root, rel); }

static void setup(void) {
#ifdef _WIN32
    char *tmp = sh_getenv("TEMP");
#else
    char *tmp = sh_getenv("TMPDIR");
#endif
    char *base = sh_path_clean(tmp && *tmp ? tmp : "/tmp");
    free(tmp);
    root = sh_strfmt("%s/skillhub-unit-git-%d", base, rand());
    free(base);
    sh_remove_tree(root);
    sh_mkpath(root);
}

static void teardown(void) {
    sh_remove_tree(root);
    free(root);
}

// git in the upstream repository, as a throw-away identity that never signs
static sh_git_result up(const char *const *args) {
    const char *argv[32] = {"-c", "user.name=t", "-c", "user.email=t@example.com", "-c", "commit.gpgsign=false",
                            "-c", "init.defaultBranch=main"};
    size_t n = 8;
    for (size_t i = 0; args[i] && n < 31; ++i) argv[n++] = args[i];
    argv[n] = NULL;
    char *dir = at("upstream");
    sh_git_result r = sh_git_run(argv, dir, 0);
    free(dir);
    return r;
}

static void up_ok(const char *const *args) {
    sh_git_result r = up(args);
    if (!r.ok) fprintf(stderr, "git %s failed: %s\n", args[0], r.err ? r.err : "");
    CHECK(r.ok);
    sh_git_result_free(&r);
}

static void write_up(const char *rel, const char *text) {
    char *p = sh_strfmt("%s/upstream/%s", root, rel);
    char *parent = sh_path_parent(p);
    sh_mkpath(parent);
    sh_write_file(p, text, strlen(text));
    free(parent);
    free(p);
}

// skills/a (a script and a nested file), skills/b (with a link), skills/big (5001 files)
static void build_upstream(void) {
    char *dir = at("upstream");
    sh_mkpath(dir);
    free(dir);
    up_ok((const char *[]){"init", "-q", NULL});
    write_up("skills/a/SKILL.md", "---\nname: a\ndescription: first\n---\n");
    write_up("skills/a/ref/notes.md", "notes\n");
    write_up("skills/a/run.sh", "#!/bin/sh\necho hi\n");
    write_up("skills/b/SKILL.md", "---\nname: bee\ndescription: second\n---\n");
    up_ok((const char *[]){"add", "-A", NULL});
    up_ok((const char *[]){"update-index", "--chmod=+x", "skills/a/run.sh", NULL});
    // a link and 5001 files, written straight into the index so no platform needs symlinks or that many files
    sh_git_result blob = up((const char *[]){"hash-object", "-w", "skills/a/ref/notes.md", NULL});
    char *id = sh_trimmed(sh_buf_str(&blob.out), blob.out.len);
    sh_git_result_free(&blob);
    char *link = sh_strfmt("120000,%s,skills/b/link", id);
    up_ok((const char *[]){"update-index", "--add", "--cacheinfo", link, NULL});
    free(link);
    sh_buf info = SH_BUF_INIT;
    for (int i = 0; i <= 5000; ++i) sh_buf_printf(&info, "100644 %s\tskills/big/f%04d.md\n", id, i);
    sh_buf_printf(&info, "100644 %s\tskills/big/SKILL.md\n", id);
    char *dir2 = at("upstream");
    const char *argv[] = {sh_git_program(), "update-index", "--index-info", NULL};
    const sh_proc_opts o = {dir2, NULL, info.data, info.len, 60000, NULL, NULL};
    sh_proc_result pr;
    CHECK(sh_proc_run(argv, &o, &pr));
    sh_proc_result_free(&pr);
    free(dir2);
    sh_buf_free(&info);
    free(id);
}

static void open_repo(sh_repo *r) {
    char *dir = at("cache/repos/up"), *url = at("upstream"), *err = NULL;
    CHECK(sh_repo_open(r, dir, url, "", NULL, &err));
    if (err) fprintf(stderr, "open: %s\n", err);
    free(err);
    free(url);
    free(dir);
}

static char *rev_parse(const char *spec) {
    sh_git_result r = up((const char *[]){"rev-parse", spec, NULL});
    char *out = sh_trimmed(sh_buf_str(&r.out), r.out.len);
    sh_git_result_free(&r);
    return out;
}

static void tree_listing(void) {
    setup();
    build_upstream();
    up_ok((const char *[]){"commit", "-q", "-m", "first", NULL});
    sh_repo r;
    open_repo(&r);
    char *want = rev_parse("HEAD:skills/a");
    CHECK_EQ_STR(sh_repo_id(&r, "skills/a"), want);
    free(want);
    want = rev_parse("HEAD^{tree}");
    CHECK_EQ_STR(sh_repo_id(&r, ""), want);
    free(want);
    CHECK(sh_repo_find(&r, "skills/a/SKILL.md") != NULL);
    CHECK(sh_repo_find(&r, "skills/a/run.sh") && sh_repo_find(&r, "skills/a/run.sh")->mode == SH_MODE_EXEC);
    CHECK(sh_repo_find(&r, "skills/b/link") && sh_repo_find(&r, "skills/b/link")->mode == 0120000u);
    CHECK(sh_repo_find(&r, "skills/zzz") == NULL);
    CHECK_EQ_STR(sh_repo_id(&r, "nope/SKILL.md"), "");
    char *dotgit = at("cache/repos/up/skills");
    CHECK(!sh_exists(dotgit));  // no working tree: the clone is read from its object store
    free(dotgit);
    sh_repo_free(&r);
    teardown();
}

static void extract(void) {
    setup();
    build_upstream();
    up_ok((const char *[]){"commit", "-q", "-m", "first", NULL});
    sh_repo r;
    open_repo(&r);
    char *da = at("out/a"), *db = at("out/b"), *dbig = at("out/big");
    sh_extract jobs[] = {{"skills/a", da, false, NULL}, {"skills/missing", db, false, NULL}, {"skills/b", db, false, NULL},
                         {"skills/big", dbig, false, NULL}};
    sh_repo_extract(&r, jobs, 4);
    CHECK(jobs[0].ok && jobs[2].ok);  // each folder on its own: the one that is too big fails alone
    CHECK(jobs[1].ok);  // a folder the commit does not have: nothing to write, the caller checks the tree id first
    CHECK(!jobs[3].ok && jobs[3].err && strcmp(jobs[3].err, "more than 5000 files") == 0);
    char *p = sh_path_join(da, "ref/notes.md");
    sh_buf b = SH_BUF_INIT;
    CHECK(sh_read_file(p, 0, &b) && strcmp(sh_buf_str(&b), "notes\n") == 0);
    sh_buf_free(&b);
    free(p);
#ifndef _WIN32
    p = sh_path_join(da, "run.sh");
    CHECK(sh_is_executable(p));
    free(p);
#endif
    p = sh_path_join(db, "link");
    CHECK(!sh_lexists(p));  // links are never recreated
    free(p);
    p = sh_path_join(db, "SKILL.md");
    CHECK(sh_exists(p));
    free(p);
    for (size_t i = 0; i < 4; ++i) free(jobs[i].err);
    free(da);
    free(db);
    free(dbig);
    sh_repo_free(&r);
    teardown();
}

typedef struct {
    long long sizes[4];
    int ends;
} sizes_seen;

static void s_begin(void *user, size_t i, long long size) { ((sizes_seen *)user)->sizes[i] = size; }
static void s_data(void *user, size_t i, const char *p, size_t n) { (void)user, (void)i, (void)p, (void)n; }
static void s_end(void *user, size_t i) {
    (void)i;
    ++((sizes_seen *)user)->ends;
}

static void read_objects(void) {
    setup();
    build_upstream();
    up_ok((const char *[]){"commit", "-q", "-m", "first", NULL});
    sh_repo r;
    open_repo(&r);
    const char *ids[] = {sh_repo_id(&r, "skills/a/ref/notes.md"), "0123456789012345678901234567890123456789",
                         sh_repo_id(&r, "skills/a"), sh_repo_id(&r, "skills/a/SKILL.md")};
    sizes_seen seen = {{0, 0, 0, 0}, 0};
    const sh_blob_reader rd = {s_begin, s_data, s_end, &seen};
    CHECK(sh_repo_read(&r, ids, 4, &rd, NULL));
    CHECK(seen.sizes[0] == 6);   // "notes\n"
    CHECK(seen.sizes[1] == -1);  // missing
    CHECK(seen.sizes[2] == -1);  // a folder is not a file
    CHECK(seen.sizes[3] > 0);
    CHECK(seen.ends == 4);
    sh_repo_free(&r);
    teardown();
}

static void fast_path(void) {
    setup();
    build_upstream();
    up_ok((const char *[]){"commit", "-q", "-m", "first", NULL});
    sh_repo r;
    open_repo(&r);
    char *first = sh_strdup(r.head);
    sh_repo_free(&r);
    open_repo(&r);  // nothing changed upstream: same commit, nothing fetched
    CHECK_EQ_STR(r.head, first);
    sh_repo_free(&r);
    write_up("skills/a/SKILL.md", "---\nname: a\ndescription: changed\n---\n");
    up_ok((const char *[]){"commit", "-q", "-am", "second", NULL});
    open_repo(&r);
    char *want = rev_parse("HEAD");
    CHECK_EQ_STR(r.head, want);
    CHECK(strcmp(r.head, first) != 0);
    char *tree = rev_parse("HEAD:skills/a");
    CHECK_EQ_STR(sh_repo_id(&r, "skills/a"), tree);
    free(tree);
    free(want);
    free(first);
    sh_repo_free(&r);
    teardown();
}

static void lock_files(void) {
    setup();
    char *p = at("busy.lock");
    sh_flock *a = sh_flock_take(p, 0);
    CHECK(a != NULL);
    CHECK(sh_flock_take(p, 0) == NULL);   // held: a second holder, even in this process, waits
    CHECK(sh_flock_take(p, 120) == NULL); // ... and gives up after its timeout
    sh_flock_release(a);
    sh_flock *b = sh_flock_take(p, 0);
    CHECK(b != NULL);
    sh_flock_release(b);
    free(p);
    teardown();
}

static void json_index(void) {
    const char *text = "{\"b\":1,\"a\":2,\"a\":3,\"c\":{\"x\":true}}";
    yyjson_doc *d = sh_json_parse(text, strlen(text));
    CHECK(d != NULL);
    yyjson_mut_doc *m = yyjson_doc_mut_copy(d, NULL);
    sh_json_index *ix = sh_json_index_new(yyjson_mut_doc_get_root(m));
    CHECK(yyjson_mut_get_sint(sh_json_index_get(ix, "a")) == 2);  // the first of a repeated key, as yyjson_mut_obj_get
    CHECK(yyjson_mut_get_sint(sh_json_index_get(ix, "b")) == 1);
    CHECK(yyjson_mut_is_obj(sh_json_index_get(ix, "c")));
    CHECK(sh_json_index_get(ix, "") == NULL && sh_json_index_get(ix, "aa") == NULL);
    sh_json_index_free(ix);
    ix = sh_json_index_new(NULL);
    CHECK(sh_json_index_get(ix, "a") == NULL);
    sh_json_index_free(ix);
    yyjson_mut_doc_free(m);
    yyjson_doc_free(d);
}

static void lock_batch(void) {
    setup();
    sh_store st;
    memset(&st, 0, sizeof st);
    st.home = sh_strdup(root);
    st.lock_path = at("lock.json");
    const char *text = "{\"version\":3,\"skills\":{\"keep\":{\"source\":\"x/y\",\"extra\":1}}}";
    sh_write_file(st.lock_path, text, strlen(text));
    sh_source s1 = {sh_strdup("github"), sh_strdup(""), sh_strdup("o/one"), sh_strdup(""), sh_strdup("SKILL.md"), sh_strdup("h1"), sh_strdup("t"), sh_strdup("t")};
    sh_source s2 = {sh_strdup("github"), sh_strdup(""), sh_strdup("o/two"), sh_strdup(""), sh_strdup("SKILL.md"), sh_strdup("h2"), sh_strdup("t"), sh_strdup("t")};
    const char *names[] = {"dup", "new", "dup", "keep"};
    const sh_source *srcs[] = {&s1, &s1, &s2, &s2};
    CHECK(sh_lock_put_many(&st, names, srcs, 4));
    char *err = NULL;
    yyjson_mut_doc *view = sh_lock_load(&st, &err);
    CHECK(view != NULL);
    yyjson_mut_val *skills = yyjson_mut_obj_get(yyjson_mut_doc_get_root(view), "skills");
    CHECK(yyjson_mut_obj_size(skills) == 3);  // a name given twice is one entry ...
    CHECK_EQ_STR(sh_json_mut_str(yyjson_mut_obj_get(skills, "dup"), "skillFolderHash"), "h2");  // ... and the last one counts
    CHECK_EQ_STR(sh_json_mut_str(yyjson_mut_obj_get(skills, "new"), "source"), "o/one");
    CHECK_EQ_STR(sh_json_mut_str(yyjson_mut_obj_get(skills, "keep"), "source"), "o/two");
    CHECK(yyjson_mut_get_sint(yyjson_mut_obj_get(yyjson_mut_obj_get(skills, "keep"), "extra")) == 1);  // other tools' fields stay
    yyjson_mut_doc_free(view);
    const char *gone[] = {"new"};
    const sh_source *none[] = {NULL};
    CHECK(sh_lock_put_many(&st, gone, none, 1));
    view = sh_lock_load(&st, &err);
    CHECK(view && yyjson_mut_obj_get(yyjson_mut_obj_get(yyjson_mut_doc_get_root(view), "skills"), "new") == NULL);
    yyjson_mut_doc_free(view);
    free(err);
    sh_source_free(&s1);
    sh_source_free(&s2);
    free(st.home);
    free(st.lock_path);
    teardown();
}

const sh_test_case sh_tests_git[] = {{"git.tree_listing", tree_listing}, {"git.extract", extract},
                                     {"git.read_objects", read_objects}, {"git.fast_path", fast_path},
                                     {"git.lock_files", lock_files},     {"git.json_index", json_index},
                                     {"git.lock_batch", lock_batch},     {NULL, NULL}};
