/*
 * UAE - The Un*x Amiga Emulator
 *
 * CIA chip support
 *
 * (c) 1995 Bernd Schmidt
 */

#pragma once
extern void CIA_reset();
extern void CIA_vsync_prehandler();
extern void CIA_hsync_prehandler();
extern void CIA_hsync_posthandler(bool);
extern void CIA_handler();
extern void CIAA_tod_inc(int);
extern void CIAB_tod_handler(int);

extern void diskindex_handler();
extern void cia_parallelack();
extern void cia_diskindex();

extern void dumpcia();
extern void rethink_cias();
extern int resetwarning_do(int);
extern void cia_set_overlay(bool);

extern int parallel_direct_write_data(uae_u8, uae_u8);
extern int parallel_direct_read_data(uae_u8*);
extern int parallel_direct_write_status(uae_u8, uae_u8);
extern int parallel_direct_read_status(uae_u8*);

extern void rtc_hardreset();
