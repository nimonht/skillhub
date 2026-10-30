// skillhub: the command line, written in C. With no command, or `gui`, it starts the window (skillhub-gui, skillhub.exe on
// Windows), which sits next to it. Every other command runs here; output and exit codes are those of 1.x, word for word,
// because scripts and tests rely on them (docs/formats.md, section 11).
#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sh_engine.h"
#include "sh_exe.h"
#include "sh_fs.h"
#include "sh_history.h"
#include "sh_json.h"
#include "sh_path.h"
#include "sh_serve.h"
#include "sh_store.h"
#include "sh_str.h"
#include "sh_utf.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

#ifndef SKILLHUB_VERSION
#define SKILLHUB_VERSION "dev"
#endif

// ---- output ----

static void out(const char *s) { fputs(s, stdout); }

static void outf(const char *fmt, ...) SH_PRINTF(1, 2);
static void outf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}

static void errf(const char *fmt, ...) SH_PRINTF(1, 2);
static void errf(const char *fmt, ...) {
    fflush(stdout);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);
}

// QString::leftJustified: pad with spaces to `width` UTF-16 units, never cut
static void padded(const char *s, size_t width) {
    out(s);
    for (size_t n = sh_utf16_len(s); n < width; ++n) putchar(' ');
}

static void log_line(void *user, const char *line) {
    (void)user;
    char *shown = sh_strdup(line);
    puts(shown);
    fflush(stdout);
    free(shown);
}

static const char *link_word(sh_link_state s) {
    switch (s) {
    case SH_LINK_NATIVE: return "shared";
    case SH_LINK_LINKED: return "link";
    case SH_LINK_MISSING: return "MISSING";
    case SH_LINK_COPY: return "COPY";
    case SH_LINK_FOREIGN: return "OTHER";
    }
    return "";
}

static const char *update_word(sh_update u) {
    switch (u) {
    case SH_UPDATE_UNTRACKED: return "local";
    case SH_UPDATE_UNKNOWN: return "tracked";
    case SH_UPDATE_CURRENT: return "up-to-date";
    case SH_UPDATE_AVAILABLE: return "update-available";
    case SH_UPDATE_ERROR: return "error";
    }
    return "";
}

static void usage(void) {
    out("skillhub " SKILLHUB_VERSION " - one skills directory for every coding agent\n\n"
        "  skillhub [gui]                  open the manager window\n"
        "  skillhub list [--json]          installed skills, source, per-agent link state\n"
        "  skillhub doctor [--fix]         report (and repair) strays, dead links, Antigravity config\n"
        "  skillhub adopt [--notify]       move stray skills from agent dirs into ~/.agents/skills\n"
        "  skillhub check                  fetch upstream, show available updates (exit 10 if any)\n"
        "  skillhub update [--all|NAME...] [--auto] [--notify]\n"
        "  skillhub add SOURCE... [--skill a,b | --all] [--link]   owner/repo, a GitLab or git URL, or a folder (--link: live)\n"
        "  skillhub remove NAME\n"
        "  skillhub restore NAME                   bring back a removed skill\n"
        "  skillhub diff NAME [--stat]             what an update would change, against the files on disk\n"
        "  skillhub update NAME... --review [--yes]   show the changes, ask, then apply exactly those\n"
        "  skillhub history NAME                   the earlier versions that are kept\n"
        "  skillhub rollback NAME [STAMP]          back to an earlier version; automatic updates then wait\n"
        "  skillhub hold NAME | release NAME       keep automatic updates away from a skill, or let them back\n"
        "  skillhub track NAME SOURCE [PATH]       set the upstream of a local skill\n"
        "  skillhub autoupdate on|off|status       daily update timer (Linux: systemd user timer)\n"
        "  skillhub watch on|off|status            auto-adopt strays (Linux: systemd path unit)\n"
        "  skillhub export [FILE] [--no-local]    write every skill to one file (https sources; local skills embedded)\n"
        "  skillhub import FILE [--yes] [--no-local] [--only a,b]   install the skills of such a file on this machine\n"
        "  skillhub providers [--all] [--json]     coding agents found on this machine and how each sees skills\n"
        "  skillhub providers enable|disable ID    manage (or stop managing) an agent\n"
        "  skillhub providers add LABEL DIR [--shared] | remove ID   your own agent (--shared: it reads the shared folder)\n"
        "  skillhub uninstall [--dry-run] [--yes] [--keep-cache]\n"
        "                                  remove units, installed files below $HOME and the cache (keeps your skills)\n"
        "  skillhub serve --stdio          the window's background service (JSON-RPC on standard input and output)\n");
    fflush(stdout);
}

// ---- arguments ----

typedef struct {
    char **v;
    int n;
} args_t;

static bool has(const args_t *a, const char *flag) {
    for (int i = 0; i < a->n; ++i)
        if (strcmp(a->v[i], flag) == 0) return true;
    return false;
}

static const char *after(const args_t *a, const char *flag) {
    for (int i = 0; i + 1 < a->n; ++i)
        if (strcmp(a->v[i], flag) == 0) return a->v[i + 1];
    return NULL;
}

static const char *first_plain(const args_t *a) {
    for (int i = 0; i < a->n; ++i)
        if (!sh_starts_with(a->v[i], "--")) return a->v[i];
    return NULL;
}

static void comma_list(const char *s, sh_strv *out_list) {
    if (s) sh_split(s, strlen(s), ',', true, out_list);
}

static bool stdin_is_terminal(void) {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(0) != 0;
#endif
}

static bool confirmed(void) {
    char line[256] = {0};
    if (!fgets(line, sizeof line, stdin)) return false;
    char *t = sh_trimmed(line, strlen(line));
    for (char *c = t; *c; ++c)
        if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    const bool yes = strcmp(t, "y") == 0 || strcmp(t, "yes") == 0;
    free(t);
    return yes;
}

static char *replace_all(const char *s, const char *what, const char *with) {
    sh_buf b = SH_BUF_INIT;
    const size_t n = strlen(what);
    for (const char *p = s; *p;) {
        if (n && strncmp(p, what, n) == 0) {
            sh_buf_puts(&b, with);
            p += n;
        } else {
            sh_buf_putc(&b, *p++);
        }
    }
    return sh_buf_steal(&b);
}

static char *today(const char *fmt) {
    char buf[32];
    const time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    strftime(buf, sizeof buf, fmt, &tm);
    return sh_strdup(buf);
}

// ---- commands ----

static int cmd_list(sh_store *st, const args_t *a) {
    sh_skills sk;
    sh_store_scan(st, &sk);
    if (has(a, "--json")) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *arr = yyjson_mut_arr(d);
        yyjson_mut_doc_set_root(d, arr);
        for (size_t i = 0; i < sk.len; ++i) {
            const sh_skill *s = &sk.items[i];
            yyjson_mut_val *o = yyjson_mut_obj(d);
            sh_json_set_str(d, o, "name", s->name);
            sh_json_set_str(d, o, "source", s->src.source);
            sh_json_set_str(d, o, "hash", s->src.hash);
            yyjson_mut_val *ag = yyjson_mut_obj(d);
            for (size_t k = 0; k < st->n_agents; ++k) sh_json_set_str(d, ag, st->agents[k]->id, link_word(s->links[k]));
            sh_json_set_val(d, o, "agents", ag);
            yyjson_mut_arr_append(arr, o);
        }
        char *json = sh_json_string(d, true);
        outf("%s\n", json);
        free(json);
        yyjson_mut_doc_free(d);
    } else {
        for (size_t i = 0; i < sk.len; ++i) {
            const sh_skill *s = &sk.items[i];
            padded(s->name, 30);
            padded(*s->src.source ? s->src.source : "local", 32);
            for (size_t k = 0; k < st->n_agents; ++k) {
                outf("%s=", st->agents[k]->id);
                padded(link_word(s->links[k]), 8);
                out(" ");
            }
            out("\n");
        }
        outf("%zu skills in %s\n", sk.len, st->central);
    }
    sh_skills_free(&sk);
    return 0;
}

static int cmd_doctor(sh_store *st, const args_t *a) {
    int problems = 0;
    sh_strays strays;
    sh_store_strays(st, &strays);
    for (size_t i = 0; i < strays.len; ++i) {
        outf("stray: %s in %s%s\n", strays.items[i].name, strays.items[i].agent_id, strays.items[i].duplicate ? " (duplicate of central)" : "");
        ++problems;
    }
    sh_strays_free(&strays);
    sh_skills sk;
    sh_store_scan(st, &sk);
    for (size_t i = 0; i < sk.len; ++i) {
        for (size_t k = 0; k < st->n_agents; ++k) {
            const sh_link_state ls = sk.items[i].links[k];
            if (ls == SH_LINK_MISSING || ls == SH_LINK_COPY || ls == SH_LINK_FOREIGN) {
                outf("link: %s / %s = %s\n", sk.items[i].name, st->agents[k]->id, link_word(ls));
                ++problems;
            }
        }
    }
    sh_skills_free(&sk);
    for (size_t k = 0; k < st->n_agents; ++k) {
        if (!sh_store_provider_wired(st, st->agents[k])) {
            outf("%s: %s does not list %s\n", st->agents[k]->id, st->agents[k]->config_file, st->central);
            ++problems;
        }
    }
    size_t only_other = 0;
    const bool split = sh_lock_split(st, &only_other);
    if (split) {  // npx and Skill Hub read different lock files (docs/formats.md, section 2)
        outf("lock: two lock files, %s and %s (%zu entries only in the second); `skillhub doctor --fix` merges them\n",
             st->lock_path, st->lock_other, only_other);
        ++problems;
    }
    if (problems) outf("problems: %d\n", problems);
    else out("healthy\n");
    if (has(a, "--fix") && problems) {
        if (split) {
            char *why = NULL;
            if (!sh_lock_migrate(st, &why)) errf("%s\n", why ? why : "");
            free(why);
        }
        const int n = sh_store_adopt(st);
        outf("fixed changes: %d\n", n);
        return 0;
    }
    return problems ? 1 : 0;
}

static int cmd_export(sh_store *st, const args_t *a) {
    const char *given = first_plain(a);
    char *date = today("%Y%m%d");
    char *file = given ? sh_strdup(given) : sh_strfmt("skillhub-skills-%s.json", date);
    free(date);
    int tracked = 0, local = 0;
    sh_strv skipped = SH_STRV_INIT;
    char *why = NULL;
    if (!sh_store_write_export(st, file, !has(a, "--no-local"), &tracked, &local, &skipped, &why)) {
        errf("%s\n", why ? why : "");
        free(why);
        free(file);
        sh_strv_free(&skipped);
        return 1;
    }
    outf("exported %d skills (%d from GitHub, %d local) to %s\n", tracked + local, tracked, local, file);
    for (size_t i = 0; i < skipped.len; ++i) outf("skipped %s\n", skipped.items[i]);
    if (local > 0) out("note: the files of the local skills are embedded in that file; share it only with people you trust\n");
    sh_strv_free(&skipped);
    free(file);
    return 0;
}

static int cmd_import(sh_store *st, const args_t *a) {
    const char *file = first_plain(a);
    if (!file) {
        usage();
        return 2;
    }
    char *why = NULL;
    sh_import_items items;
    sh_store_read_export(st, file, &items, &why);
    if (items.len == 0) {
        errf("%s\n", why ? why : "nothing to import in that file");
        free(why);
        sh_import_items_free(&items);
        return 1;
    }
    free(why);
    const bool no_local = has(a, "--no-local");
    sh_strv only = SH_STRV_INIT;
    comma_list(after(a, "--only"), &only);
    sh_import_item **todo = sh_xcalloc(items.len + 1, sizeof *todo);
    size_t n_todo = 0;
    for (size_t i = 0; i < items.len; ++i) {
        sh_import_item *it = &items.items[i];
        char *what = it->local ? sh_strfmt("local, %zu files", it->n_files) : sh_strdup(it->source);
        const char *mark = "+";
        char *tmp;
        if (*it->problem) {
            mark = "!";
            tmp = sh_strfmt("rejected: %s", it->problem);
        } else if (it->exists) {
            mark = "=";
            tmp = sh_strfmt("%s (already installed, kept)", what);
        } else if (it->local && no_local) {
            mark = "-";
            tmp = sh_strfmt("%s (skipped: --no-local)", what);
        } else if (only.len > 0 && !sh_strv_contains(&only, it->name)) {
            mark = "-";
            tmp = sh_strfmt("%s (not selected)", what);
        } else {
            tmp = sh_strdup(what);
            todo[n_todo++] = it;
        }
        free(what);
        outf("%s ", mark);
        padded(it->name, 30);
        outf("%s\n", tmp);
        free(tmp);
    }
    sh_strv_free(&only);
    int rc = 0;
    if (n_todo == 0) {
        out("nothing to install\n");
    } else {
        out("\nSkills are instructions your agents follow. Install only files you trust. GitHub skills arrive at their latest upstream version.\n");
        bool go = true;
        if (!has(a, "--yes")) {
            if (!stdin_is_terminal()) {
                errf("pass --yes to install %zu skills without a prompt\n", n_todo);
                go = false;
                rc = 2;
            } else {
                outf("Install %zu skills? [y/N] ", n_todo);
                fflush(stdout);
                if (!confirmed()) {
                    out("aborted\n");
                    go = false;
                    rc = 1;
                }
            }
        }
        if (go) {
            int ok = 0, failed = 0;
            sh_store_import_items(st, (const sh_import_item *const *)todo, n_todo, &ok, &failed);
            sh_store_relink(st);
            outf("%d installed, %d failed\n", ok, failed);
            rc = failed ? 1 : 0;
        }
    }
    free(todo);
    sh_import_items_free(&items);
    return rc;
}

static int cmd_providers(sh_store *st, const args_t *a) {
    const char *what = a->n > 0 ? a->v[0] : "";
    char *why = NULL;
    if (strcmp(what, "enable") == 0 || strcmp(what, "disable") == 0) {
        if (a->n < 2) {
            usage();
            return 2;
        }
        const bool on = strcmp(what, "enable") == 0;
        if (!sh_store_set_provider_managed(st, a->v[1], on, &why)) {
            errf("%s\n", why ? why : "");
            free(why);
            return 1;
        }
        outf("%s%s\n", a->v[1], on ? " is now managed" : " is no longer managed");
        return 0;
    }
    if (strcmp(what, "add") == 0) {
        if (a->n < 3) {
            usage();
            return 2;
        }
        char *id = sh_store_add_custom_provider(st, a->v[1], a->v[2], has(a, "--shared") ? "native" : "link", &why);
        if (!id) {
            errf("%s\n", why ? why : "");
            free(why);
            return 1;
        }
        outf("added %s\n", id);
        free(id);
        return 0;
    }
    if (strcmp(what, "remove") == 0) {
        if (a->n < 2) {
            usage();
            return 2;
        }
        return sh_store_remove_custom_provider(st, a->v[1]) ? 0 : 1;
    }
    const bool all = has(a, "--all");
    if (has(a, "--json")) {
        yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
        yyjson_mut_val *arr = yyjson_mut_arr(d);
        yyjson_mut_doc_set_root(d, arr);
        for (size_t i = 0; i < st->n_providers; ++i) {
            const sh_agent *p = &st->providers[i];
            if (!all && !p->managed) continue;
            yyjson_mut_val *o = yyjson_mut_obj(d);
            sh_json_set_str(d, o, "id", p->id);
            sh_json_set_str(d, o, "label", p->label);
            sh_json_set_str(d, o, "mode", p->mode);
            sh_json_set_str(d, o, "dir", p->dir);
            sh_json_set_bool(d, o, "detected", p->detected);
            sh_json_set_str(d, o, "detectedBy", p->detected_by);
            sh_json_set_bool(d, o, "managed", p->managed);
            sh_json_set_bool(d, o, "custom", p->custom);
            yyjson_mut_arr_append(arr, o);
        }
        char *json = sh_json_string(d, true);
        outf("%s\n", json);
        free(json);
        yyjson_mut_doc_free(d);
        return 0;
    }
    int shown = 0;
    for (size_t i = 0; i < st->n_providers; ++i) {
        const sh_agent *p = &st->providers[i];
        if (!all && !p->managed && !p->detected) continue;
        ++shown;
        const char *state = p->managed ? (p->detected ? "managed" : "managed (forced)") : (p->detected ? "detected, off" : "not found");
        char *where_raw = strcmp(p->mode, "link") == 0     ? sh_strfmt("links in %s", p->dir)
                          : strcmp(p->mode, "config") == 0 ? sh_strfmt("lists the shared folder in %s", p->config_file)
                                                           : sh_strdup("reads the shared folder");
        char *where = replace_all(where_raw, st->home, "~");
        padded(p->id, 18);
        padded(state, 17);
        padded(*p->detected_by ? p->detected_by : "-", 34);
        outf("%s\n", where);
        free(where);
        free(where_raw);
    }
    outf("%d%s%zu managed. `skillhub providers --all` lists every known agent.\n", shown, all ? " known providers, " : " providers shown, ",
         st->n_agents);
    return 0;
}

static int cmd_adopt(sh_store *st, const args_t *a) {
    const int n = sh_store_adopt(st);
    outf("%d change(s)\n", n);
    if (has(a, "--notify") && n) {
        char *body = sh_strfmt("Tidied %d stray skill change(s) into ~/.agents/skills", n);
        sh_notify("Skill Hub", body);
        free(body);
    }
    return 0;
}

static void print_file(const sh_diff_file *f) {
    outf("  %c  ", f->status);
    padded(f->path, 44);
    if (f->binary) out("binary");
    else outf("+%d -%d", f->added, f->removed);
    if (f->exec) out("  executable");
    if (f->risk) out("  [script]");
    out("\n");
}

static void print_patch(const sh_diff_file *f) {
    if (!*f->patch) return;
    outf("--- %s\n+++ %s\n%s", f->status == 'A' ? "/dev/null" : f->path, f->status == 'D' ? "/dev/null" : f->path, f->patch);
    if (f->cut) out("[... cut here: the file changes too much to show ...]\n");
}

static void print_review(const sh_review *r, bool patches) {
    if (r->err) {
        errf("%s: %s\n", r->name, r->err);
        return;
    }
    if (r->n == 0) {
        outf("%s: no differences\n", r->name);
        return;
    }
    outf("%s: %s\n", r->name, r->available ? "update available" : "no update, these are local edits");
    if (r->available && r->local_edits) out("  local edits will be replaced; a backup is kept\n");
    if (r->risk) out("  adds or changes scripts: read them before you agree\n");
    for (size_t i = 0; i < r->n; ++i) print_file(&r->files[i]);
    for (size_t i = 0; patches && i < r->n; ++i) print_patch(&r->files[i]);
}

static sh_skill *skill_named(sh_skills *sk, const char *name) {
    for (size_t i = 0; i < sk->len; ++i)
        if (strcmp(sk->items[i].name, name) == 0) return &sk->items[i];
    return NULL;
}

static int cmd_diff(sh_store *st, const args_t *a) {
    if (a->n == 0 || sh_starts_with(a->v[0], "--")) {
        usage();
        return 2;
    }
    sh_skills sk;
    sh_store_scan(st, &sk);
    sh_skill *s = skill_named(&sk, a->v[0]);
    int rc = 1;
    if (!s) {
        errf("no skill named %s\n", a->v[0]);
    } else {
        sh_engine *engine = sh_engine_new(st);
        sh_review r;
        sh_engine_diff(engine, &s, 1, &r);
        print_review(&r, !has(a, "--stat"));
        rc = r.err ? 1 : 0;
        if (r.stage) sh_remove_tree(r.stage);  // nobody applies it
        sh_review_free(&r);
        sh_engine_free(engine);
    }
    sh_skills_free(&sk);
    return rc;
}

// skillhub update ... --review: what is applied is what was shown, even when upstream moves on while the question is open
static int update_reviewed(const args_t *a, sh_skills *sk, const sh_strv *names, sh_engine *engine) {
    sh_skill **todo = sh_xcalloc(sk->len + 1, sizeof *todo);
    size_t n = 0;
    for (size_t i = 0; i < sk->len; ++i)
        if ((names->len == 0 || sh_strv_contains(names, sk->items[i].name)) && sk->items[i].update == SH_UPDATE_AVAILABLE) todo[n++] = &sk->items[i];
    sh_review *reviews = sh_xcalloc(n + 1, sizeof *reviews);
    if (n) sh_engine_diff(engine, todo, n, reviews);
    sh_skill **ready = sh_xcalloc(n + 1, sizeof *ready);
    const char **stages = sh_xcalloc(n + 1, sizeof *stages);
    size_t n_ready = 0;
    for (size_t i = 0; i < n; ++i) {
        print_review(&reviews[i], true);
        if (!reviews[i].stage) continue;
        ready[n_ready] = todo[i];
        stages[n_ready++] = reviews[i].stage;
    }
    bool go = n_ready > 0;
    int rc = 0;
    if (go && !has(a, "--yes")) {
        if (!stdin_is_terminal()) {
            errf("pass --yes to apply %zu update(s) without a prompt\n", n_ready);
            go = false;
            rc = 2;
        } else {
            outf("Apply %zu update(s)? [y/N] ", n_ready);
            fflush(stdout);
            if (!confirmed()) {
                out("aborted\n");
                go = false;
                rc = 1;
            }
        }
    }
    bool *done = sh_xcalloc(n_ready + 1, sizeof *done);
    const int updated = go ? sh_engine_apply(engine, ready, stages, n_ready, done) : 0;
    if (!go)
        for (size_t i = 0; i < n_ready; ++i) sh_remove_tree(stages[i]);
    if (go || n_ready == 0) {
        outf("%d updated, %d failed\n", updated, (int)n_ready - updated);
        rc = updated == (int)n_ready ? 0 : 1;
    }
    free(done);
    for (size_t i = 0; i < n; ++i) sh_review_free(&reviews[i]);
    free(reviews);
    free(stages);
    free(ready);
    free(todo);
    return rc;
}

static int cmd_history(sh_store *st, const args_t *a) {
    if (a->n == 0) {
        usage();
        return 2;
    }
    sh_versions v;
    sh_history_list(st, a->v[0], &v);
    if (v.len == 0) outf("%s has no earlier versions\n", a->v[0]);
    for (size_t i = 0; i < v.len; ++i) {
        padded(v.items[i].stamp, 20);
        padded(*v.items[i].reason ? v.items[i].reason : "backup", 10);
        outf("%.8s\n", v.items[i].tree);
    }
    sh_versions_free(&v);
    sh_holds *holds = sh_holds_load(st);
    if (sh_holds_get(holds, a->v[0])) outf("on hold: automatic updates leave %s alone (skillhub release %s)\n", a->v[0], a->v[0]);
    sh_holds_free(holds);
    return 0;
}

static int cmd_rollback(sh_store *st, const args_t *a) {
    if (a->n == 0) {
        usage();
        return 2;
    }
    char *err = NULL;
    const bool ok = sh_history_rollback(st, a->v[0], a->n > 1 ? a->v[1] : NULL, &err);
    if (!ok) errf("%s\n", err ? err : "");
    free(err);
    return ok ? 0 : 1;
}

static int cmd_restore(sh_store *st, const args_t *a) {
    if (a->n == 0) {
        usage();
        return 2;
    }
    char *err = NULL;
    const bool ok = sh_history_restore(st, a->v[0], &err);
    if (!ok) errf("%s\n", err ? err : "");
    free(err);
    return ok ? 0 : 1;
}

static int cmd_hold(sh_store *st, const char *cmd, const args_t *a) {
    if (a->n == 0) {
        usage();
        return 2;
    }
    const char *name = a->v[0];
    if (strcmp(cmd, "release") == 0) {
        const char *const names[] = {name};
        if (sh_holds_release(st, names, 1)) {
            outf("released %s\n", name);
            return 0;
        }
        errf("%s is not on hold\n", name);
        return 1;
    }
    sh_skills sk;
    sh_store_scan(st, &sk);
    const bool known = skill_named(&sk, name) != NULL;
    sh_skills_free(&sk);
    char *err = NULL;
    const bool ok = known && sh_hold_set(st, name, "", &err);
    if (ok) outf("%s is on hold: automatic updates leave it alone\n", name);
    else errf("%s\n", err ? err : "no such skill");
    free(err);
    return ok ? 0 : 1;
}

static int cmd_check_update(sh_store *st, const args_t *a, bool do_update) {
    sh_skills sk;
    sh_store_scan(st, &sk);
    sh_strv names = SH_STRV_INIT;
    bool all = false;
    for (int i = 0; i < a->n; ++i) {
        if (strcmp(a->v[i], "--all") == 0) all = true;
        else if (!sh_starts_with(a->v[i], "--")) sh_strv_push(&names, a->v[i]);
    }
    if (do_update && names.len == 0 && !all) {
        errf("update: give skill names or --all\n");
        sh_strv_free(&names);
        sh_skills_free(&sk);
        return 2;
    }
    if (do_update && has(a, "--auto")) sh_store_adopt(st);  // housekeeping before the update run
    const sh_strv none = SH_STRV_INIT;
    sh_engine *engine = sh_engine_new(st);
    const int avail = sh_engine_check(engine, &sk, do_update && !all ? &names : &none);
    if (do_update && has(a, "--review")) {
        const int reviewed = update_reviewed(a, &sk, all ? &none : &names, engine);
        sh_engine_free(engine);
        sh_strv_free(&names);
        sh_skills_free(&sk);
        return reviewed;
    }
    if (do_update && has(a, "--auto")) sh_engine_set_auto(engine);
    sh_skill **todo = sh_xcalloc(sk.len + 1, sizeof *todo);
    size_t n_todo = 0;
    for (size_t i = 0; i < sk.len; ++i) {
        sh_skill *s = &sk.items[i];
        if (names.len > 0 && !all && !sh_strv_contains(&names, s->name)) continue;
        if (s->update == SH_UPDATE_AVAILABLE || s->update == SH_UPDATE_ERROR) {
            padded(s->name, 30);
            out(update_word(s->update));
            if (*s->note) outf(" (%s)", s->note);
            out("\n");
        }
        if (do_update && s->update == SH_UPDATE_AVAILABLE) todo[n_todo++] = s;
    }
    fflush(stdout);
    bool *done = sh_xcalloc(n_todo + 1, sizeof *done);
    const int updated = n_todo ? sh_engine_update(engine, todo, n_todo, done) : 0;
    int held = 0;
    for (size_t i = 0; i < n_todo; ++i) held += todo[i]->left_out;
    const int failed = (int)n_todo - updated - held;
    sh_strv changed = SH_STRV_INIT;
    for (size_t i = 0; i < n_todo; ++i)
        if (done[i]) sh_strv_push(&changed, todo[i]->name);
    free(done);
    free(todo);
    sh_engine_free(engine);
    int rc;
    if (!do_update) {
        outf("%d update(s) available\n", avail);
        rc = avail ? 10 : 0;
    } else {
        outf("%d updated, %d failed\n", updated, failed);
        if (held) outf("%d left out\n", held);
        if (has(a, "--notify") && (updated || failed)) {
            char *joined = sh_strv_join(&changed, ", ");
            char *short_list = sh_left_utf16(joined, 160);
            char *extra = failed ? sh_strfmt(", %d failed", failed) : sh_strdup("");
            char *body = sh_strfmt("%d skill(s) updated%s: %s", updated, extra, short_list);
            sh_notify("Skill Hub", body);
            free(body);
            free(extra);
            free(short_list);
            free(joined);
        }
        rc = failed ? 1 : 0;
    }
    sh_strv_free(&changed);
    sh_strv_free(&names);
    sh_skills_free(&sk);
    return rc;
}

// The chosen candidates of every repository, one per name: when two repositories offer the same name, the one named
// last on the command line wins, as if they had been added one after the other.
static size_t chosen(sh_store *st, const sh_discovery *found, size_t n, bool all, const sh_strv *want, const sh_candidate **pick) {
    size_t k = 0;
    for (size_t r = 0; r < n; ++r) {
        for (size_t i = 0; i < found[r].cands.len; ++i) {
            const sh_candidate *c = &found[r].cands.items[i];
            if (!all && !sh_strv_contains(want, c->name)) continue;
            for (size_t j = 0; j < k; ++j) {
                if (pick[j] && strcmp(pick[j]->name, c->name) == 0) {
                    sh_store_log(st, "skipping %s from %s: %s has a skill of that name too", c->name, pick[j]->slug, c->slug);
                    pick[j] = NULL;
                }
            }
            pick[k++] = c;
        }
    }
    size_t kept = 0;
    for (size_t j = 0; j < k; ++j)
        if (pick[j]) pick[kept++] = pick[j];
    return kept;
}

// skillhub add SOURCE... [--skill a,b | --all] [--link]: one source behaves exactly as in 1.x; several are read, then
// installed, all at once
static int cmd_add(sh_store *st, const args_t *a) {
    sh_strv repos = SH_STRV_INIT;
    for (int i = 0; i < a->n; ++i) {
        if (strcmp(a->v[i], "--skill") == 0) {
            ++i;  // its value is not a repository
            continue;
        }
        if (!sh_starts_with(a->v[i], "--")) sh_strv_push(&repos, a->v[i]);
    }
    if (repos.len == 0) {
        usage();
        sh_strv_free(&repos);
        return 2;
    }
    sh_strv want = SH_STRV_INIT;
    comma_list(after(a, "--skill"), &want);
    const bool all = has(a, "--all");
    const bool listing = !all && want.len == 0;
    sh_engine *engine = sh_engine_new(st);
    sh_discovery *found = sh_xcalloc(repos.len, sizeof *found);
    sh_engine_discover(engine, (const char *const *)repos.items, repos.len, found);
    int failed_repos = 0;
    size_t n_cands = 0;
    for (size_t r = 0; r < repos.len; ++r) {
        n_cands += found[r].cands.len;
        if (found[r].cands.len == 0) {
            errf("%s\n", found[r].err ? found[r].err : "");
            ++failed_repos;
        } else if (listing) {
            outf("skills in %s:\n", repos.items[r]);
            for (size_t i = 0; i < found[r].cands.len; ++i) {
                char *desc = sh_left_utf16(found[r].cands.items[i].description, 80);
                out("  ");
                padded(found[r].cands.items[i].name, 30);
                outf("%s\n", desc);
                free(desc);
            }
        }
    }
    int ok = 0;
    if (!listing) {
        const sh_candidate **pick = sh_xcalloc(n_cands + 1, sizeof *pick);
        const size_t n_pick = chosen(st, found, repos.len, all, &want, pick);
        bool *done = sh_xcalloc(n_pick + 1, sizeof *done);
        ok = n_pick ? sh_engine_install(engine, pick, n_pick, has(a, "--link"), done) : 0;
        free(done);
        free(pick);
    }
    int rc;
    if (listing) {
        if (failed_repos < (int)repos.len) out("re-run with --skill NAME[,NAME] or --all\n");
        rc = failed_repos ? 1 : 0;
    } else {
        if (failed_repos < (int)repos.len) outf("%d installed\n", ok);
        rc = ok && !failed_repos ? 0 : 1;
    }
    for (size_t r = 0; r < repos.len; ++r) sh_discovery_free(&found[r]);
    free(found);
    sh_engine_free(engine);
    sh_strv_free(&want);
    sh_strv_free(&repos);
    return rc;
}

static int cmd_uninstall(sh_store *st, const args_t *a) {
    const bool dry = has(a, "--dry-run");
    if (!dry && !has(a, "--yes")) {
        out("This removes the skillhub units, installed files below $HOME and its cache. Your skills stay.\n"
            "Preview with --dry-run. Continue? [y/N] ");
        fflush(stdout);
        if (!confirmed()) {
            out("aborted\n");
            return 1;
        }
    }
    return sh_store_uninstall(st, dry, has(a, "--keep-cache")) ? 0 : 1;
}

static int cmd_automation(sh_store *st, const char *cmd, const args_t *a) {
    if (!sh_automation_supported()) {
        errf("skillhub %s is not available on this platform: it needs systemd user units (Linux only)\n", cmd);
        return 1;
    }
    const bool timer = strcmp(cmd, "autoupdate") == 0;
    const char *unit = timer ? "skillhub-update.timer" : "skillhub-adopt.path";
    const char *what = a->n > 0 ? a->v[0] : "";
    char *e = NULL;
    if (strcmp(what, "on") == 0) {
        if (!sh_store_install_units(st, timer, !timer, &e)) {
            errf("%s\n", e ? e : "");
            free(e);
            return 1;
        }
    } else if (strcmp(what, "off") == 0) {
        sh_store_remove_units(st, timer, !timer, &e);
    }
    free(e);
    char *state = sh_store_unit_state(st, unit);
    outf("%s: %s\n", unit, state);
    free(state);
    return 0;
}

// ---- one change at a time ----

#define EXIT_BUSY 75

static bool is_any(const char *s, const char *const *words) {
    for (; *words; ++words)
        if (strcmp(s, *words) == 0) return true;
    return false;
}

// commands that change skills, links, lock files or config
static bool mutates(const char *cmd, const args_t *a) {
    static const char *const always[] = {"adopt", "update", "remove", "restore", "rollback", "hold", "release", "track", "import", "uninstall", NULL};
    static const char *const provider_changes[] = {"enable", "disable", "add", "remove", NULL};
    static const char *const switches[] = {"on", "off", NULL};
    if (is_any(cmd, always)) return true;
    if (strcmp(cmd, "add") == 0) return has(a, "--all") || has(a, "--skill");  // listing what a repository offers changes nothing
    if (strcmp(cmd, "doctor") == 0) return has(a, "--fix");
    if (strcmp(cmd, "providers") == 0) return a->n > 0 && is_any(a->v[0], provider_changes);
    if (strcmp(cmd, "autoupdate") == 0 || strcmp(cmd, "watch") == 0) return a->n > 0 && is_any(a->v[0], switches);
    return false;
}

typedef enum { LOCK_HELD, LOCK_BUSY, LOCK_STEP_ASIDE } lock_outcome;

// Changing commands hold <state>/mutate.lock, so two Skill Hub processes never change things at the same time
// (docs/formats.md, section 8). The watcher's own run (`adopt --notify`) steps aside at once and quietly: the operation
// that holds the lock rescans when it ends.
static lock_outcome take_mutation_lock(const char *cmd, const args_t *a, int wait_s, sh_flock **held) {
    *held = NULL;
    char *state = sh_state_dir();
    if (!*state) {  // SKILLHUB_STATE set but empty: there is no state folder to lock in
        free(state);
        return LOCK_HELD;
    }
    char *path = sh_path_join(state, "mutate.lock");
    free(state);
    *held = sh_flock_take(path, 0);
    const bool watcher = strcmp(cmd, "adopt") == 0 && has(a, "--notify");
    if (!*held && !watcher) {
        errf("waiting for another Skill Hub operation\xe2\x80\xa6\n");
        *held = sh_flock_take(path, wait_s * 1000);
        if (!*held) errf("another Skill Hub operation is still running; try again later\n");
    }
    free(path);
    if (*held) return LOCK_HELD;
    return watcher ? LOCK_STEP_ASIDE : LOCK_BUSY;
}

static int run_cli(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "serve") == 0) {  // the window's backend: its own process, no store of the command line's
        if (argc != 3 || strcmp(argv[2], "--stdio") != 0) {
            errf("usage: skillhub serve --stdio\n");
            return 2;
        }
        return sh_serve_stdio();
    }
    // --wait SECONDS (how long a changing command waits for another one to finish) is taken out before the commands see
    // their arguments
    int wait_s = 60;
    char **args = sh_xcalloc((size_t)argc + 1, sizeof *args);
    int n_args = 0;
    for (int i = 0; i < argc; ++i) {
        if (i >= 2 && strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
            wait_s = atoi(argv[++i]);
            continue;
        }
        args[n_args++] = argv[i];
    }
    argc = n_args;
    argv = args;
    sh_store st;
    sh_store_init(&st, log_line, NULL);
    const char *cmd = argc > 1 ? argv[1] : "";
    const args_t a = {argv + 2, argc > 2 ? argc - 2 : 0};
    sh_flock *lock = NULL;
    const lock_outcome got = mutates(cmd, &a) ? take_mutation_lock(cmd, &a, wait_s, &lock) : LOCK_HELD;
    if (got != LOCK_HELD) {
        sh_store_free(&st);
        free(args);
        return got == LOCK_BUSY ? EXIT_BUSY : 0;
    }
    if (lock) sh_store_recover(&st, true);  // a process that died while changing skills may have left work half done
    int rc;
    if (strcmp(cmd, "list") == 0) rc = cmd_list(&st, &a);
    else if (strcmp(cmd, "doctor") == 0) rc = cmd_doctor(&st, &a);
    else if (strcmp(cmd, "export") == 0) rc = cmd_export(&st, &a);
    else if (strcmp(cmd, "import") == 0) rc = cmd_import(&st, &a);
    else if (strcmp(cmd, "providers") == 0) rc = cmd_providers(&st, &a);
    else if (strcmp(cmd, "adopt") == 0) rc = cmd_adopt(&st, &a);
    else if (strcmp(cmd, "check") == 0) rc = cmd_check_update(&st, &a, false);
    else if (strcmp(cmd, "update") == 0) rc = cmd_check_update(&st, &a, true);
    else if (strcmp(cmd, "add") == 0) rc = cmd_add(&st, &a);
    else if (strcmp(cmd, "diff") == 0) rc = cmd_diff(&st, &a);
    else if (strcmp(cmd, "history") == 0) rc = cmd_history(&st, &a);
    else if (strcmp(cmd, "rollback") == 0) rc = cmd_rollback(&st, &a);
    else if (strcmp(cmd, "restore") == 0) rc = cmd_restore(&st, &a);
    else if (strcmp(cmd, "hold") == 0 || strcmp(cmd, "release") == 0) rc = cmd_hold(&st, cmd, &a);
    else if (strcmp(cmd, "remove") == 0) {
        if (a.n == 0) {
            usage();
            rc = 2;
        } else {
            rc = sh_store_remove_skill(&st, a.v[0]) ? 0 : 1;
        }
    } else if (strcmp(cmd, "track") == 0) {
        if (a.n < 2) {
            usage();
            rc = 2;
        } else {
            char *e = NULL;
            rc = sh_store_track(&st, a.v[0], a.v[1], a.n > 2 ? a.v[2] : "", &e) ? 0 : 1;
            if (rc) errf("%s\n", e ? e : "");
            free(e);
        }
    } else if (strcmp(cmd, "uninstall") == 0) rc = cmd_uninstall(&st, &a);
    else if (strcmp(cmd, "autoupdate") == 0 || strcmp(cmd, "watch") == 0) rc = cmd_automation(&st, cmd, &a);
    else {
        usage();
        rc = !*cmd || strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ? 0 : 2;
    }
    fflush(stdout);
    sh_flock_release(lock);
    sh_store_free(&st);
    free(args);
    return rc;
}

// ---- the window ----

#ifdef _WIN32

#define GUI_NAME L"skillhub.exe"

// One argument for CreateProcess, quoted the way CommandLineToArgvW and the C runtime split it again.
static wchar_t *append_quoted(wchar_t *o, const wchar_t *arg) {
    if (*arg && !wcspbrk(arg, L" \t\n\v\"")) {
        const size_t n = wcslen(arg);
        memcpy(o, arg, n * sizeof *o);
        return o + n;
    }
    *o++ = L'"';
    for (const wchar_t *p = arg;; ++p) {
        size_t backslashes = 0;
        while (*p == L'\\') {
            ++p;
            ++backslashes;
        }
        if (*p == L'\0') {
            for (size_t i = 0; i < backslashes * 2; ++i) *o++ = L'\\';
            break;
        }
        const size_t keep = *p == L'"' ? backslashes * 2 + 1 : backslashes;
        for (size_t i = 0; i < keep; ++i) *o++ = L'\\';
        *o++ = *p;
    }
    *o++ = L'"';
    return o;
}

static void inherit(DWORD which, HANDLE *slot) {
    HANDLE h = GetStdHandle(which);
    if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    *slot = h;
}

// Windows has no exec: start the window with the same standard handles, wait, and pass its exit code on.
static int start_gui(int argc, wchar_t **argv) {
    static wchar_t gui[32768];
    const wchar_t *override = _wgetenv(L"SKILLHUB_GUI_BIN");
    if (override && *override) {
        if (wcslen(override) >= sizeof gui / sizeof gui[0]) return 127;
        wcscpy(gui, override);
    } else {
        const DWORD n = GetModuleFileNameW(NULL, gui, (DWORD)(sizeof gui / sizeof gui[0]));
        wchar_t *slash = n ? wcsrchr(gui, L'\\') : NULL;
        if (!slash || (size_t)(slash - gui) + 1 + wcslen(GUI_NAME) >= sizeof gui / sizeof gui[0]) {
            fputs("skillhub: cannot find its own location\n", stderr);
            return 127;
        }
        wcscpy(slash + 1, GUI_NAME);
    }
    size_t size = 2 * wcslen(gui) + 3;
    for (int i = 1; i < argc; ++i) size += 2 * wcslen(argv[i]) + 3;
    wchar_t *cmd = calloc(size + 1, sizeof *cmd);
    if (!cmd) return 127;
    wchar_t *end = append_quoted(cmd, gui);
    for (int i = 1; i < argc; ++i) {
        *end++ = L' ';
        end = append_quoted(end, argv[i]);
    }
    *end = L'\0';
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    inherit(STD_INPUT_HANDLE, &si.hStdInput);
    inherit(STD_OUTPUT_HANDLE, &si.hStdOutput);
    inherit(STD_ERROR_HANDLE, &si.hStdError);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(gui, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fwprintf(stderr, L"skillhub: cannot start %ls (error %lu)\n", gui, (unsigned long)GetLastError());
        free(cmd);
        return 127;
    }
    free(cmd);
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

int wmain(int argc, wchar_t **wargv) {
    SetConsoleOutputCP(CP_UTF8);
    char **argv = calloc((size_t)argc + 1, sizeof *argv);
    for (int i = 0; i < argc; ++i) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = malloc((size_t)(n > 0 ? n : 1));
        if (!argv[i] || n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL)) argv[i] = "";
    }
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0)) {
        printf("skillhub %s\n", SKILLHUB_VERSION);
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "gui") == 0) return start_gui(argc, wargv);
    return run_cli(argc, argv);
}

#else  // POSIX

#define GUI_NAME "skillhub-gui"

static int start_gui(int argc, char **argv) {
    char gui[4096];
    const char *override = getenv("SKILLHUB_GUI_BIN");
    if (override && *override) {
        if (strlen(override) >= sizeof gui) return 127;
        strcpy(gui, override);
    } else {
        // the real location: a link such as /usr/local/bin/skillhub -> the macOS bundle must not decide where to look
        const size_t n = sh_exe_dir(gui, sizeof gui);
        if (n == 0 || n + 1 + sizeof GUI_NAME > sizeof gui) {
            fputs("skillhub: cannot find its own location\n", stderr);
            return 127;
        }
        gui[n] = '/';
        memcpy(gui + n + 1, GUI_NAME, sizeof GUI_NAME);
    }
    char **args = calloc((size_t)argc + 2, sizeof *args);
    if (!args) return 127;
    args[0] = gui;
    for (int i = 1; i < argc; ++i) args[i] = argv[i];
    execv(gui, args);
    fprintf(stderr, "skillhub: cannot start %s: %s\n", gui, strerror(errno));
    free(args);
    return 127;
}

int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0)) {
        printf("skillhub %s\n", SKILLHUB_VERSION);
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "gui") == 0) return start_gui(argc, argv);
    signal(SIGPIPE, SIG_IGN);  // a program that stops reading its input must not end this one
    signal(SIGCHLD, SIG_DFL);  // an ignored SIGCHLD is inherited through exec, and waitpid could then never see an exit code
    return run_cli(argc, argv);
}

#endif
