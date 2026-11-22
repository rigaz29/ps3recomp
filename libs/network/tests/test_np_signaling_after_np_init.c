/*
 * test_np_signaling_after_np_init - sceNpSignaling contexts follow sceNpInit.
 *
 * The firmware exports no sceNpSignalingInit; titles call sceNpInit and then
 * sceNpSignalingCreateCtx (God of War: Ascension's SAM transport does, and
 * stopped with SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED). A context must be
 * creatable between sceNpInit and sceNpTerm, refused before and after, and
 * still creatable after the runtime's own sceNpSignalingInit.
 *
 * Built with test_np_basic_after_np_init_np.c (sceNp.c and the runtime
 * symbols) as its second file:
 *   cc -std=gnu17 -I include -o test_np_signaling_after_np_init \
 *      libs/network/tests/test_np_signaling_after_np_init.c \
 *      libs/network/tests/test_np_basic_after_np_init_np.c
 * Driven by tools/test_np_signaling_after_np_init.py.
 */
#include "../sceNpSignaling.c"
#include <stdlib.h>

s32 sceNpInit(u32 poolSize, void* poolPtr);
s32 sceNpTerm(void);

#define NOTINIT ((s32)SCE_NP_SIGNALING_ERROR_NOT_INITIALIZED)
#define CTX_EA 0x1000u                                   /* guest word the context id goes to */
#define CTXID ((SceNpSignalingCtxId*)(uintptr_t)CTX_EA)

static int fails = 0;
#define CHECK(expr, want, what) do { s32 got_ = (s32)(expr); int ok_ = got_ == (s32)(want);              \
        printf("[%s] %s: 0x%08X (want 0x%08X)\n", ok_ ? "PASS" : "FAIL", what, (u32)got_, (u32)(want)); \
        if (!ok_) fails++; } while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = calloc(1, 65536);
    if (!vm_base) return 2;

    CHECK(sceNpSignalingCreateCtx(NULL, NULL, NULL, CTXID), NOTINIT, "before sceNpInit: CreateCtx");

    CHECK(sceNpInit(131072, (void*)0x2000), CELL_OK, "sceNpInit");
    vm_write32(CTX_EA, 0xFFFFFFFFu);
    CHECK(sceNpSignalingCreateCtx(NULL, NULL, NULL, CTXID), CELL_OK, "after sceNpInit: CreateCtx");
    u32 id = vm_read32(CTX_EA);
    CHECK(id < SCE_NP_SIGNALING_CTX_MAX, 1, "after sceNpInit: context id written");
    CHECK(sceNpSignalingCreateCtx(NULL, NULL, NULL, NULL), SCE_NP_SIGNALING_ERROR_INVALID_ARGUMENT,
          "after sceNpInit: CreateCtx without a place for the id");
    CHECK(sceNpSignalingDestroyCtx(id), CELL_OK, "after sceNpInit: DestroyCtx");

    CHECK(sceNpTerm(), CELL_OK, "sceNpTerm");
    CHECK(sceNpSignalingCreateCtx(NULL, NULL, NULL, CTXID), NOTINIT, "after sceNpTerm: CreateCtx");

    CHECK(sceNpSignalingInit(), CELL_OK, "sceNpSignalingInit alone");
    CHECK(sceNpSignalingCreateCtx(NULL, NULL, NULL, CTXID), CELL_OK, "sceNpSignalingInit alone: CreateCtx");
    CHECK(sceNpSignalingTerm(), CELL_OK, "sceNpSignalingTerm");
    CHECK(sceNpSignalingCreateCtx(NULL, NULL, NULL, CTXID), NOTINIT, "after sceNpSignalingTerm: CreateCtx");

    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
