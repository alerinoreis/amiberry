/*
* UAE - The Un*x Amiga Emulator
*
* Cirrus Logic based graphics board emulation
*
* Copyright 2013 Toni Wilen
*
*/

#define VRAMLOG 0
#define MEMLOGR 0
#define MEMLOGW 0
#define MEMLOGINDIRECT 0
#define MEMDEBUG 0
#define MEMDEBUGMASK 0x7fffff
#define MEMDEBUGTEST 0x1ff000
#define PICASSOIV_DEBUG_IO 0

static bool memlogr = false;
static bool memlogw = false;

#define BYTESWAP_WORD -1
#define BYTESWAP_LONG 1

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "uae.h"
#include "include/memory.h"
#include "debug.h"
#include "custom.h"
#include "newcpu.h"
#include "picasso96.h"
#include "statusline.h"
#include "rommgr.h"
#include "zfile.h"
#include "gfxboard.h"

#define GFXBOARD_AUTOCONFIG_SIZE 131072

#define BOARD_REGISTERS_SIZE 0x00010000

#define BOARD_MANUFACTURER_PICASSO 2167
#define BOARD_MODEL_MEMORY_PICASSOII 11
#define BOARD_MODEL_REGISTERS_PICASSOII 12

#define BOARD_MODEL_MEMORY_PICASSOIV 24
#define BOARD_MODEL_REGISTERS_PICASSOIV 23
#define PICASSOIV_REG  0x00600000
#define PICASSOIV_IO   0x00200000
#define PICASSOIV_VRAM1 0x01000000
#define PICASSOIV_VRAM2 0x00800000
#define PICASSOIV_ROM_OFFSET 0x0200
#define PICASSOIV_FLASH_OFFSET 0x8000
#define PICASSOIV_FLASH_BANK 0x8000
#define PICASSOIV_MAX_FLASH (GFXBOARD_AUTOCONFIG_SIZE - 32768)

#define PICASSOIV_BANK_UNMAPFLASH 2
#define PICASSOIV_BANK_MAPRAM 4
#define PICASSOIV_BANK_FLASHBANK 128

#define PICASSOIV_INT_VBLANK 128

#define BOARD_MANUFACTURER_PICCOLO 2195
#define BOARD_MODEL_MEMORY_PICCOLO 5
#define BOARD_MODEL_REGISTERS_PICCOLO 6
#define BOARD_MODEL_MEMORY_PICCOLO64 10
#define BOARD_MODEL_REGISTERS_PICCOLO64 11

#define BOARD_MANUFACTURER_SPECTRUM 2193
#define BOARD_MODEL_MEMORY_SPECTRUM 1
#define BOARD_MODEL_REGISTERS_SPECTRUM 2

extern addrbank gfxboard_bank_memory, gfxboard_bank_memory_nojit;
extern addrbank gfxboard_bank_special;
extern addrbank gfxboard_bank_wbsmemory;
extern addrbank gfxboard_bank_lbsmemory;
extern addrbank gfxboard_bank_nbsmemory;

struct gfxboard
{
	TCHAR *name;
	int manufacturer;
	int model_memory;
	int model_registers;
	int serial;
	int vrammin;
	int vrammax;
	int banksize;
	int chiptype;
	bool z3;
	int irq;
	bool swap;
};

#define PICASSOIV_Z2 10
#define PICASSOIV_Z3 11

#define ISP4() (currprefs.rtgmem_type == PICASSOIV_Z2 || currprefs.rtgmem_type == PICASSOIV_Z3)

// Picasso II: 8* 4x256 (1M) or 16* 4x256 (2M)
// Piccolo: 8* 4x256 + 2* 16x256 (2M)

//static struct gfxboard boards[] =
//{
//	{
//		_T("Picasso II"),
//		BOARD_MANUFACTURER_PICASSO, BOARD_MODEL_MEMORY_PICASSOII, BOARD_MODEL_REGISTERS_PICASSOII,
//		0x00020000, 0x00100000, 0x00200000, 0x00200000, CIRRUS_ID_CLGD5426, false, 0, false
//	},
//	{
//		_T("Picasso II+"),
//		BOARD_MANUFACTURER_PICASSO, BOARD_MODEL_MEMORY_PICASSOII, BOARD_MODEL_REGISTERS_PICASSOII,
//		0x00100000, 0x00100000, 0x00200000, 0x00200000, CIRRUS_ID_CLGD5428, false, 2, false
//	},
//	{
//		_T("Piccolo Zorro II"),
//		BOARD_MANUFACTURER_PICCOLO, BOARD_MODEL_MEMORY_PICCOLO, BOARD_MODEL_REGISTERS_PICCOLO,
//		0x00000000, 0x00100000, 0x00200000, 0x00200000, CIRRUS_ID_CLGD5426, false, 6, true
//	},
//	{
//		_T("Piccolo Zorro III"),
//		BOARD_MANUFACTURER_PICCOLO, BOARD_MODEL_MEMORY_PICCOLO, BOARD_MODEL_REGISTERS_PICCOLO,
//		0x00000000, 0x00100000, 0x00200000, 0x00200000, CIRRUS_ID_CLGD5426, true, 6, true
//	},
//	{
//		_T("Piccolo SD64 Zorro II"),
//		BOARD_MANUFACTURER_PICCOLO, BOARD_MODEL_MEMORY_PICCOLO64, BOARD_MODEL_REGISTERS_PICCOLO64,
//		0x00000000, 0x00200000, 0x00400000, 0x00400000, CIRRUS_ID_CLGD5434, false, 6, true
//	},
//	{
//		_T("Piccolo SD64 Zorro III"),
//		BOARD_MANUFACTURER_PICCOLO, BOARD_MODEL_MEMORY_PICCOLO64, BOARD_MODEL_REGISTERS_PICCOLO64,
//		0x00000000, 0x00200000, 0x00400000, 0x04000000, CIRRUS_ID_CLGD5434, true, 6, true
//	},
//	{
//		_T("Spectrum 28/24 Zorro II"),
//		BOARD_MANUFACTURER_SPECTRUM, BOARD_MODEL_MEMORY_SPECTRUM, BOARD_MODEL_REGISTERS_SPECTRUM,
//		0x00000000, 0x00100000, 0x00200000, 0x00200000, CIRRUS_ID_CLGD5428, false, 6, true
//	},
//	{
//		_T("Spectrum 28/24 Zorro III"),
//		BOARD_MANUFACTURER_SPECTRUM, BOARD_MODEL_MEMORY_SPECTRUM, BOARD_MODEL_REGISTERS_SPECTRUM,
//		0x00000000, 0x00100000, 0x00200000, 0x00200000, CIRRUS_ID_CLGD5428, true, 6, true
//	},
//	{
//		_T("Picasso IV Zorro II"),
//		BOARD_MANUFACTURER_PICASSO, BOARD_MODEL_MEMORY_PICASSOIV, BOARD_MODEL_REGISTERS_PICASSOIV,
//		0x00000000, 0x00400000, 0x00400000, 0x00400000, CIRRUS_ID_CLGD5446, false, 2, false
//	},
//	{
//		// REG:00600000 IO:00200000 VRAM:01000000
//		_T("Picasso IV Zorro III"),
//		BOARD_MANUFACTURER_PICASSO, BOARD_MODEL_MEMORY_PICASSOIV, 0,
//		0x00000000, 0x00400000, 0x00400000, 0x04000000, CIRRUS_ID_CLGD5446, true, 2, false
//	}
//};

static TCHAR memorybankname[40];
static TCHAR wbsmemorybankname[40];
static TCHAR lbsmemorybankname[40];
static TCHAR regbankname[40];

static int configured_mem, configured_regs;
static struct gfxboard *board;
static uae_u8 expamem_lo;
static uae_u8 *automemory;
static uae_u32 banksize_mask;

static uae_u8 picassoiv_bank, picassoiv_flifi;
static uae_u8 p4autoconfig[256];
static struct zfile *p4rom;
static bool p4z2;
static uae_u32 p4_mmiobase;
static uae_u32 p4_special_mask;
static uae_u32 p4_vram_bank[2];

//static CirrusVGAState vga;
static uae_u8 *vram, *vramrealstart;
static int vram_start_offset;
static uae_u32 gfxboardmem_start;
static bool monswitch;
static bool oldswitch;
static int fullrefresh;
static bool modechanged;
static uae_u8 *gfxboard_surface, *vram_address, *fakesurface_surface;
static bool gfxboard_vblank;
static bool gfxboard_intena;
static bool vram_enabled, vram_offset_enabled;
static bool vram_byteswap;
//static hwaddr vram_offset[2];
static uae_u8 cirrus_pci[0x44];
static uae_u8 p4_pci[0x44];

static uae_u32 vgaioregionptr, vgavramregionptr, vgabank0regionptr, vgabank1regionptr;

//static const MemoryRegionOps *vgaio, *vgaram, *vgalowram, *vgammio;
//static MemoryRegion vgaioregion, vgavramregion;

bool gfxboard_toggle(int mode)
{
	if (vram == NULL)
		return false;
	if (monswitch) {
		monswitch = false;
		picasso_requested_on = 0;
		return true;
	}
	else {
		/*int width, height;
		vga.vga.get_resolution(&vga.vga, &width, &height);
		if (width > 16 && height > 16) {
			monswitch = true;
			picasso_requested_on = 1;*/
			return true;
		//}
	}
	return false;
}

void gfxboard_refresh(void)
{
	fullrefresh = 2;
}

double gfxboard_get_vsync(void)
{
	return vblank_hz; // FIXME
}

bool gfxboard_is_z3 (int type)
{
	if (type == GFXBOARD_UAE_Z2)
		return false;
	if (type == GFXBOARD_UAE_Z3)
		return true;
	return false;
}

