/*
 * ps3recomp - the system's own questions over the title's picture (see sys_dialog.h)
 */
#include "sys_dialog.h"
#include "ps3emu/ps3_settings.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static unsigned long long now_ms(void) { return GetTickCount64(); }
static void sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static unsigned long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}
static void sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}
#endif

/* The pad's buttons as cellPad packs them: (DIGITAL2 << 8) | DIGITAL1. */
#define BTN_LEFT   0x0080u
#define BTN_RIGHT  0x0020u
#define BTN_CIRCLE 0x2000u
#define BTN_CROSS  0x4000u
#define SETTLE_MS  250           /* nothing is taken sooner after a question comes up */

static volatile char s_lock;
static void lock(void)   { while (__atomic_test_and_set(&s_lock, __ATOMIC_ACQUIRE)) sleep_ms(0); }
static void unlock(void) { __atomic_clear(&s_lock, __ATOMIC_RELEASE); }

static PS3SysDialog s_d;         /* the question up, under the lock */
static int s_open;               /* a question is up */
static int s_answer = -1;        /* the choice taken, -1 none yet */
static int s_cancel;             /* the choice going back takes */
static int s_armed;              /* what was held as the question came up has been let go */
static uint32_t s_prev;          /* the last report's buttons; all ones before the first */
static int s_prev_dir;           /* the stick's side in the last report: -1, 0, 1 */
static unsigned s_rev;           /* bumped at each change of what is drawn */
static unsigned long long s_opened;
static int s_drawn;              /* something draws the questions */
static int s_asking;             /* a question is being asked: the next waits */
static int s_hold;               /* answered, but the pad is still held: the title gets it once it is let go */

void ps3_sysdlg_drawn(void) { s_drawn = 1; }

/* Up, or just answered with the pad still held: the button that answered is the system's, and would otherwise
 * reach the title the moment the question went (a Back held a moment longer took God of War: Ascension's profiles
 * screen back to its main menu). */
int ps3_sysdlg_active(void)
{
    return __atomic_load_n(&s_open, __ATOMIC_ACQUIRE) || __atomic_load_n(&s_hold, __ATOMIC_ACQUIRE);
}

unsigned ps3_sysdlg_state(void)
{
    lock();
    const unsigned st = s_open ? (s_rev << 1) | 1u : 0u;
    unlock();
    return st;
}

int ps3_sysdlg_view(PS3SysDialog* out)
{
    lock();
    const int open = s_open;
    if (open) memcpy(out, &s_d, sizeof *out);
    unlock();
    return open;
}

static int enter_is_circle(void)
{
    return ps3_settings_ieq(ps3_settings_str("input", "enter_button", NULL, "cross"), "circle");
}

int ps3_sysdlg_ask(const PS3SysDialog* d, int cancel)
{
    if (!s_drawn) {
        printf("[sysdlg] nothing draws a question here: '%s' answered '%s'\n",
               d->nlines > 0 ? d->line[d->nlines - 1] : d->title, d->choice[cancel ? 1 : 0]);
        return cancel;
    }
    while (__atomic_exchange_n(&s_asking, 1, __ATOMIC_ACQUIRE)) sleep_ms(10);
    lock();
    memcpy(&s_d, d, sizeof s_d);
    s_d.enter_circle = enter_is_circle();
    if (s_d.focus != 0 && s_d.focus != 1) s_d.focus = cancel;
    s_cancel = cancel;
    s_answer = -1;
    s_armed = 0;
    s_prev = 0xFFFFFFFFu;
    s_prev_dir = 0;
    s_opened = now_ms();
    s_rev++;
    __atomic_store_n(&s_open, 1, __ATOMIC_RELEASE);
    unlock();
    printf("[sysdlg] asking: %s\n", d->nlines > 0 ? d->line[d->nlines - 1] : d->title);
    int a;
    for (;;) {
        lock();
        a = s_answer;
        if (a >= 0) {
            __atomic_store_n(&s_hold, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&s_open, 0, __ATOMIC_RELEASE);
            s_rev++;
        }
        unlock();
        if (a >= 0) break;
        sleep_ms(10);
    }
    printf("[sysdlg] answered: %s\n", s_d.choice[a]);
    __atomic_store_n(&s_asking, 0, __ATOMIC_RELEASE);
    return a;
}

void ps3_sysdlg_pad(uint32_t b, int lx)
{
    if (!ps3_sysdlg_active()) return;
    lock();
    if (!s_open && s_hold && !b && lx >= 64 && lx <= 192)   /* all let go: the pad is the title's again */
        __atomic_store_n(&s_hold, 0, __ATOMIC_RELEASE);
    if (s_open && s_answer < 0) {
        const uint32_t press = s_prev == 0xFFFFFFFFu ? 0u : (b & ~s_prev);   /* held as it came up: no press */
        s_prev = b;
        const int dir = lx < 64 ? -1 : lx > 192 ? 1 : 0;
        const int moved = dir != s_prev_dir ? dir : 0;
        s_prev_dir = dir;
        const uint32_t enter = s_d.enter_circle ? BTN_CIRCLE : BTN_CROSS;
        const uint32_t back = s_d.enter_circle ? BTN_CROSS : BTN_CIRCLE;
        if (!s_armed) {
            if (!(b & (enter | back))) s_armed = 1;
        } else if (now_ms() - s_opened >= SETTLE_MS) {
            if ((press & BTN_LEFT) || moved < 0) {
                if (s_d.focus != 0) { s_d.focus = 0; s_rev++; }
            } else if ((press & BTN_RIGHT) || moved > 0) {
                if (s_d.focus != 1) { s_d.focus = 1; s_rev++; }
            } else if (press & enter) {
                s_answer = s_d.focus;
            } else if (press & back) {
                s_answer = s_cancel;
            }
        }
    }
    unlock();
}

int ps3_sysdlg_key(int vk)
{
    if (!ps3_sysdlg_active()) return 0;
    int took = 0;
    lock();
    if (s_open && s_answer < 0) {
        const int ready = now_ms() - s_opened >= SETTLE_MS;
        took = 1;
        switch (vk) {
        case 0x25:   /* VK_LEFT */
            if (s_d.focus != 0) { s_d.focus = 0; s_rev++; }
            break;
        case 0x27:   /* VK_RIGHT */
            if (s_d.focus != 1) { s_d.focus = 1; s_rev++; }
            break;
        case 0x0D:   /* VK_RETURN */
        case 0x20:   /* VK_SPACE */
            if (ready) s_answer = s_d.focus;
            break;
        case 0x1B:   /* VK_ESCAPE */
        case 0x08:   /* VK_BACK */
            if (ready) s_answer = s_cancel;
            break;
        default:
            took = 0;
        }
    }
    unlock();
    return took;
}
