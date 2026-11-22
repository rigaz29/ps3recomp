/*
 * test_net_guard - PS3_NET_HOSTS names and PS3_NET_LOOPBACK_ONLY.
 *
 *   clang-cl /I include /Fe:test_net_guard.exe libs/network/tests/test_net_guard.c
 *   cc -std=gnu17 -I include -o test_net_guard libs/network/tests/test_net_guard.c
 */
#include "../net_guard.h"

#include <assert.h>

static void put(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static const char* map(const char* name)
{
    static char ip[16];
    return net_guard_host(name, ip) ? ip : NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Unset: every name goes to DNS as before. */
    put("PS3_NET_HOSTS", NULL);
    assert(map("gowa.dcds.online.scea.com") == NULL);

    /* The first entry that matches wins: the exact name, then "*.suffix" (any case), then "*". */
    put("PS3_NET_HOSTS", "a.example.com=10.0.0.1; *.scea.com=127.0.0.1;*=127.0.0.2");
    assert(strcmp(map("a.example.com"), "10.0.0.1") == 0);
    assert(strcmp(map("A.Example.COM"), "10.0.0.1") == 0);
    assert(strcmp(map("gowa.dcds.online.scea.com"), "127.0.0.1") == 0);
    assert(strcmp(map("IPMAPPING-PROD.ONLINE.SCEA.COM"), "127.0.0.1") == 0);
    assert(strcmp(map("scea.com"), "127.0.0.2") == 0);           /* ".scea.com" does not end "scea.com" */
    assert(strcmp(map("b.example.com"), "127.0.0.2") == 0);
    assert(map("") == NULL);

    /* No catch-all: other names resolve as before. */
    put("PS3_NET_HOSTS", "*.scea.com=127.0.0.1");
    assert(map("example.org") == NULL);
    assert(strcmp(map("x.scea.com"), "127.0.0.1") == 0);

    /* Loopback only: 127.0.0.0/8 passes, anything else is refused every time (reported once). */
    put("PS3_NET_LOOPBACK_ONLY", "1");
    const unsigned char lo[4] = { 127, 0, 0, 1 }, lo2[4] = { 127, 9, 9, 9 }, far[4] = { 8, 8, 8, 8 };
    assert(!net_guard_blocked(lo, 80, "connect"));
    assert(!net_guard_blocked(lo2, 3658, "sendto"));
    assert(net_guard_blocked(far, 443, "connect"));
    assert(net_guard_blocked(far, 443, "connect"));
    assert(net_guard_blocked(far, 53, "sendto"));

    puts("test_net_guard: OK");
    return 0;
}
