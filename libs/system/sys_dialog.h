/*
 * ps3recomp - the system's own questions over the title's picture
 *
 * On a PS3 some system calls ask the player before they act -- deleting a save asks whether to -- and the system
 * draws the question over the title's picture and takes the pad while it is up: the title's pad reads come back with
 * nothing pressed. Here the renderer draws it (ps3_sysdlg_view), cellPad hands it the pad (ps3_sysdlg_pad) and the
 * window hands it the keyboard (ps3_sysdlg_key), while the calling thread waits for the answer (ps3_sysdlg_ask).
 */
#ifndef PS3RECOMP_SYS_DIALOG_H
#define PS3RECOMP_SYS_DIALOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3_SYSDLG_LINES  5
#define PS3_SYSDLG_ICON_W 320     /* a save's ICON0.PNG */
#define PS3_SYSDLG_ICON_H 176

typedef struct PS3SysDialog {
    char title[96];                                  /* UTF-8, the heading */
    char line[PS3_SYSDLG_LINES][192];                /* UTF-8; the last one is the question */
    int  nlines;
    char choice[2][32];                              /* "Yes", "No" */
    int  focus;                                      /* the choice the cursor is on */
    int  enter_circle;                               /* circle chooses, cross goes back (input.enter_button) */
    int  iw, ih;                                     /* the picture, 0 for none */
    unsigned char icon[PS3_SYSDLG_ICON_W * PS3_SYSDLG_ICON_H * 4];   /* straight-alpha RGBA, iw x ih */
} PS3SysDialog;

/* Ask d, and wait for the answer: the index of the choice taken, 0 or 1; going back takes `cancel`. With nothing
 * to draw the question (no ps3_sysdlg_drawn), `cancel` at once. One question at a time: a second waits. */
int ps3_sysdlg_ask(const PS3SysDialog* d, int cancel);

/* The renderer: it draws the questions. */
void ps3_sysdlg_drawn(void);
/* 0 with no question up; else a number that changes whenever what is drawn changes. */
unsigned ps3_sysdlg_state(void);
/* The question up, copied into *out; 0 if none. */
int ps3_sysdlg_view(PS3SysDialog* out);

/* cellPad: a question is up, so the title is handed nothing, and port 0's report goes to the question:
 * buttons (DIGITAL2 << 8) | DIGITAL1, the left stick's x 0..255. */
int ps3_sysdlg_active(void);
void ps3_sysdlg_pad(uint32_t buttons, int lx);

/* The window: a key went down (a Windows virtual-key code). 1 if the question took it. */
int ps3_sysdlg_key(int vk);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SYS_DIALOG_H */
