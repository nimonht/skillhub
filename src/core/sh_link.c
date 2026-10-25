#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE
#endif

#include "sh_link.h"

#include <stdlib.h>
#include <string.h>

#include "sh_fs.h"
#include "sh_path.h"
#include "sh_str.h"

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2  // Windows 10 1703; older SDK headers lack it
#endif
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32

// REPARSE_DATA_BUFFER (only declared in the driver kit): the tag, then the name offsets and lengths of a symlink or junction
typedef struct {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    USHORT sub_offset;
    USHORT sub_length;
    USHORT print_offset;
    USHORT print_length;
    char data[1];  // junction: the path names; symlink: 4 bytes of flags, then the path names
} reparse_buffer;

bool sh_is_link(const char *path) {
    wchar_t *w = sh_wide_path(path);
    const DWORD a = GetFileAttributesW(w);
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

char *sh_link_target(const char *path) {
    if (!sh_is_link(path)) return NULL;
    // read the reparse point itself: junctions are not resolved by the usual APIs
    wchar_t *w = sh_wide_path(path);
    HANDLE h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    static _Alignas(8) char buf[16 * 1024];
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, sizeof buf, &got, NULL);
    CloseHandle(h);
    if (!ok) return NULL;
    const reparse_buffer *r = (const reparse_buffer *)buf;
    const char *names;
    bool relative = false;
    if (r->tag == IO_REPARSE_TAG_MOUNT_POINT) {
        names = r->data;
    } else if (r->tag == IO_REPARSE_TAG_SYMLINK) {
        names = r->data + 4;  // after the flags
        relative = (*(const ULONG *)r->data & 1) != 0;  // SYMLINK_FLAG_RELATIVE
    } else {
        return NULL;
    }
    const size_t chars = r->sub_length / sizeof(wchar_t);
    wchar_t *t = sh_xmalloc((chars + 1) * sizeof *t);
    memcpy(t, names + r->sub_offset, chars * sizeof *t);
    t[chars] = 0;
    const wchar_t *start = wcsncmp(t, L"\\??\\", 4) == 0 ? t + 4 : t;  // NT namespace prefix
    char *target = sh_narrow(start);
    free(t);
    char *out;
    if (relative) {
        char *parent = sh_path_parent(path);
        char *joined = sh_path_join(parent, target);
        out = sh_path_clean(joined);
        free(parent);
        free(joined);
    } else {
        out = sh_path_clean(target);
    }
    free(target);
    return out;
}

bool sh_points_to(const char *link_path, const char *dir) {
    char *t = sh_link_target(link_path);
    if (!t) return false;
    char *d = sh_path_clean(dir);
    bool same = sh_path_eq(t, d);
    if (!same) {  // 8.3 short names, other spellings: compare what both really are
        char *a = sh_canonical(link_path), *b = sh_canonical(dir);
        same = a && b && sh_path_eq(a, b);
        free(a);
        free(b);
    }
    free(t);
    free(d);
    return same;
}

static bool make_junction(const char *link_path, const char *abs_target) {
    if (!sh_is_dir(abs_target)) return false;
    wchar_t *wl = sh_wide_path(link_path);
    if (!CreateDirectoryW(wl, NULL)) {
        free(wl);
        return false;
    }
    char *native = sh_strdup(abs_target);
    for (char *c = native; *c; ++c)
        if (*c == '/') *c = '\\';
    char *sub_utf8 = sh_strfmt("\\??\\%s", native);
    wchar_t *sub = sh_wide(sub_utf8), *print = sh_wide(native);
    free(sub_utf8);
    free(native);
    const USHORT sub_bytes = (USHORT)(wcslen(sub) * sizeof(wchar_t)), print_bytes = (USHORT)(wcslen(print) * sizeof(wchar_t));
    const DWORD header = 8, fields = 8;
    const DWORD size = header + fields + sub_bytes + (DWORD)sizeof(wchar_t) + print_bytes + (DWORD)sizeof(wchar_t);
    reparse_buffer *r = sh_xcalloc(1, size + 8);
    r->tag = IO_REPARSE_TAG_MOUNT_POINT;
    r->data_length = (USHORT)(size - header);
    r->sub_offset = 0;
    r->sub_length = sub_bytes;
    r->print_offset = (USHORT)(sub_bytes + sizeof(wchar_t));
    r->print_length = print_bytes;
    memcpy(r->data, sub, sub_bytes);
    memcpy(r->data + r->print_offset, print, print_bytes);
    free(sub);
    free(print);
    bool ok = false;
    HANDLE h = CreateFileW(wl, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD ret = 0;
        ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, r, size, NULL, 0, &ret, NULL) != 0;
        CloseHandle(h);
    }
    free(r);
    if (!ok) RemoveDirectoryW(wl);
    free(wl);
    return ok;
}

bool sh_make_dir_link(const char *link_path, const char *target) {
    if (!sh_env_is_set("SKILLHUB_NO_SYMLINKS")) {  // set by tests: behave like an account that may not create symlinks
        wchar_t *wl = sh_wide_path(link_path), *wt = sh_wide(target);
        for (wchar_t *c = wt; *c; ++c)
            if (*c == L'/') *c = L'\\';
        const BOOL ok = CreateSymbolicLinkW(wl, wt, SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
        free(wl);
        free(wt);
        if (ok) return true;
    }
    char *abs;
    if (sh_path_is_absolute(target)) {
        abs = sh_path_clean(target);
    } else {
        char *parent = sh_path_parent(link_path);
        char *joined = sh_path_join(parent, target);
        abs = sh_path_clean(joined);
        free(parent);
        free(joined);
    }
    const bool ok = make_junction(link_path, abs);
    free(abs);
    return ok;
}

bool sh_remove_link(const char *link_path) {
    if (!sh_is_link(link_path)) return false;
    wchar_t *w = sh_wide_path(link_path);
    const bool ok = RemoveDirectoryW(w) || DeleteFileW(w);
    free(w);
    return ok;
}

bool sh_can_make_dir_links(void) {
    static int known = -1;
    if (known >= 0) return known == 1;
    char *tmpdir = NULL;
    wchar_t tmp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    if (n > 0) {
        char *t = sh_narrow(tmp);
        tmpdir = sh_path_clean(t);
        free(t);
    }
    char *dir = sh_strfmt("%s/skillhub-probe-%lu", tmpdir ? tmpdir : "C:/Windows/Temp", (unsigned long)GetCurrentProcessId());
    char *t = sh_path_join(dir, "t"), *l = sh_path_join(dir, "l");
    sh_mkpath(t);
    const bool made = sh_make_dir_link(l, "t");
    if (made) sh_remove_link(l);
    sh_remove_tree(dir);
    free(t);
    free(l);
    free(dir);
    free(tmpdir);
    known = made ? 1 : 0;
    return made;
}

#else  // POSIX

bool sh_is_link(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

char *sh_link_target(const char *path) {
    if (!sh_is_link(path)) return NULL;
    sh_buf b = SH_BUF_INIT;
    size_t size = 256;
    for (;;) {
        char *buf = sh_xmalloc(size);
        const ssize_t n = readlink(path, buf, size);
        if (n < 0) {
            free(buf);
            return NULL;
        }
        if ((size_t)n < size) {
            sh_buf_append(&b, buf, (size_t)n);
            free(buf);
            break;
        }
        free(buf);
        size *= 2;
    }
    char *raw = sh_buf_steal(&b);
    char *out;
    if (raw[0] == '/') {
        out = sh_path_clean(raw);
    } else {
        char *parent = sh_path_parent(path);
        char *joined = sh_path_join(parent, raw);
        out = sh_path_clean(joined);
        free(parent);
        free(joined);
    }
    free(raw);
    return out;
}

bool sh_points_to(const char *link_path, const char *dir) {
    if (!sh_is_link(link_path) || !sh_exists(link_path)) return false;
    char *t = sh_link_target(link_path);
    char *d = sh_path_clean(dir);
    bool same = t && strcmp(t, d) == 0;  // the usual case, without resolving every link of both paths
    if (!same) {
        char *a = sh_canonical(link_path), *b = sh_canonical(dir);
        same = a && b && strcmp(a, b) == 0;
        free(a);
        free(b);
    }
    free(t);
    free(d);
    return same;
}

bool sh_make_dir_link(const char *link_path, const char *target) { return symlink(target, link_path) == 0; }

bool sh_remove_link(const char *link_path) { return sh_is_link(link_path) && unlink(link_path) == 0; }

bool sh_can_make_dir_links(void) { return true; }

#endif
