#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#define _FILE_OFFSET_BITS 64
#endif

#include "sh_fs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sh_link.h"
#include "sh_path.h"
#include "sh_thread.h"
#include "sh_time.h"
#include "sh_utf.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#ifdef _WIN32

wchar_t *sh_wide(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = sh_xmalloc((size_t)(n > 0 ? n : 1) * sizeof *w);
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}

wchar_t *sh_wide_path(const char *path) {
    wchar_t *w = sh_wide(path);
    for (wchar_t *c = w; *c; ++c)
        if (*c == L'/') *c = L'\\';
    const bool drive = ((w[0] >= L'A' && w[0] <= L'Z') || (w[0] >= L'a' && w[0] <= L'z')) && w[1] == L':' && w[2] == L'\\';
    const bool unc = w[0] == L'\\' && w[1] == L'\\' && w[2] != L'?';
    if (!drive && !unc) return w;
    const size_t n = wcslen(w);
    wchar_t *out = sh_xmalloc((n + 8) * sizeof *out);
    if (drive) {
        wcscpy(out, L"\\\\?\\");
        wcscat(out, w);
    } else {
        wcscpy(out, L"\\\\?\\UNC");
        wcscat(out, w + 1);
    }
    free(w);
    return out;
}

char *sh_narrow(const wchar_t *w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = sh_xmalloc((size_t)(n > 0 ? n : 1));
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) s[0] = 0;
    return s;
}

static DWORD attributes(const char *path) {
    wchar_t *w = sh_wide_path(path);
    const DWORD a = GetFileAttributesW(w);
    free(w);
    return a;
}

#endif

// ---- reading and writing ----

// Files are opened close-on-exec, so a git process another thread starts meanwhile never inherits them. macOS gets the
// same from POSIX_SPAWN_CLOEXEC_DEFAULT in sh_proc; Windows passes an explicit handle list.
#if defined(__linux__)
#define FOPEN_CLOEXEC "e"
#else
#define FOPEN_CLOEXEC ""
#endif

bool sh_read_file(const char *path, size_t max, sh_buf *out) {
#ifdef _WIN32
    wchar_t *w = sh_wide_path(path);
    FILE *f = _wfopen(w, L"rb");
    free(w);
#else
    FILE *f = fopen(path, "rb" FOPEN_CLOEXEC);
#endif
    if (!f) return false;
    char chunk[65536];
    size_t total = 0;
    if (!out->data) sh_buf_append(out, "", 0);
    for (;;) {
        size_t want = sizeof chunk;
        if (max && max - total < want) want = max - total;
        if (want == 0) break;
        const size_t n = fread(chunk, 1, want, f);
        if (n == 0) break;
        sh_buf_append(out, chunk, n);
        total += n;
    }
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

bool sh_write_file(const char *path, const void *data, size_t n) {
#ifdef _WIN32
    wchar_t *w = sh_wide_path(path);
    FILE *f = _wfopen(w, L"wb");
    free(w);
#else
    FILE *f = fopen(path, "wb" FOPEN_CLOEXEC);
#endif
    if (!f) return false;
    const bool ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

bool sh_mkpath(const char *path) {
    if (!*path || sh_is_dir(path)) return true;
    char *parent = sh_path_parent(path);
    const bool parent_ok = !*parent || strcmp(parent, path) == 0 || sh_mkpath(parent);
    free(parent);
    if (!parent_ok) return false;
#ifdef _WIN32
    wchar_t *w = sh_wide_path(path);
    const bool ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    free(w);
    return ok;
#else
    return mkdir(path, 0777) == 0 || errno == EEXIST;
#endif
}

bool sh_write_atomic(const char *path_in, const void *data, size_t n) {
    char *path = NULL;
    if (sh_is_link(path_in)) path = sh_link_target(path_in);  // write through to the link's target
    if (!path || !*path) {
        free(path);
        path = sh_strdup(path_in);
    }
    char *dir = sh_path_parent(path);
    if (*dir && !sh_mkpath(dir)) {
        free(dir);
        free(path);
        return false;
    }
    free(dir);
    static long counter;  // shared by every thread
#ifdef _WIN32
    char *tmp = sh_strfmt("%s.tmp-%lu-%ld", path, (unsigned long)GetCurrentProcessId(), sh_atomic_inc(&counter));
#else
    char *tmp = sh_strfmt("%s.tmp-%ld-%ld", path, (long)getpid(), sh_atomic_inc(&counter));
#endif
    bool ok = false;
#ifdef _WIN32
    wchar_t *wt = sh_wide_path(tmp), *wp = sh_wide_path(path);
    HANDLE h = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ok = (n == 0 || (WriteFile(h, data, (DWORD)n, &written, NULL) && written == n)) && FlushFileBuffers(h);
        CloseHandle(h);
        // retried: another program (an antivirus, an editor) may hold the file for a moment
        for (int i = 0; ok && i < 10; ++i) {
            if (MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
            if (i == 9) ok = false;
            else Sleep(50);
        }
        if (!ok) DeleteFileW(wt);
    }
    free(wt);
    free(wp);
#else
    const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        const char *p = data;
        size_t left = n;
        ok = true;
        while (left > 0) {
            const ssize_t w = write(fd, p, left);
            if (w < 0) {
                if (errno == EINTR) continue;
                ok = false;
                break;
            }
            p += w;
            left -= (size_t)w;
        }
        struct stat st;
        if (ok && stat(path, &st) == 0) fchmod(fd, st.st_mode & 07777);  // keep the permissions of the file it replaces
        ok = fsync(fd) == 0 && ok;
        ok = close(fd) == 0 && ok;
        if (ok) ok = rename(tmp, path) == 0;
        if (!ok) unlink(tmp);
    }
#endif
    free(tmp);
    free(path);
    return ok;
}

// ---- inspecting ----

bool sh_exists(const char *path) {
#ifdef _WIN32
    if (attributes(path) == INVALID_FILE_ATTRIBUTES) return false;
    if (!sh_is_link(path)) return true;
    wchar_t *w = sh_wide_path(path);  // a link: does its target exist?
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

bool sh_lexists(const char *path) {
#ifdef _WIN32
    return attributes(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return lstat(path, &st) == 0;
#endif
}

bool sh_is_dir(const char *path) {
#ifdef _WIN32
    const DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && sh_exists(path);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool sh_is_file(const char *path) {
#ifdef _WIN32
    const DWORD a = attributes(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) && sh_exists(path);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

bool sh_is_executable(const char *path) {
#ifdef _WIN32
    return sh_is_file(path);  // Windows has no execute bit; callers that care look at the name
#else
    return sh_is_file(path) && access(path, X_OK) == 0;
#endif
}

int64_t sh_file_size(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    wchar_t *w = sh_wide_path(path);
    const BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    return ok ? (int64_t)(((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow) : -1;
#else
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
#endif
}

int64_t sh_mtime_ms(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA d;
    wchar_t *w = sh_wide_path(path);
    const BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok) return -1;
    const uint64_t t = ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    return (int64_t)(t / 10000 - 11644473600000ULL);
#else
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
#if defined(__APPLE__)
    return (int64_t)st.st_mtimespec.tv_sec * 1000 + st.st_mtimespec.tv_nsec / 1000000;
#else
    return (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000;
#endif
#endif
}

char *sh_canonical(const char *path) {
#ifdef _WIN32
    wchar_t *w = sh_wide_path(path);
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    wchar_t buf[32768];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, 32768, FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (n == 0 || n >= 32768) return NULL;
    const wchar_t *p = buf;
    if (wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0) {
        buf[6] = L'\\';
        p = buf + 6;
    } else if (wcsncmp(p, L"\\\\?\\", 4) == 0) {
        p = buf + 4;
    }
    char *s = sh_narrow(p);
    char *clean = sh_path_clean(s);
    free(s);
    return clean;
#else
    char *r = realpath(path, NULL);
    if (!r) return NULL;
    char *out = sh_strdup(r);
    free(r);
    return out;
#endif
}

// ---- listing ----

int sh_compare_ci(const char *a, const char *b) { return sh_casecmp(a, b); }

bool sh_list_dir(const char *path, int flags, sh_strv *names) {
#ifdef _WIN32
    char *pattern = sh_path_join(path, "*");
    wchar_t *w = sh_wide_path(pattern);
    free(pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char *name = sh_narrow(fd.cFileName);
        const bool hidden = name[0] == '.';
        const bool link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        bool keep = !((flags & SH_LIST_NO_HIDDEN) && hidden) && !((flags & SH_LIST_NO_SYMLINKS) && link);
        if (keep && (flags & SH_LIST_DIRS) && !(flags & SH_LIST_ALL)) {
            char *full = sh_path_join(path, name);
            keep = link ? sh_is_dir(full) : (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            free(full);
        }
        if (keep) sh_strv_take(names, name);
        else free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        const char *name = e->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if ((flags & SH_LIST_NO_HIDDEN) && name[0] == '.') continue;
        char *full = sh_path_join(path, name);
        struct stat lst;
        bool keep = lstat(full, &lst) == 0;
        if (keep && (flags & SH_LIST_NO_SYMLINKS) && S_ISLNK(lst.st_mode)) keep = false;
        if (keep && (flags & SH_LIST_DIRS) && !(flags & SH_LIST_ALL)) {
            struct stat st;
            keep = S_ISLNK(lst.st_mode) ? (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) : S_ISDIR(lst.st_mode);
        }
        free(full);
        if (keep) sh_strv_push(names, name);
    }
    closedir(d);
#endif
    if (flags & SH_SORT_NAME_CI) sh_strv_sort(names, sh_compare_ci);
    else if (flags & SH_SORT_NAME) sh_strv_sort(names, strcmp);
    return true;
}

static bool walk(const char *root, const char *rel, sh_walk_fn fn, void *user) {
    char *dir = *rel ? sh_path_join(root, rel) : sh_strdup(root);
    sh_strv names = SH_STRV_INIT;
    bool go_on = true;
    if (sh_list_dir(dir, SH_LIST_ALL | SH_SORT_NAME, &names)) {
        for (size_t i = 0; go_on && i < names.len; ++i) {
            char *full = sh_path_join(dir, names.items[i]);
            char *sub = *rel ? sh_path_join(rel, names.items[i]) : sh_strdup(names.items[i]);
            const bool link = sh_is_link(full);
            if (link) {
                if (sh_is_file(full)) go_on = fn(sub, full, true, user);  // a link to a file counts; a folder link is not entered
            } else if (sh_is_dir(full)) {
                go_on = walk(root, sub, fn, user);
            } else if (sh_lexists(full)) {
                go_on = fn(sub, full, false, user);
            }
            free(full);
            free(sub);
        }
    }
    sh_strv_free(&names);
    free(dir);
    return go_on;
}

bool sh_walk_files(const char *root, sh_walk_fn fn, void *user) { return walk(root, "", fn, user); }

// ---- changing ----

bool sh_remove_file(const char *path) {
#ifdef _WIN32
    wchar_t *w = sh_wide_path(path);
    SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);  // read-only files (git packs) cannot be deleted otherwise
    const bool ok = DeleteFileW(w) != 0;
    free(w);
    return ok;
#else
    return unlink(path) == 0;
#endif
}

bool sh_remove_tree(const char *path) {
    if (sh_is_link(path)) return sh_remove_link(path);
    if (!sh_lexists(path)) return true;
    if (!sh_is_dir(path)) return sh_remove_file(path);
    sh_strv names = SH_STRV_INIT;
    bool ok = sh_list_dir(path, SH_LIST_ALL, &names);
    for (size_t i = 0; i < names.len; ++i) {
        char *full = sh_path_join(path, names.items[i]);
        if (!sh_remove_tree(full)) ok = false;
        free(full);
    }
    sh_strv_free(&names);
#ifdef _WIN32
    wchar_t *w = sh_wide_path(path);
    SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryW(w)) ok = false;
    free(w);
#else
    if (rmdir(path) != 0) ok = false;
#endif
    return ok;
}

bool sh_rename(const char *from, const char *to) {
    if (sh_lexists(to)) return false;
#ifdef _WIN32
    wchar_t *wf = sh_wide_path(from), *wt = sh_wide_path(to);
    bool ok = false;
    for (int i = 0; i < 10 && !ok; ++i) {  // a scanner may hold a file for a moment
        ok = MoveFileExW(wf, wt, 0) != 0;
        if (!ok && GetLastError() != ERROR_ACCESS_DENIED && GetLastError() != ERROR_SHARING_VIOLATION) break;
        if (!ok) Sleep(50);
    }
    free(wf);
    free(wt);
    return ok;
#else
    return rename(from, to) == 0;
#endif
}

static bool cross_device(void) {
#ifdef _WIN32
    return GetLastError() == ERROR_NOT_SAME_DEVICE;
#else
    return errno == EXDEV;
#endif
}

bool sh_move_dir(const char *from, const char *to) {
    char *parent = sh_path_parent(to);
    sh_mkpath(parent);
    free(parent);
    if (sh_rename(from, to)) return true;
    // another volume: copy, then remove. Never through a link, which would delete what it points to.
    if (!cross_device() || sh_is_link(from)) return false;
    // beside the target first and then renamed, so a crash never leaves half a folder under the real name
    char *dir = sh_path_parent(to);
    char *partial = sh_strfmt("%s/" SH_PARTIAL_PREFIX "%s", dir, sh_path_name(to));
    free(dir);
    sh_remove_tree(partial);
    const bool copied = sh_copy_tree(from, partial) && sh_rename(partial, to);
    if (!copied) sh_remove_tree(partial);
    free(partial);
    return copied && sh_remove_tree(from);
}

bool sh_copy_file(const char *from, const char *to) {
    if (sh_lexists(to)) return false;
#ifdef _WIN32
    wchar_t *wf = sh_wide_path(from), *wt = sh_wide_path(to);
    const bool ok = CopyFileW(wf, wt, TRUE) != 0;
    free(wf);
    free(wt);
    return ok;
#else
    const int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    struct stat st;
    if (fstat(in, &st) != 0) {
        close(in);
        return false;
    }
    const int out = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777);
    if (out < 0) {
        close(in);
        return false;
    }
    char buf[65536];
    bool ok = true;
    for (;;) {
        const ssize_t n = read(in, buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        for (ssize_t done = 0; done < n;) {
            const ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w < 0 && errno == EINTR) continue;
            if (w < 0) {
                ok = false;
                break;
            }
            done += w;
        }
        if (!ok) break;
    }
    fchmod(out, st.st_mode & 07777);
    close(in);
    if (close(out) != 0) ok = false;
    if (!ok) unlink(to);
    return ok;
#endif
}

bool sh_copy_tree(const char *from, const char *to) {
    if (sh_is_link(from)) return false;
    if (!sh_mkpath(to)) return false;
    sh_strv names = SH_STRV_INIT;
    sh_list_dir(from, SH_LIST_ALL, &names);
    bool ok = true;
    for (size_t i = 0; ok && i < names.len; ++i) {
        char *src = sh_path_join(from, names.items[i]);
        char *dst = sh_path_join(to, names.items[i]);
        if (sh_is_link(src)) {
            // links are skipped, never followed
        } else if (sh_is_dir(src)) {
            ok = sh_copy_tree(src, dst);
        } else if (sh_is_file(src)) {
            ok = sh_copy_file(src, dst);
        }  // sockets and FIFOs are skipped too
        free(src);
        free(dst);
    }
    sh_strv_free(&names);
    return ok;
}

bool sh_set_executable(const char *path, bool exec) {
#ifdef _WIN32
    (void)path;
    (void)exec;
    return true;
#else
    return chmod(path, exec ? 0755 : 0644) == 0;
#endif
}

// folders count too: removing a file only changes its folder's time
static void newest(const char *dir, int64_t *t) {
    sh_strv names = SH_STRV_INIT;
    sh_list_dir(dir, SH_LIST_ALL, &names);
    for (size_t i = 0; i < names.len; ++i) {
        char *full = sh_path_join(dir, names.items[i]);
        const int64_t m = sh_mtime_ms(full);
        if (m > *t) *t = m;
        if (!sh_is_link(full) && sh_is_dir(full)) newest(full, t);
        free(full);
    }
    sh_strv_free(&names);
}

int64_t sh_newest_mtime_ms(const char *dir) {
    int64_t t = sh_mtime_ms(dir);
    newest(dir, &t);
    return t;
}

// ---- files written in pieces ----

struct sh_file {
    FILE *f;
    bool failed;
};

sh_file *sh_file_create(const char *path) {
#ifdef _WIN32
    wchar_t *w = sh_wide_path(path);
    FILE *f = _wfopen(w, L"wb");
    free(w);
#else
    FILE *f = fopen(path, "wb" FOPEN_CLOEXEC);
#endif
    if (!f) return NULL;
    sh_file *file = sh_xcalloc(1, sizeof *file);
    file->f = f;
    return file;
}

bool sh_file_write(sh_file *f, const void *data, size_t n) {
    if (!f->failed && n > 0 && fwrite(data, 1, n, f->f) != n) f->failed = true;
    return !f->failed;
}

bool sh_file_close(sh_file *f) {
    if (!f) return false;
    const bool ok = fclose(f->f) == 0 && !f->failed;
    free(f);
    return ok;
}

// ---- lock files ----

struct sh_flock {
#ifdef _WIN32
    HANDLE h;
#else
    int fd;
#endif
};

sh_flock *sh_flock_take(const char *path, int timeout_ms) {
    char *parent = sh_path_parent(path);
    if (*parent) sh_mkpath(parent);
    free(parent);
    const int64_t until = timeout_ms > 0 ? sh_mono_ms() + timeout_ms : 0;
#ifdef _WIN32
    wchar_t *w = sh_wide_path(path);
    HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    for (;;) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        const DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | (timeout_ms < 0 ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
        if (LockFileEx(h, flags, 0, 1, 0, &ov)) break;
        if (timeout_ms < 0 || GetLastError() != ERROR_LOCK_VIOLATION || sh_mono_ms() >= until) {
            CloseHandle(h);
            return NULL;
        }
        sh_sleep_ms(50);
    }
    sh_flock *l = sh_xcalloc(1, sizeof *l);
    l->h = h;
    return l;
#else
    const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return NULL;
    for (;;) {
        if (flock(fd, timeout_ms < 0 ? LOCK_EX : LOCK_EX | LOCK_NB) == 0) break;
        if (errno == EINTR) continue;
        if (timeout_ms < 0 || errno != EWOULDBLOCK || sh_mono_ms() >= until) {
            close(fd);
            return NULL;
        }
        sh_sleep_ms(50);
    }
    sh_flock *l = sh_xcalloc(1, sizeof *l);
    l->fd = fd;
    return l;
#endif
}

void sh_flock_release(sh_flock *l) {
    if (!l) return;
#ifdef _WIN32
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    UnlockFileEx(l->h, 0, 1, 0, &ov);
    CloseHandle(l->h);
#else
    flock(l->fd, LOCK_UN);
    close(l->fd);
#endif
    free(l);
}

static bool add_bytes(const char *rel, const char *full, bool is_link, void *user) {
    (void)rel;
    if (!is_link) *(int64_t *)user += sh_file_size(full);
    return true;
}

int64_t sh_tree_bytes(const char *dir) {
    int64_t total = 0;
    sh_walk_files(dir, add_bytes, &total);
    return total;
}

bool sh_disk_usage(const char *path, int64_t *total, int64_t *free_bytes) {
#ifdef _WIN32
    wchar_t *w = sh_wide_path(path);
    ULARGE_INTEGER avail, all;
    const BOOL ok = GetDiskFreeSpaceExW(w, &avail, &all, NULL);
    free(w);
    if (!ok) return false;
    *total = (int64_t)all.QuadPart;
    *free_bytes = (int64_t)avail.QuadPart;
    return true;
#else
    struct statvfs s;
    if (statvfs(path, &s) != 0) return false;
    *total = (int64_t)s.f_blocks * (int64_t)s.f_frsize;
    *free_bytes = (int64_t)s.f_bavail * (int64_t)s.f_frsize;
    return true;
#endif
}
