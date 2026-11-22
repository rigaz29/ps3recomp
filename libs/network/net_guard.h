/*
 * ps3recomp - where a title's names and packets may go (header-only, used by
 * sysNet.c and cellHttp.c)
 *
 * PS3_NET_HOSTS="name=a.b.c.d;*.example.com=a.b.c.d;*=127.0.0.1": a host name
 * the title looks up (gethostbyname, cellHttp) gets the address of the first
 * entry that matches it -- the exact name, "*.suffix" for every name under
 * it, or "*" for every name -- instead of a DNS answer. How a title's own
 * servers are pointed at a local stand-in; names no entry matches resolve as
 * before.
 *
 * PS3_NET_LOOPBACK_ONLY=1: connects, datagrams and HTTP requests to anything
 * outside 127.0.0.0/8 fail as unreachable, each destination reported once.
 * For runs that must reach nothing but local stand-ins.
 */
#ifndef NET_GUARD_H
#define NET_GUARD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static inline int net_guard_same(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

/* 1 and the entry's address (dotted, NUL-terminated) in ip when an entry of
 * PS3_NET_HOSTS matches name; 0 when none does. */
static inline int net_guard_host(const char* name, char ip[16])
{
    const char* e = getenv("PS3_NET_HOSTS");
    if (!e || !*e || !name || !*name) return 0;
    const size_t nl = strlen(name);
    while (*e) {
        const char* end = strchr(e, ';');
        if (!end) end = e + strlen(e);
        while (e < end && *e == ' ') e++;
        const char* eq = (const char*)memchr(e, '=', (size_t)(end - e));
        if (eq) {
            const size_t pl = (size_t)(eq - e);
            int match;
            if (pl == 1 && e[0] == '*')
                match = 1;
            else if (pl > 2 && e[0] == '*' && e[1] == '.')   /* "*.suffix": ".suffix" ends the name */
                match = nl >= pl - 1 && net_guard_same(name + nl - (pl - 1), e + 1, pl - 1);
            else
                match = pl == nl && net_guard_same(name, e, nl);
            if (match) {
                size_t il = (size_t)(end - (eq + 1));
                if (il > 15) il = 15;
                memcpy(ip, eq + 1, il);
                ip[il] = '\0';
                return 1;
            }
        }
        e = *end ? end + 1 : end;
    }
    return 0;
}

/* 1 (and a report, once per destination) when PS3_NET_LOOPBACK_ONLY is set
 * and addr -- four bytes in network order -- is outside 127.0.0.0/8. */
static inline int net_guard_blocked(const void* addr, unsigned port, const char* what)
{
    static int on = -1;
    static unsigned seen[32];
    static int nseen;
    if (on < 0) on = getenv("PS3_NET_LOOPBACK_ONLY") ? 1 : 0;
    if (!on) return 0;
    const unsigned char* b = (const unsigned char*)addr;
    if (b[0] == 127) return 0;
    const unsigned key = ((unsigned)b[0] << 24 | (unsigned)b[1] << 16 | (unsigned)b[2] << 8 | b[3]) ^ (port * 2654435761u);
    for (int i = 0; i < nseen; i++)
        if (seen[i] == key) return 1;
    if (nseen < 32) seen[nseen++] = key;
    printf("[net] %s to %u.%u.%u.%u:%u blocked (PS3_NET_LOOPBACK_ONLY)\n", what, b[0], b[1], b[2], b[3], port);
    return 1;
}

#endif /* NET_GUARD_H */
