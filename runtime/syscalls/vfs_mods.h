/*
 * ps3recomp - mod folders over the guest's file system
 *
 * PS3_MODS_DIRS names host folders, '|' between them (a character no Windows
 * name has), each standing for the game's USRDIR as the guest reads it:
 * /dev_hdd0/game/<title>/USRDIR/FILE (the title PS3_TITLE names, else any
 * title id, but never a game data folder such as <title>-DATA) and
 * /dev_bdvd/PS3_GAME/USRDIR/FILE are <dir>/FILE, the first folder that has it
 * winning. So one folder serves a disc and its update, and every release of a
 * title. Names match without
 * regard to case, as Windows has them. Each folder's files are listed once, at
 * the first lookup: a lookup reads nothing from the disk, and a file added
 * while the game runs is seen at the next start.
 *
 * PS3_MODS_ROOT, asked after them, names a host folder laid out like the
 * guest's paths: <root>/dev_hdd0/game/<title>/USRDIR/FILE stands for
 * /dev_hdd0/game/<title>/USRDIR/FILE.
 *
 * Either way a regular file is used in place of the guest's own, or added
 * where the guest has none. Folders are not merged: a listing shows the
 * guest's folder alone. Unset or empty, nothing changes.
 *
 * All three path translators (sys_fs, cellFs, ppu_fs) ask it first. It is
 * header-only, so each carries its own copy (its own list of the folders'
 * files) and the test fixtures that link one translator without the others
 * need no new symbol.
 */
#ifndef PS3RECOMP_VFS_MODS_H
#define PS3RECOMP_VFS_MODS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#include <intrin.h>
#else
#include <dirent.h>
#include <sched.h>
#endif

/* A file of PS3_MODS_DIRS: its name under the folder, in lower case with '/',
 * and its host path; order says which folder came first. */
typedef struct { char* key; char* host; unsigned order; } ps3_vfs_mod_file;
typedef struct { ps3_vfs_mod_file* v; size_t n, cap; unsigned next; } ps3_vfs_mod_list;

static inline char ps3_vfs_mod_lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c; }

static inline void ps3_vfs_mod_add(ps3_vfs_mod_list* l, const char* rel, const char* host)
{
    if (l->n == l->cap) {
        const size_t cap = l->cap ? l->cap * 2 : 256;
        ps3_vfs_mod_file* v = (ps3_vfs_mod_file*)realloc(l->v, cap * sizeof *v);
        if (!v) return;
        l->v = v;
        l->cap = cap;
    }
    const size_t kn = strlen(rel), hn = strlen(host);
    char* k = (char*)malloc(kn + 1);
    char* h = (char*)malloc(hn + 1);
    if (!k || !h) { free(k); free(h); return; }
    for (size_t i = 0; i <= kn; i++) k[i] = rel[i] == '\\' ? '/' : ps3_vfs_mod_lower(rel[i]);
    memcpy(h, host, hn + 1);
    l->v[l->n].key = k;
    l->v[l->n].host = h;
    l->v[l->n].order = l->next++;
    l->n++;
}

/* Every file under dir, named rel and down from it. Not deeper than 32
 * folders, nor more than 500,000 files: a folder that links back to itself
 * ends there. */
static inline void ps3_vfs_mod_walk(ps3_vfs_mod_list* l, const char* dir, const char* rel, int depth)
{
    if (depth > 32 || l->n >= 500000) return;
    char host[1024], r[1024];
#ifdef _WIN32
    char pat[1024];
    if ((size_t)snprintf(pat, sizeof pat, "%s/*", dir) >= sizeof pat) return;
    struct __finddata64_t fd;
    const intptr_t h = _findfirst64(pat, &fd);
    if (h == -1) return;
    do {
        const char* name = fd.name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if ((size_t)snprintf(host, sizeof host, "%s/%s", dir, name) >= sizeof host) continue;
        if ((size_t)(rel[0] ? snprintf(r, sizeof r, "%s/%s", rel, name) : snprintf(r, sizeof r, "%s", name)) >= sizeof r)
            continue;
        if (fd.attrib & _A_SUBDIR) ps3_vfs_mod_walk(l, host, r, depth + 1);
        else ps3_vfs_mod_add(l, r, host);
    } while (_findnext64(h, &fd) == 0);
    _findclose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        const char* name = e->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if ((size_t)snprintf(host, sizeof host, "%s/%s", dir, name) >= sizeof host) continue;
        if ((size_t)(rel[0] ? snprintf(r, sizeof r, "%s/%s", rel, name) : snprintf(r, sizeof r, "%s", name)) >= sizeof r)
            continue;
        struct stat st;
        if (stat(host, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) ps3_vfs_mod_walk(l, host, r, depth + 1);
        else if (S_ISREG(st.st_mode)) ps3_vfs_mod_add(l, r, host);
    }
    closedir(d);
#endif
}

static inline int ps3_vfs_mod_cmp(const void* a, const void* b)
{
    const ps3_vfs_mod_file* x = (const ps3_vfs_mod_file*)a;
    const ps3_vfs_mod_file* y = (const ps3_vfs_mod_file*)b;
    const int c = strcmp(x->key, y->key);
    return c ? c : x->order < y->order ? -1 : x->order > y->order;
}

/* PS3_MODS_DIRS's files, sorted by name, the first folder's kept where two
 * have one name. */
static inline void ps3_vfs_mod_list_make(ps3_vfs_mod_list* l)
{
    const char* p = getenv("PS3_MODS_DIRS");
    if (!p) return;
    while (*p) {
        const char* e = strchr(p, '|');
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n && n < 1000) {
            char d[1024];
            memcpy(d, p, n);
            d[n] = 0;
            for (char* c = d; *c; c++) if (*c == '\\') *c = '/';
            size_t k = n;
            while (k > 1 && d[k - 1] == '/') d[--k] = 0;
            ps3_vfs_mod_walk(l, d, "", 0);
        }
        p += n;
        if (*p == '|') p++;
    }
    if (l->n < 2) return;
    qsort(l->v, l->n, sizeof *l->v, ps3_vfs_mod_cmp);
    size_t w = 1;
    for (size_t i = 1; i < l->n; i++) {
        if (!strcmp(l->v[i].key, l->v[w - 1].key)) { free(l->v[i].key); free(l->v[i].host); continue; }
        l->v[w++] = l->v[i];
    }
    l->n = w;
}

/* Whether /dev_hdd0/game/<id> is the game's own folder: the title PS3_TITLE
 * names, else one named as titles are (four capitals, five digits). Not a
 * game data folder such as <id>-DATA, which has a USRDIR of its own. */
static inline int ps3_vfs_mod_title(const char* id, size_t n)
{
    static const char* title = NULL;
    static int init = 0;
    if (!init) {
        title = getenv("PS3_TITLE");
        if (title && !*title) title = NULL;
        init = 1;
    }
    if (title) return strlen(title) == n && !strncmp(id, title, n);
    if (n != 9) return 0;
    for (size_t i = 0; i < 9; i++)
        if (i < 4 ? !(id[i] >= 'A' && id[i] <= 'Z') : !(id[i] >= '0' && id[i] <= '9')) return 0;
    return 1;
}

/* The part of a guest path under the game's USRDIR, or NULL. */
static inline const char* ps3_vfs_mod_usrdir(const char* guest)
{
    static const char disc[] = "/dev_bdvd/PS3_GAME/USRDIR/", game[] = "/dev_hdd0/game/";
    if (!strncmp(guest, disc, sizeof disc - 1)) return guest + sizeof disc - 1;
    if (strncmp(guest, game, sizeof game - 1)) return NULL;
    const char* id = guest + sizeof game - 1;
    const char* s = strchr(id, '/');
    if (!s || !ps3_vfs_mod_title(id, (size_t)(s - id))) return NULL;
    return !strncmp(s, "/USRDIR/", 8) ? s + 8 : NULL;
}

/* 1, with the host path in out, when a folder of PS3_MODS_DIRS has this guest
 * path's file. */
static inline int ps3_vfs_mod_dirs_path(const char* guest, char* out, size_t cap)
{
    static ps3_vfs_mod_list l;
    static volatile long made;   /* 0 not yet, 1 being made, 2 made */
#ifdef _WIN32
    if (made != 2) {
        if (_InterlockedCompareExchange(&made, 1, 0) == 0) {
            ps3_vfs_mod_list_make(&l);
            _InterlockedExchange(&made, 2);
        } else {
            while (made != 2) {
#if defined(_M_X64) || defined(_M_IX86)
                _mm_pause();
#endif
            }
        }
    }
#else
    if (__atomic_load_n(&made, __ATOMIC_ACQUIRE) != 2) {
        long none = 0;
        if (__atomic_compare_exchange_n(&made, &none, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            ps3_vfs_mod_list_make(&l);
            __atomic_store_n(&made, 2, __ATOMIC_RELEASE);
        } else {
            while (__atomic_load_n(&made, __ATOMIC_ACQUIRE) != 2) sched_yield();
        }
    }
#endif
    if (!l.n) return 0;
    const char* rel = ps3_vfs_mod_usrdir(guest);
    if (!rel || !*rel) return 0;
    char key[1024];
    const size_t n = strlen(rel);
    if (n >= sizeof key) return 0;
    for (size_t i = 0; i <= n; i++) key[i] = ps3_vfs_mod_lower(rel[i]);
    size_t lo = 0, hi = l.n;
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        const int c = strcmp(l.v[mid].key, key);
        if (!c) {
            const size_t hn = strlen(l.v[mid].host);
            if (hn >= cap) return 0;
            memcpy(out, l.v[mid].host, hn + 1);
            return 1;
        }
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

/* 1, with the host path in out, when a mod folder holds a file for this guest
 * path; 0 to translate it as usual (out untouched). */
static inline int ps3_vfs_mod_path(const char* guest, char* out, size_t cap)
{
    static const char* root = NULL;
    static int init = 0, said = 0;
    if (!init) {
        root = getenv("PS3_MODS_ROOT");
        if (root && !*root) root = NULL;
        init = 1;
    }
    /* "..": a guest path never needs it, and it must not reach outside the folder. */
    if (!guest || guest[0] != '/' || strstr(guest, ".."))
        return 0;
    int got = ps3_vfs_mod_dirs_path(guest, out, cap);
    if (!got && root) {
        char p[1024];
        const int n = snprintf(p, sizeof p, "%s/%s", root, guest + 1);
        if (n < 0 || (size_t)n >= sizeof p || (size_t)n >= cap)
            return 0;
        for (char* c = p; *c; c++)
            if (*c == '\\') *c = '/';
#ifdef _WIN32
        struct _stat64 st;
        if (_stat64(p, &st) != 0 || (st.st_mode & _S_IFMT) != _S_IFREG)
            return 0;
#else
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
            return 0;
#endif
        memcpy(out, p, (size_t)n + 1);
        got = 1;
    }
    if (got && said < 64) {
        said++;
        fprintf(stderr, "[fs] mod '%s' -> '%s'\n", guest, out);
    }
    return got;
}

#endif /* PS3RECOMP_VFS_MODS_H */
