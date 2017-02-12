
#pragma once
#define AKIKO_BASE 0xb80000
#define AKIKO_BASE_END 0xb80100 /* ?? */

extern void akiko_reset ();
extern int akiko_init ();
extern void akiko_free ();

extern void AKIKO_hsync_handler ();
extern void akiko_mute (int);

extern void rethink_akiko ();
