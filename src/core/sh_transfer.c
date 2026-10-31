// Export and import of the skill set. One JSON file:
//   { "format": "skillhub-export", "version": 1, "skills": [ { name, description, source, url, path, hash }          (GitHub skill)
//                                                           | { name, description, local: true, files: [ {path, exec, data(base64)} ] } ] }
// A GitHub skill is installed fresh from its repository (latest upstream); a local skill carries its files.
// The file may come from anywhere, so reading is strict: names and paths are validated, sizes are capped, nothing is
// executed, links are never created, and installed skills are never overwritten.
#include <stdlib.h>
#include <string.h>

#include "sh_engine.h"
#include "sh_frontmatter.h"
#include "sh_fs.h"
#include "sh_hash.h"
#include "sh_json.h"
#include "sh_link.h"
#include "sh_origin.h"
#include "sh_path.h"
#include "sh_store.h"
#include "sh_time.h"
#include "sh_utf.h"

#ifndef _WIN32
#include <strings.h>
#endif

#define MAX_FILE (2LL * 1024 * 1024)      // one embedded file
#define MAX_SKILL (12LL * 1024 * 1024)    // one local skill
#define MAX_EXPORT (48LL * 1024 * 1024)   // the whole export file
#define MAX_SKILLS 600
#define MAX_FILES 800

static int name_cmp(const char *a, const char *b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

// A URL an export may carry: https, and no user name or token in it.
static bool plain_https(const char *url) {
    if (!sh_starts_with(url, "https://")) return false;
    const char *host = url + 8;
    const char *at = strchr(host, '@'), *slash = strchr(host, '/');
    return !at || (slash && slash < at);
}

// A skill whose source works on another machine: GitHub, or GitLab or git over https. The others travel as files.
static bool travels(const sh_source *s) {
    if (!sh_source_runnable(s) || strcmp(s->type, "local") == 0) return false;
    if (!*s->type || strcmp(s->type, "github") == 0) return true;
    return plain_https(s->url);
}

// Import files are untrusted: GitHub as a plain owner/repo (the only kind a version 1 file holds), or GitLab and git over
// https. Nothing else, so a hostile file cannot make Skill Hub copy a folder from this machine or use its SSH keys.
// NULL when fine, with *o filled in.
static const char *remote_problem(const char *source, const char *type, sh_origin *o) {
    const bool github = !*type || strcmp(type, "github") == 0;
    const char *const why = github ? "source is not a GitHub repository (owner/repo)" : "only https sources can be imported";
    memset(o, 0, sizeof *o);
    if (!github && strcmp(type, "gitlab") != 0 && strcmp(type, "git") != 0) return why;
    if (!sh_origin_parse(source, o, NULL)) return why;
    const bool plain = strcmp(o->source, source) == 0 && !*o->subpath && !*o->ref;
    if (plain && (github ? o->type == SH_ORIGIN_GITHUB : o->type != SH_ORIGIN_GITHUB && o->type != SH_ORIGIN_LOCAL && plain_https(o->url))) return NULL;
    sh_origin_free(o);
    return why;
}

static bool valid_name(const char *n) {
    const size_t len = strlen(n);
    if (len == 0 || len > 100 || strstr(n, "..")) return false;
    for (size_t i = 0; i < len; ++i) {
        const char c = n[i];
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!(alnum || (i > 0 && (c == '.' || c == '_' || c == '-')))) return false;
    }
    return true;
}

static bool reserved_on_windows(const char *seg) {
    static const char *const names[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                        "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    const char *dot = strchr(seg, '.');
    const size_t base = dot ? (size_t)(dot - seg) : strlen(seg);
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) {
        if (strlen(names[i]) != base) continue;
        bool same = true;
        for (size_t k = 0; k < base && same; ++k) same = (seg[k] & ~0x20) == names[i][k];
        if (same) return true;
    }
    return false;
}

// a relative path made of plain segments that is safe on every system (docs/formats.md, section 9)
static bool valid_rel(const char *p) {
    if (!*p || sh_utf16_len(p) > 300 || p[0] == '/' || strchr(p, '\\') || strchr(p, ':')) return false;
    sh_strv seg = SH_STRV_INIT;
    sh_split(p, strlen(p), '/', false, &seg);
    bool ok = true;
    for (size_t i = 0; ok && i < seg.len; ++i) {
        const char *s = seg.items[i];
        const size_t n = strlen(s);
        if (n == 0 || strcmp(s, ".") == 0 || strcmp(s, "..") == 0 || name_cmp(s, ".git") == 0) ok = false;
        else if (s[n - 1] == '.' || s[n - 1] == ' ' || reserved_on_windows(s)) ok = false;
    }
    sh_strv_free(&seg);
    return ok;
}

// the first value of `key` in a SKILL.md's front matter (the first 16 KB are enough)
static char *first_frontmatter(const sh_buf *md, const char *key) {
    const size_t n = md->len < 16 * 1024 ? md->len : 16 * 1024;
    sh_frontmatter fm;
    sh_fm_parse(sh_buf_str(md), n, &fm);  // same rules as the rest of the core
    const char *v = sh_fm_value(&fm, key);
    char *out = sh_strdup(strcmp(v, ">") == 0 || strcmp(v, ">-") == 0 || strcmp(v, "|") == 0 ? "" : v);
    sh_fm_free(&fm);
    return out;
}

// ---- export ----

typedef struct {
    sh_strv files;
} collected;

static bool collect(const char *rel, const char *full, bool is_link, void *user) {
    (void)full;
    (void)is_link;
    sh_strv_push(&((collected *)user)->files, rel);
    return true;
}

static int cmp_str(const char *a, const char *b) { return strcmp(a, b); }

bool sh_store_write_export(sh_store *st, const char *path, bool include_local, int *tracked, int *local, sh_strv *skipped,
                           char **err) {
    int n_tracked = 0, n_local = 0;
    yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(d);
    yyjson_mut_doc_set_root(d, root);
    yyjson_mut_val *arr = yyjson_mut_arr(d);
    sh_skills skills;
    sh_store_scan(st, &skills);
    for (size_t i = 0; i < skills.len; ++i) {
        const sh_skill *s = &skills.items[i];
        yyjson_mut_val *o = yyjson_mut_obj(d);
        sh_json_set_str(d, o, "name", s->name);
        sh_json_set_str(d, o, "description", s->description);
        if (travels(&s->src)) {
            sh_json_set_str(d, o, "sourceType", *s->src.type ? s->src.type : "github");
            sh_json_set_str(d, o, "source", s->src.source);
            char *url = *s->src.url ? sh_strdup(s->src.url) : sh_strfmt("https://github.com/%s.git", s->src.source);
            sh_json_set_str(d, o, "url", url);
            free(url);
            sh_json_set_str(d, o, "path", s->src.skill_path);
            if (*s->src.ref) sh_json_set_str(d, o, "ref", s->src.ref);
            sh_json_set_str(d, o, "hash", s->src.hash);
            ++n_tracked;
        } else if (include_local) {
            collected c = {SH_STRV_INIT};
            sh_walk_files(s->path, collect, &c);
            sh_strv_sort(&c.files, cmp_str);
            yyjson_mut_val *files = yyjson_mut_arr(d);
            long long total = 0;
            int count = 0;
            bool too_big = false;
            for (size_t f = 0; f < c.files.len && !too_big; ++f) {
                const char *rel = c.files.items[f];
                if (sh_starts_with(rel, ".git/") || strstr(rel, "__pycache__/") || strstr(rel, "node_modules/")) continue;
                char *full = sh_path_join(s->path, rel);
                if (sh_is_link(full)) {
                    if (skipped) sh_strv_take(skipped, sh_strfmt("%s/%s (symlink)", s->name, rel));
                    free(full);
                    continue;
                }
                // an import refuses names that some system cannot store, so the export leaves them out and says so
                bool collides = false;
                for (size_t k = 0; k < f; ++k)
                    if (name_cmp(c.files.items[k], rel) == 0) collides = true;
                if (!valid_rel(rel) || collides) {
                    if (skipped) sh_strv_take(skipped, sh_strfmt("%s/%s (a file name other systems cannot use)", s->name, rel));
                    free(full);
                    continue;
                }
                const long long size = sh_file_size(full);
                sh_buf data = SH_BUF_INIT;
                if (size > MAX_FILE || total + size > MAX_SKILL || ++count > MAX_FILES || !sh_read_file(full, 0, &data)) {
                    too_big = true;
                } else {
                    total += (long long)data.len;
                    yyjson_mut_val *fo = yyjson_mut_obj(d);
                    sh_json_set_str(d, fo, "path", rel);
#ifdef _WIN32
                    const bool exec = data.len >= 2 && data.data[0] == '#' && data.data[1] == '!';  // Windows has no exec bit: scripts keep theirs
#else
                    const bool exec = sh_is_executable(full);
#endif
                    sh_json_set_bool(d, fo, "exec", exec);
                    sh_buf b64 = SH_BUF_INIT;
                    sh_base64_encode(data.data ? data.data : "", data.len, &b64);
                    sh_json_set_str(d, fo, "data", sh_buf_str(&b64));
                    sh_buf_free(&b64);
                    yyjson_mut_arr_append(files, fo);
                }
                sh_buf_free(&data);
                free(full);
            }
            sh_strv_free(&c.files);
            if (too_big) {
                if (skipped) sh_strv_take(skipped, sh_strfmt("%s (too large to embed)", s->name));
                continue;
            }
            sh_json_set_bool(d, o, "local", true);
            sh_json_set_val(d, o, "files", files);
            ++n_local;
        } else {
            continue;
        }
        yyjson_mut_arr_append(arr, o);
    }
    sh_skills_free(&skills);
    char *created = sh_now_iso();
    created[19] = 'Z';  // seconds precision
    created[20] = '\0';
    sh_json_set_str(d, root, "format", "skillhub-export");
    sh_json_set_int(d, root, "version", 2);
    sh_json_set_str(d, root, "createdAt", created);
    sh_json_set_str(d, root, "createdBy", "skillhub " SKILLHUB_VERSION);
    sh_json_set_val(d, root, "skills", arr);
    free(created);
    char *parent = sh_path_parent(path);
    if (*parent) sh_mkpath(parent);
    free(parent);
    const bool ok = sh_json_write(path, d, true, true);
    yyjson_mut_doc_free(d);
    if (!ok) {
        if (err) *err = sh_strfmt("cannot write %s", path);
        return false;
    }
    if (tracked) *tracked = n_tracked;
    if (local) *local = n_local;
    return true;
}

// ---- import ----

static void item_free(sh_import_item *it) {
    free(it->name); free(it->description); free(it->source); free(it->url); free(it->skill_path); free(it->ref); free(it->problem);
    for (size_t i = 0; i < it->n_files; ++i) {
        free(it->files[i].path);
        sh_buf_free(&it->files[i].data);
    }
    free(it->files);
}

void sh_import_items_free(sh_import_items *items) {
    for (size_t i = 0; i < items->len; ++i) item_free(&items->items[i]);
    free(items->items);
    items->items = NULL;
    items->len = 0;
}

static void set(char **field, char *value) {
    free(*field);
    *field = value;
}

void sh_store_read_export(const sh_store *st, const char *path, sh_import_items *out, char **err) {
    memset(out, 0, sizeof *out);
    const long long size = sh_file_size(path);
    if (size < 0 || !sh_is_file(path)) {
        if (err) *err = sh_strfmt("cannot open %s", path);
        return;
    }
    if (size > MAX_EXPORT) {
        if (err) *err = sh_strfmt("that file is larger than %lld MB, which no export of Skill Hub is", MAX_EXPORT / (1024 * 1024));
        return;
    }
    sh_buf raw = SH_BUF_INIT;
    if (!sh_read_file(path, 0, &raw)) {
        if (err) *err = sh_strfmt("cannot open %s", path);
        sh_buf_free(&raw);
        return;
    }
    sh_store_read_export_data(st, sh_buf_str(&raw), raw.len, out, err);
    sh_buf_free(&raw);
}

void sh_store_read_export_data(const sh_store *st, const char *data, size_t n, sh_import_items *out, char **err) {
    memset(out, 0, sizeof *out);
    if ((long long)n > MAX_EXPORT) {
        if (err) *err = sh_strfmt("that file is larger than %lld MB, which no export of Skill Hub is", MAX_EXPORT / (1024 * 1024));
        return;
    }
    yyjson_doc *doc = sh_json_parse(data, n);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    if (!yyjson_is_obj(root) || strcmp(sh_json_str(root, "format"), "skillhub-export") != 0) {
        if (err) *err = sh_strdup("this is not a Skill Hub export file");
        yyjson_doc_free(doc);
        return;
    }
    const long long version = sh_json_int(root, "version", 0);
    if (version != 1 && version != 2) {
        if (err) *err = sh_strfmt("this export was made by a newer Skill Hub (format version %lld); update the app", version);
        yyjson_doc_free(doc);
        return;
    }
    yyjson_val *arr = sh_json_arr(root, "skills");
    const size_t count = yyjson_arr_size(arr);
    if (count > MAX_SKILLS) {
        if (err) *err = sh_strfmt("too many skills in one file (%zu)", count);
        yyjson_doc_free(doc);
        return;
    }
    out->items = sh_xcalloc(count + 1, sizeof *out->items);
    sh_strv seen = SH_STRV_INIT;
    size_t idx, max;
    yyjson_val *o;
    yyjson_arr_foreach(arr, idx, max, o) {
        sh_import_item *it = &out->items[out->len++];
        memset(it, 0, sizeof *it);
        it->name = sh_strdup(sh_json_str(o, "name"));
        it->description = sh_left_utf16(sh_json_str(o, "description"), 600);
        it->local = sh_json_bool(o, "local");
        it->source = sh_strdup("");
        it->url = sh_strdup("");
        it->skill_path = sh_strdup("");
        it->ref = sh_strdup("");
        it->problem = sh_strdup("");
        if (!valid_name(it->name)) {
            set(&it->problem, sh_strdup("not a valid skill name"));
        } else if (sh_strv_contains(&seen, it->name)) {
            set(&it->problem, sh_strdup("listed twice"));
        } else if (!it->local) {
            sh_origin origin;
            const char *p = sh_json_str(o, "path");
            const char *ref = sh_json_str(o, "ref");
            const char *why = remote_problem(sh_json_str(o, "source"), sh_json_str(o, "sourceType"), &origin);
            if (!why && !sh_origin_ref_ok(ref)) why = "unsafe branch or tag";
            if (!why && (!valid_rel(p) || !(strcmp(p, "SKILL.md") == 0 || sh_ends_with(p, "/SKILL.md")))) why = "unsafe path inside the repository";
            if (why) {
                set(&it->problem, sh_strdup(why));
            } else {
                set(&it->source, sh_strdup(origin.source));
                set(&it->url, sh_strdup(origin.url));  // always rebuilt from the validated source; the file's own URL is ignored
                set(&it->skill_path, sh_strdup(p));
                set(&it->ref, sh_strdup(ref));
            }
            if (origin.source) sh_origin_free(&origin);
        } else {
            yyjson_val *files = sh_json_arr(o, "files");
            const size_t nf = yyjson_arr_size(files);
            long long total = 0;
            bool has_skill_md = false;
            if (nf == 0 || nf > MAX_FILES) set(&it->problem, sh_strdup("no files, or too many"));
            it->files = sh_xcalloc(nf + 1, sizeof *it->files);
            size_t fi, fmax;
            yyjson_val *fv;
            yyjson_arr_foreach(files, fi, fmax, fv) {
                if (*it->problem) break;
                const char *fpath = sh_json_str(fv, "path");
                if (!valid_rel(fpath)) {
                    char *shown = sh_left_utf16(fpath, 60);
                    set(&it->problem, sh_strfmt("unsafe file path \"%s\"", shown));
                    free(shown);
                    break;
                }
                for (size_t k = 0; k < it->n_files; ++k) {  // two names that one case-insensitive disk would merge
                    if (name_cmp(it->files[k].path, fpath) == 0) {
                        char *shown = sh_left_utf16(fpath, 60);
                        set(&it->problem, sh_strfmt("unsafe file path \"%s\"", shown));
                        free(shown);
                    }
                }
                if (*it->problem) break;
                sh_import_file *file = &it->files[it->n_files];
                memset(file, 0, sizeof *file);
                const char *b64 = sh_json_str(fv, "data");
                sh_base64_decode_lenient(b64, strlen(b64), &file->data);
                if ((long long)file->data.len > MAX_FILE) {
                    set(&it->problem, sh_strfmt("a file is larger than %lld MB", MAX_FILE / (1024 * 1024)));
                    sh_buf_free(&file->data);
                    break;
                }
                total += (long long)file->data.len;
                if (total > MAX_SKILL) {
                    set(&it->problem, sh_strfmt("larger than %lld MB", MAX_SKILL / (1024 * 1024)));
                    sh_buf_free(&file->data);
                    break;
                }
                file->path = sh_strdup(fpath);
                file->exec = sh_json_bool(fv, "exec");
                if (strcmp(fpath, "SKILL.md") == 0) has_skill_md = true;
                ++it->n_files;
            }
            if (!*it->problem && !has_skill_md) set(&it->problem, sh_strdup("no SKILL.md at the top level"));
            it->bytes = total;
            if (!*it->problem && !*it->description) {
                for (size_t k = 0; k < it->n_files; ++k)
                    if (strcmp(it->files[k].path, "SKILL.md") == 0) set(&it->description, first_frontmatter(&it->files[k].data, "description"));
            }
        }
        if (!*it->problem) sh_strv_push(&seen, it->name);
        if (valid_name(it->name)) {
            char *dest = sh_store_skill_dir(st, it->name);
            it->exists = sh_exists(dest);
            free(dest);
        } else if (*it->problem) {
            set(&it->name, sh_strdup("(invalid name)"));
        }
    }
    sh_strv_free(&seen);
    yyjson_doc_free(doc);
}

static bool fail(sh_store *st, const sh_import_item *it, char *msg) {
    sh_store_log(st, "FAILED to import %s: %s", it->name, msg);
    free(msg);
    return false;
}

// a skill embedded in the file: written into a staging folder first, then moved into place
static bool import_local(sh_store *st, const sh_import_item *it) {
    char *label = sh_strfmt("import-%s", it->name);
    char *stage = sh_store_stage_dir(st, label);
    free(label);
    sh_remove_tree(stage);
    for (size_t i = 0; i < it->n_files; ++i) {
        const sh_import_file *f = &it->files[i];
        char *to = sh_path_join(stage, f->path);
        char *parent = sh_path_parent(to);
        sh_mkpath(parent);
        free(parent);
        const bool written = sh_write_file(to, f->data.data ? f->data.data : "", f->data.len);
        if (written) sh_set_executable(to, f->exec);
        free(to);
        if (!written) {
            sh_remove_tree(stage);
            free(stage);
            return fail(st, it, sh_strfmt("cannot write %s", f->path));
        }
    }
    char *dest = sh_path_join(st->central, it->name);
    sh_mkpath(st->central);
    const bool placed = sh_move_dir(stage, dest);  // a rename, or a copy when the cache is on another disk
    if (!placed) sh_remove_tree(stage);
    free(stage);
    free(dest);
    if (!placed) return fail(st, it, sh_strdup("cannot place the skill in the shared folder"));
    sh_store_log(st, "imported local skill %s (%zu files)", it->name, it->n_files);
    return true;
}

// a remote skill that may be installed: a source that passes the same checks as in the file, and a sane path inside it
static bool remote_ok(const sh_import_item *it) {
    sh_origin o;
    const char *why = remote_problem(it->source, sh_starts_with(it->source, "https://") ? "git" : "github", &o);
    if (!why) sh_origin_free(&o);
    return !why && valid_rel(it->skill_path);
}

void sh_store_import_items(sh_store *st, const sh_import_item *const *items, size_t n, int *ok, int *failed) {
    *ok = *failed = 0;
    sh_strv taken = SH_STRV_INIT;  // names installed earlier in this run count as installed
    const sh_import_item **remote = sh_xcalloc(n + 1, sizeof *remote);
    size_t n_remote = 0;
    for (size_t i = 0; i < n; ++i) {
        const sh_import_item *it = items[i];
        char *dest = sh_store_skill_dir(st, it->name);
        bool go;
        if (*it->problem || !valid_name(it->name)) go = fail(st, it, sh_strdup(*it->problem ? it->problem : "invalid skill"));
        else if (sh_exists(dest) || sh_strv_contains(&taken, it->name)) go = fail(st, it, sh_strdup("already installed (not overwritten)"));
        else if (!it->local && !remote_ok(it)) go = fail(st, it, sh_strdup("unsafe source"));
        else go = true;
        free(dest);
        if (!go) {
            ++*failed;
            continue;
        }
        sh_strv_push(&taken, it->name);
        if (!it->local) remote[n_remote++] = it;
        else if (import_local(st, it)) ++*ok;
        else ++*failed;
    }
    bool *done = sh_xcalloc(n_remote + 1, sizeof *done);
    sh_import_why *why = sh_xcalloc(n_remote + 1, sizeof *why);
    char **errs = sh_xcalloc(n_remote + 1, sizeof *errs);
    sh_engine *e = sh_engine_new(st);
    *ok += sh_engine_import(e, remote, n_remote, done, why, errs);
    sh_engine_free(e);
    for (size_t i = 0; i < n_remote; ++i) {
        if (done[i]) continue;
        ++*failed;
        const char *src = remote[i]->source;
        if (why[i] == SH_IMPORT_UNREACHABLE) fail(st, remote[i], errs[i] ? sh_strdup(errs[i]) : sh_strfmt("cannot reach %s", src));
        else if (why[i] == SH_IMPORT_GONE) fail(st, remote[i], sh_strfmt("not found in %s any more", src));
        else fail(st, remote[i], sh_strfmt("could not install from %s", src));
        free(errs[i]);
    }
    free(errs);
    free(why);
    free(done);
    free(remote);
    sh_strv_free(&taken);
}
