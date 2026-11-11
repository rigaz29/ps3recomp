/*
 * ps3recomp - a mod folder over the guest's file system
 *
 * PS3_MODS_ROOT names a host folder laid out like the guest's paths:
 * <root>/dev_hdd0/game/<title>/USRDIR/FILE stands for
 * /dev_hdd0/game/<title>/USRDIR/FILE. A regular file there is used in place of
 * the guest's own, or added where the guest has none. Folders are not merged:
 * a listing shows the guest's folder alone. Unset or empty, nothing changes.
 *
 * All three path translators (sys_fs, cellFs, ppu_fs) ask it first. It is
 * header-only, so each carries its own copy and the test fixtures that link one
 * translator without the others need no new symbol.
 */
#ifndef PS3RECOMP_VFS_MODS_H
#define PS3RECOMP_VFS_MODS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

/* 1, with the host path in out, when the mod folder holds a file for this
 * guest path; 0 to translate it as usual. */
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
    if (!root || !guest || guest[0] != '/' || strstr(guest, ".."))
        return 0;
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
    if (said < 64) {
        said++;
        fprintf(stderr, "[fs] mod '%s' -> '%s'\n", guest, p);
    }
    return 1;
}

#endif /* PS3RECOMP_VFS_MODS_H */
