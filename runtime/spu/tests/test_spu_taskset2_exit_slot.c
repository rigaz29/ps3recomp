/* Taskset2 task_exit_code entry (+0x1980 + 16 * task). The driver inserts the
 * unchanged production functions; guest memory is a plain buffer and the
 * lock-line lock a no-op (single-threaded here).
 *
 * An SPU creating a task writes the entry with state 1, makes the task pending,
 * and then checks the entry: 1 or 0x12, else it halts; then it sets 0x10. The
 * runtime may run the task to its exit before that check. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t* vm_base;
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void spu_coh_notify_write(uint32_t ea) { (void)ea; }
/* PRODUCTION_FUNCTIONS */
static unsigned passed, failed;
#define CHECK(test, message) do { if (test) passed++; else { failed++; fprintf(stderr, "FAIL: %s\n", message); } } while (0)
enum { TASKSET = 0x10000, OTHER = 0x20000 };
static uint8_t* entry(uint32_t ts, uint32_t task) { return vm_base + ts + 0x1980 + 16 * task; }
/* What the creating SPU writes before it makes the task pending. */
static void created(uint32_t ts, uint32_t task)
{
    uint8_t* e = entry(ts, task);
    memset(e, 0, 16);
    e[15] = 1;
}
/* The creator's check after the task is pending: 1 if it would go on. */
static int creator_check(uint32_t ts, uint32_t task)
{
    uint8_t* e = entry(ts, task);
    if (e[15] != 1 && e[15] != 0x12) return 0;
    e[15] |= 0x10;
    return 1;
}
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
int main(void)
{
    vm_base = calloc(1, 0x40000); if (!vm_base) return 2;
    spu_taskset2_register(TASKSET);

    created(TASKSET, 3);
    spu_taskset2_set_exit(TASKSET, 3, 1, 0x11223344u);
    CHECK(entry(TASKSET, 3)[15] == 0x12, "an exit leaves 0x12, as libsre's policy module does");
    CHECK(be32(entry(TASKSET, 3) + 8) == 0x11223344u, "the exit code is at +8");
    CHECK(entry(TASKSET, 3)[14] == 0, "byte 14 stays 0 (non-zero reads as ESRCH in the join)");
    CHECK(creator_check(TASKSET, 3), "a task that exits before its creator's check does not halt it");
    CHECK(entry(TASKSET, 3)[15] == 0x12, "the creator's 0x10 keeps the entry exited");

    created(TASKSET, 4);
    CHECK(creator_check(TASKSET, 4), "the creator's check passes on a running task");
    spu_taskset2_set_exit(TASKSET, 4, 1, 7);
    CHECK(entry(TASKSET, 4)[15] == 0x12 && be32(entry(TASKSET, 4) + 8) == 7,
          "an exit after the check ends in the same entry");

    memset(entry(TASKSET, 5), 0xA5, 16);
    spu_taskset2_set_exit(TASKSET, 5, 0, 0);
    { int zero = 1; for (int i = 0; i < 16; i++) zero &= entry(TASKSET, 5)[i] == 0;
      CHECK(zero, "a fresh entry for a recycled id is all zero"); }

    memset(entry(OTHER, 1), 0xA5, 16);
    spu_taskset2_set_exit(OTHER, 1, 1, 0);
    CHECK(entry(OTHER, 1)[15] == 0xA5, "a taskset not registered as Taskset2 is left alone");
    memset(vm_base + TASKSET + 0x1980 + 16 * 128, 0xA5, 16);
    spu_taskset2_set_exit(TASKSET, 128, 1, 0);
    CHECK(vm_base[TASKSET + 0x1980 + 16 * 128 + 15] == 0xA5, "task ids past 127 write nothing");

    free(vm_base);
    printf("taskset2 exit-code entry: %u passed, %u failed\n", passed, failed);
    return failed ? 1 : 0;
}
