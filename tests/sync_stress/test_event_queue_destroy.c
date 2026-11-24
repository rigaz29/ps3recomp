/* sys_event_queue_destroy wakes a thread blocked in sys_event_queue_receive on that queue: the receive returns
 * CELL_ECANCELED, as lv2 does for a queue destroyed with SYS_EVENT_QUEUE_DESTROY_FORCE. It used to delete the queue's
 * lock under the sleeping thread and wake nobody: God of War: Ascension's network client (brbClient) slept on for
 * good as the game left multiplayer, and the game's join of it never returned. A receive that began on the queue
 * before it was destroyed does not go on waiting on a new queue made in its slot either, and the slot works again. */
#include "runtime/platform/win32_compat.h"
#include "runtime/syscalls/sys_event.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t* vm_base;
static uint32_t qid;

/* sys_event.c's SPU and diagnostic paths, never taken here (as main.c has them). */
size_t spu_elf_image_size(const uint8_t* image, size_t max_avail) { (void)image; (void)max_avail; return 0; }
int spu_workload_dispatch_async(const uint8_t* image, uint32_t image_size, uint32_t args_ea)
{
    (void)image; (void)image_size; (void)args_ea;
    return 0;
}
void ppu_dump_guest_stack(ppu_context* ctx, const char* tag) { (void)ctx; (void)tag; }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

static int32_t call4(int64_t (*f)(ppu_context*), ppu_context* c, uint64_t a, uint64_t b, uint64_t d, uint64_t e)
{
    c->gpr[3] = a; c->gpr[4] = b; c->gpr[5] = d; c->gpr[6] = e;
    return (int32_t)f(c);
}

static uint32_t be_read(unsigned off)
{
    const uint8_t* p = vm_base + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static volatile LONG s_rc = 1, s_done;

/* Thread 2: a receive with no timeout on the queue, as the title's client waits for its next message. */
static DWORD WINAPI receiver(void* raw)
{
    (void)raw;
    ppu_context c = {0};
    c.thread_id = 2;
    InterlockedExchange(&s_rc, call4(sys_event_queue_receive, &c, qid, 0x300, 0, 0));
    InterlockedExchange(&s_done, 1);
    return 0;
}

int main(void)
{
    vm_base = calloc(1, 65536);
    ppu_context c = {0};
    c.thread_id = 1;
    vm_base[0x103] = 1;    /* attribute: protocol FIFO */
    vm_base[0x107] = 1;    /* type: a PPU queue */
    CHECK(call4(sys_event_queue_create, &c, 0x200, 0x100, 0x6272620001ull, 8) == CELL_OK);
    qid = be_read(0x200);
    CHECK(qid != 0);

    /* 1. A receive asleep on the queue is woken by its destroy, with CELL_ECANCELED. */
    HANDLE t = CreateThread(NULL, 0, receiver, NULL, 0, NULL);
    Sleep(200);
    CHECK(!s_done);
    CHECK(call4(sys_event_queue_destroy, &c, qid, 1 /* SYS_EVENT_QUEUE_DESTROY_FORCE */, 0, 0) == CELL_OK);
    CHECK(WaitForSingleObject(t, 3000) == WAIT_OBJECT_0);
    CHECK(s_done);
    CHECK(s_rc == (int32_t)CELL_ECANCELED);
    CloseHandle(t);

    /* 2. The slot made into a new queue works: a timed receive on it times out, and it is destroyed again. */
    CHECK(call4(sys_event_queue_create, &c, 0x200, 0x100, 0x6272620002ull, 8) == CELL_OK);
    qid = be_read(0x200);
    CHECK(qid != 0);
    CHECK(call4(sys_event_queue_receive, &c, qid, 0x300, 20000, 0) == (int32_t)CELL_ETIMEDOUT);
    CHECK(call4(sys_event_queue_destroy, &c, qid, 0, 0, 0) == CELL_OK);

    /* 3. A receive on the destroyed queue's id fails at once. */
    CHECK(call4(sys_event_queue_receive, &c, qid, 0x300, 0, 0) == (int32_t)CELL_ESRCH);

    if (failures) {
        fprintf(stderr, "event queue destroy: %d failure(s)\n", failures);
        return 1;
    }
    printf("event queue destroy: OK\n");
    return 0;
}
