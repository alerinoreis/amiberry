#include "sysconfig.h"
#include "sysdeps.h"
#include "config.h"
#include "uae.h"
#include "options.h"
#include "gui.h"
#include "memory.h"
#include "newcpu.h"
#include "custom.h"
#include "xwin.h"
#include "drawing.h"
#include "inputdevice.h"
#include "savestate.h"
#include "picasso96.h"
#include "pandora_gfx.h"

#include <png.h>
#include "SDL.h"
#include "sounddep/sound.h"
#include "gfxboard.h"

#define DM_DX_FULLSCREEN 1
#define DM_W_FULLSCREEN 2
#define DM_D3D_FULLSCREEN 16
#define DM_PICASSO96 32
#define DM_DDRAW 64
#define DM_DC 128
#define DM_D3D 256
#define DM_SWSCALE 1024

#define SM_WINDOW 0
#define SM_FULLSCREEN_DX 2
#define SM_OPENGL_WINDOW 3
#define SM_OPENGL_FULLWINDOW 9
#define SM_OPENGL_FULLSCREEN_DX 4
#define SM_D3D_WINDOW 5
#define SM_D3D_FULLWINDOW 10
#define SM_D3D_FULLSCREEN_DX 6
#define SM_FULLWINDOW 7
#define SM_NONE 11

struct uae_filter* usedfilter;
int scalepicasso;
static double remembered_vblank;
static volatile int vblankthread_mode, vblankthread_counter;

struct winuae_currentmode
{
	unsigned int flags;
	int native_width, native_height, native_depth, pitch;
	int current_width, current_height, current_depth;
	int amiga_width, amiga_height;
	int initdone;
	int fullfill;
	int vsync;
	int freq;
};

/* SDL surface for output of emulation */
SDL_Surface* screen = nullptr;

/* Possible screen modes (x and y resolutions) */
#define MAX_SCREEN_MODES 13
static int x_size_table[MAX_SCREEN_MODES] = {640, 640, 720, 800, 800, 960, 1024, 1280, 1280, 1360, 1366, 1680, 1920};
static int y_size_table[MAX_SCREEN_MODES] = {400, 480, 400, 480, 600, 540, 768, 720, 800, 768, 768, 1050, 1080};

struct PicassoResolution* DisplayModes;
struct MultiDisplay Displays[MAX_DISPLAYS];

static struct winuae_currentmode currentmodestruct;
static int screen_is_initialized;
static int display_change_requested;
int window_led_drives, window_led_drives_end;
int window_led_hd, window_led_hd_end;
int window_led_joys, window_led_joys_end, window_led_joy_start;
extern int console_logging;
int window_extra_width, window_extra_height;

static struct winuae_currentmode* currentmode = &currentmodestruct;
static int wasfullwindow_a, wasfullwindow_p;

static int vblankbasewait1, vblankbasewait2, vblankbasewait3, vblankbasefull, vblankbaseadjust;
static bool vblankbaselace;
static int vblankbaselace_chipset;
static bool vblankthread_oddeven, vblankthread_oddeven_got;
static int graphics_mode_changed;
int vsync_modechangetimeout = 10;

int screen_is_picasso = 0;

extern int reopen(int, bool);

#define VBLANKTH_KILL 0
#define VBLANKTH_CALIBRATE 1
#define VBLANKTH_IDLE 2
#define VBLANKTH_ACTIVE_WAIT 3
#define VBLANKTH_ACTIVE 4
#define VBLANKTH_ACTIVE_START 5
#define VBLANKTH_ACTIVE_SKIPFRAME 6
#define VBLANKTH_ACTIVE_SKIPFRAME2 7

static void changevblankthreadmode(int newmode)
{
	//changevblankthreadmode_do(newmode, false);
}

static void changevblankthreadmode_fast(int newmode)
{
	//changevblankthreadmode_do(newmode, true);
}

int WIN32GFX_IsPicassoScreen()
{
	return screen_is_picasso;
}

int isscreen()
{
	return screen ? 1 : 0;
}

static void clearscreen()
{
	SDL_RenderClear(renderer);
}

static int isfullscreen_2(struct uae_prefs* p)
{
	int idx = screen_is_picasso ? 1 : 0;
	return p->gfx_apmode[idx].gfx_fullscreen == GFX_FULLSCREEN ? 1 : (p->gfx_apmode[idx].gfx_fullscreen == GFX_FULLWINDOW ? -1 : 0);
}

int isfullscreen()
{
	return isfullscreen_2(&currprefs);
}

int WIN32GFX_GetDepth(int real)
{
	if (!currentmode->native_depth)
		return currentmode->current_depth;
	return real ? currentmode->native_depth : currentmode->current_depth;
}

int WIN32GFX_GetWidth()
{
	return currentmode->current_width;
}

int WIN32GFX_GetHeight()
{
	return currentmode->current_height;
}

static int init_round;
static BOOL doInit();

int default_freq = 60;

static uae_u8 *scrlinebuf;

static struct MultiDisplay* getdisplay2(struct uae_prefs* p, int index)
{
	int max;
	int display = index < 0 ? p->gfx_apmode[screen_is_picasso ? APMODE_RTG : APMODE_NATIVE].gfx_display - 1 : index;

	max = 0;
	while (Displays[max].monitorname)
		max++;
	if (max == 0)
	{
		gui_message(_T("no display adapters! Exiting"));
		exit(0);
	}
	if (index >= 0 && display >= max)
		return nullptr;
	if (display >= max)
		display = 0;
	if (display < 0)
		display = 0;
	return &Displays[display];
}

struct MultiDisplay* getdisplay(struct uae_prefs* p)
{
	return getdisplay2(p, -1);
}

void desktop_coords(int* dw, int* dh, int* ax, int* ay, int* aw, int* ah)
{
	struct MultiDisplay* md = getdisplay(&currprefs);

	*dw = md->rect.right - md->rect.left;
	*dh = md->rect.bottom - md->rect.top;
	*ax = amigawin_rect.x;
	*ay = amigawin_rect.y;
	*aw = amigawin_rect.w;
	*ah = amigawin_rect.h;
}

int target_get_display(const TCHAR* name)
{
	for (int i = 0; Displays[i].monitorname; i++)
	{
		struct MultiDisplay* md = &Displays[i];
		if (!_tcscmp(md->monitorid, name))
			return i + 1;
	}
	for (int i = 0; Displays[i].monitorname; i++)
	{
		struct MultiDisplay* md = &Displays[i];
		if (!_tcscmp(md->adapterid, name))
			return i + 1;
	}
	for (int i = 0; Displays[i].monitorname; i++)
	{
		struct MultiDisplay* md = &Displays[i];
		if (!_tcscmp(md->adaptername, name))
			return i + 1;
	}
	for (int i = 0; Displays[i].monitorname; i++)
	{
		struct MultiDisplay* md = &Displays[i];
		if (!_tcscmp(md->adapterid, name))
			return i + 1;
	}
	return -1;
}

const TCHAR* target_get_display_name(int num, bool friendlyname)
{
	if (num <= 0)
		return nullptr;
	struct MultiDisplay* md = getdisplay2(nullptr, num - 1);
	if (!md)
		return nullptr;
	if (friendlyname)
		return md->monitorname;
	return md->monitorid;
}

static int picasso_offset_x, picasso_offset_y;
static float picasso_offset_mx, picasso_offset_my;

void getfilteroffset(float *dx, float *dy, float *mx, float *my)
{
	*dx = 0;
	*dy = 0;
	*mx = 0;
	*my = 0;
}

void getgfxoffset(float* dxp, float* dyp, float* mxp, float* myp)
{
	float dx, dy;

	getfilteroffset(&dx, &dy, mxp, myp);
	*dxp = dx;
	*dyp = dy;
	if (picasso_on)
	{
		dx = picasso_offset_x;
		dy = picasso_offset_y;
		*mxp = picasso_offset_mx;
		*myp = picasso_offset_my;
	}
	*dxp = dx;
	*dyp = dy;
	//if (currentmode->flags & DM_W_FULLSCREEN) {
	if (scalepicasso && screen_is_picasso)
		return;
	if (usedfilter && !screen_is_picasso)
		return;
	if (currentmode->fullfill && (currentmode->current_width > currentmode->native_width || currentmode->current_height > currentmode->native_height))
		return;
	dx += (currentmode->native_width - currentmode->current_width) / 2;
	dy += (currentmode->native_height - currentmode->current_height) / 2;
	//}
	*dxp = dx;
	*dyp = dy;
}

void DX_Fill(int dstx, int dsty, int width, int height, uae_u32 color)
{
	SDL_Rect dstrect;
	if (width < 0)
		width = currentmode->current_width;
	if (height < 0)
		height = currentmode->current_height;

	dstrect.x = dstx;
	dstrect.y = dsty;
	dstrect.w = width;
	dstrect.h = height;

	SDL_FillRect(screen, &dstrect, color);
}

static int rgbformat_bits(RGBFTYPE t)
{
	unsigned long f = 1 << t;
	return ((f & RGBMASK_8BIT) != 0 ? 8
		        : (f & RGBMASK_15BIT) != 0 ? 15
		        : (f & RGBMASK_16BIT) != 0 ? 16
		        : (f & RGBMASK_24BIT) != 0 ? 24
		        : (f & RGBMASK_32BIT) != 0 ? 32
		        : 0);
}

int getrefreshrate(int width, int height)
{
	struct apmode* ap = picasso_on ? &currprefs.gfx_apmode[APMODE_RTG] : &currprefs.gfx_apmode[APMODE_NATIVE];
	int freq = 0;

	if (ap->gfx_refreshrate <= 0)
		return 0;

	struct MultiDisplay* md = getdisplay(&currprefs);
	for (int i = 0; md->DisplayModes[i].depth >= 0; i++)
	{
		struct PicassoResolution* pr = &md->DisplayModes[i];
		if (pr->res.width == width && pr->res.height == height)
		{
			for (int j = 0; pr->refresh[j] > 0; j++)
			{
				if (pr->refresh[j] == ap->gfx_refreshrate)
					return ap->gfx_refreshrate;
				if (pr->refresh[j] > freq && pr->refresh[j] < ap->gfx_refreshrate)
					freq = pr->refresh[j];
			}
		}
	}
	write_log(_T("Refresh rate %d not supported, using %d\n"), ap->gfx_refreshrate, freq);
	return freq;
}

static int set_ddraw_2()
{
	int bits = (currentmode->current_depth + 7) & ~7;
	int width = currentmode->native_width;
	int height = currentmode->native_height;

	struct apmode* ap = picasso_on ? &currprefs.gfx_apmode[APMODE_RTG] : &currprefs.gfx_apmode[APMODE_NATIVE];
	int freq = ap->gfx_refreshrate;

	if (WIN32GFX_IsPicassoScreen() && (picasso96_state.Width > width || picasso96_state.Height > height))
	{
		width = picasso96_state.Width;
		height = picasso96_state.Height;
	}

	SDL_FreeSurface(screen);

	screen = SDL_CreateRGBSurface(0, width, height, bits, 0, 0, 0, 0);
	check_error_sdl(screen == nullptr, "Unable to create a surface");

	SDL_RenderSetLogicalSize(renderer, width, height);

	// Initialize SDL Texture for the renderer
	texture = SDL_CreateTexture(renderer,
		SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STREAMING,
		width,
		height);
	check_error_sdl(texture == nullptr, "Unable to create texture");

	if (SDL_LockSurface(screen))
	{
		currentmode->pitch = screen->pitch;
		SDL_UnlockSurface(screen);
	}

	//write_log(_T("set_ddraw: %dx%d@%d-bytes\n"), width, height, bits);
	return 1;
}

static int _cdecl resolution_compare(const void* a, const void* b)
{
	struct PicassoResolution* ma = (struct PicassoResolution *)a;
	struct PicassoResolution* mb = (struct PicassoResolution *)b;
	if (ma->res.width < mb->res.width)
		return -1;
	if (ma->res.width > mb->res.width)
		return 1;
	if (ma->res.height < mb->res.height)
		return -1;
	if (ma->res.height > mb->res.height)
		return 1;
	return ma->depth - mb->depth;
}

static void sortmodes(struct MultiDisplay* md)
{
	int i, idx = -1;
	int pw = -1, ph = -1;

	i = 0;
	while (md->DisplayModes[i].depth >= 0)
		i++;
	qsort(md->DisplayModes, i, sizeof(struct PicassoResolution), resolution_compare);
	for (i = 0; md->DisplayModes[i].depth >= 0; i++)
	{
		int j, k;
		for (j = 0; md->DisplayModes[i].refresh[j]; j++)
		{
			for (k = j + 1; md->DisplayModes[i].refresh[k]; k++)
			{
				if (md->DisplayModes[i].refresh[j] > md->DisplayModes[i].refresh[k])
				{
					int t = md->DisplayModes[i].refresh[j];
					md->DisplayModes[i].refresh[j] = md->DisplayModes[i].refresh[k];
					md->DisplayModes[i].refresh[k] = t;
					t = md->DisplayModes[i].refreshtype[j];
					md->DisplayModes[i].refreshtype[j] = md->DisplayModes[i].refreshtype[k];
					md->DisplayModes[i].refreshtype[k] = t;
				}
			}
		}
		if (md->DisplayModes[i].res.height != ph || md->DisplayModes[i].res.width != pw)
		{
			ph = md->DisplayModes[i].res.height;
			pw = md->DisplayModes[i].res.width;
			idx++;
		}
		md->DisplayModes[i].residx = idx;
	}
}

static void modesList(struct MultiDisplay* md)
{
	int i, j;

	i = 0;
	while (md->DisplayModes[i].depth >= 0)
	{
		write_log(_T("%d: %s%s ("), i, md->DisplayModes[i].rawmode ? _T("!") : _T(""), md->DisplayModes[i].name);
		j = 0;
		while (md->DisplayModes[i].refresh[j] > 0)
		{
			if (j > 0)
			write_log(_T(","));
			if (md->DisplayModes[i].refreshtype[j] & REFRESH_RATE_RAW)
			write_log(_T("!"));
			write_log(_T("%d"), md->DisplayModes[i].refresh[j]);
			if (md->DisplayModes[i].refreshtype[j] & REFRESH_RATE_LACE)
			write_log(_T("i"));
			j++;
		}
		write_log(_T(")\n"));
		i++;
	}
}

void sortdisplays()
{
	struct MultiDisplay* md;
	int i, count = 0;
	int ct;
	int bit_idx;
	int bits[] = { 8, 16, 32 };

	// Declare display mode structure to be filled in.
	SDL_DisplayMode current;
	// Get current display mode
	SDL_GetCurrentDisplayMode(0, &current);

	int w = current.w;
	int h = current.h;
	int b = 32; //TODO: Bit depth
	int freq = current.refresh_rate;
	write_log(_T("Desktop: W=%d H=%d B=%d.\n"), w, h, b);

	md = Displays;
	DisplayModes = md->DisplayModes = xmalloc(struct PicassoResolution, MAX_PICASSO_MODES);
	for (i = 0; i < MAX_SCREEN_MODES && count < MAX_PICASSO_MODES; i++)
	{
		for (bit_idx = 0; bit_idx < 3; ++bit_idx)
		{
			int bitdepth = bits[bit_idx];
			int bit_unit = (bitdepth + 1) & 0xF8;
			int rgbFormat = (bitdepth == 8 ? RGBFB_CLUT : (bitdepth == 16 ? RGBFB_R5G6B5 : RGBFB_R8G8B8A8));

			ct = 0;
			if (bitdepth == 8)
				ct = RGBMASK_8BIT;
			if (bitdepth == 16)
				ct = RGBMASK_16BIT;
			if (bitdepth == 32)
				ct = RGBMASK_32BIT;

			DisplayModes[count].res.width = x_size_table[i];
			DisplayModes[count].res.height = y_size_table[i];
			DisplayModes[count].depth = bit_unit >> 3;
			DisplayModes[count].refresh[0] = freq;
			DisplayModes[count].refresh[1] = 0;
			DisplayModes[count].colormodes = ct;
			sprintf(DisplayModes[count].name, "%dx%d, %d-bit",
				DisplayModes[count].res.width, DisplayModes[count].res.height, DisplayModes[count].depth * 8);

			count++;
		}
	}
	DisplayModes[count].depth = -1;
	sortmodes(md);
	modesList(md);
	DisplayModes = Displays[0].DisplayModes;
}

static int flushymin, flushymax;
#define FLUSH_DIFF 50

static void flushit(struct vidbuffer* vb, int lineno)
{
	if (!currprefs.gfx_api)
		return;
	if (flushymin > lineno)
	{
		if (flushymin - lineno > FLUSH_DIFF && flushymax != 0)
		{
			//D3D_flushtexture(flushymin, flushymax);
			flushymin = currentmode->amiga_height;
			flushymax = 0;
		}
		else
		{
			flushymin = lineno;
		}
	}
	if (flushymax < lineno)
	{
		if (lineno - flushymax > FLUSH_DIFF && flushymax != 0)
		{
			//D3D_flushtexture(flushymin, flushymax);
			flushymin = currentmode->amiga_height;
			flushymax = 0;
		}
		else
		{
			flushymax = lineno;
		}
	}
}

void flush_line(struct vidbuffer* vb, int lineno)
{
	flushit(vb, lineno);
}

void flush_block(struct vidbuffer* vb, int first, int last)
{
	flushit(vb, first);
	flushit(vb, last);
}

void flush_screen(struct vidbuffer* vb, int a, int b)
{
}

static volatile bool render_ok, wait_render;

bool render_screen(bool immediate)
{
	bool v = false;
	int cnt;

	render_ok = false;
	if (picasso_on)
		return render_ok;
	cnt = 0;
	while (wait_render)
	{
		sleep_millis(1);
		cnt++;
		if (cnt > 500)
			return render_ok;
	}
	flushymin = 0;
	flushymax = currentmode->amiga_height;

	updatedisplayarea();

	return true;
}

bool show_screen_maybe(bool show)
{
	struct apmode* ap = picasso_on ? &currprefs.gfx_apmode[1] : &currprefs.gfx_apmode[0];
	if (!ap->gfx_vflip || ap->gfx_vsyncmode == 0 || !ap->gfx_vsync)
	{
		if (show)
			show_screen(0);
		return false;
	}
	return false;
}

void show_screen(int mode)
{
	if (mode == 2)
	{
		updatedisplayarea();
		return;
	}
	if (!render_ok)
	{
		return;
	}

	updatedisplayarea();
	render_ok = false;
}

static uae_u8* ddraw_dolock()
{
	if (!SDL_LockSurface(screen))
	{
		return 0;
	}
	gfxvidinfo.outbuffer->bufmem = static_cast<uae_u8 *>(screen->pixels);
	gfxvidinfo.outbuffer->rowbytes = screen->pitch;
	init_row_map();
	clear_inhibit_frame(IHF_WINDOWHIDDEN);
	return gfxvidinfo.outbuffer->bufmem;
}

int lockscr()
{
	SDL_LockSurface(screen);
	return 1;
}

int lockscr(struct vidbuffer*, bool)
{
	flushymin = currentmode->amiga_height;
	flushymax = 0;

	return lockscr();
}

void unlockscr()
{
	SDL_UnlockSurface(screen);
}

void unlockscr(struct vidbuffer* vb)
{
	vb->bufmem = nullptr;
	unlockscr();
}

void flush_clear_screen(struct vidbuffer* vb)
{
	if (!vb)
		return;
	if (lockscr(vb, true))
	{
		int y;
		for (y = 0; y < vb->height_allocated; y++)
		{
			memset(vb->bufmem + y * vb->rowbytes, 0, vb->width_allocated * vb->pixbytes);
		}
		unlockscr(vb);
		flush_screen(vb, 0, 0);
	}
}

/* For the DX_Invalidate() and gfx_unlock_picasso() functions */
static int p96_double_buffer_firstx, p96_double_buffer_lastx;
static int p96_double_buffer_first, p96_double_buffer_last;
static int p96_double_buffer_needs_flushing = 0;

static bool rtg_locked;

static uae_u8* gfx_lock_picasso2(bool fullupdate)
{
	if (!SDL_LockSurface(screen))
	{
		return 0;
	}
	picasso_vidinfo.rowbytes = screen->pitch;
	return static_cast<uae_u8 *>(screen->pixels);
}

uae_u8* gfx_lock_picasso(bool fullupdate, bool doclear)
{
	static uae_u8* p;
	if (rtg_locked)
	{
		return p;
	}
	//EnterCriticalSection(&screen_cs);
	p = gfx_lock_picasso2(fullupdate);
	if (!p)
	{
		//LeaveCriticalSection(&screen_cs);
	}
	else
	{
		rtg_locked = true;
		if (doclear)
		{
			uae_u8* p2 = p;
			for (int h = 0; h < picasso_vidinfo.height; h++)
			{
				memset(p2, 0, picasso_vidinfo.width * picasso_vidinfo.pixbytes);
				p2 += picasso_vidinfo.rowbytes;
			}
		}
	}
	return p;
}

void gfx_unlock_picasso(bool dorender)
{
	rtg_locked = false;
	SDL_UnlockSurface(screen);
}

static void close_hwnds(void)
{
	screen_is_initialized = 0;

	if (renderer)
	{
		SDL_DestroyRenderer(renderer);
		SDL_DestroyWindow(sdlWindow);
		SDL_VideoQuit();
	}
}

static void updatemodes()
{
	RECT rc = getdisplay(&currprefs)->rect;
	currentmode->native_width = rc.right - rc.left;
	currentmode->native_height = rc.bottom - rc.top;
	currentmode->current_width = currentmode->native_width;
	currentmode->current_height = currentmode->native_height;
}

static void update_gfxparams(void)
{
	updatewinfsmode(&currprefs);

#ifdef PICASSO96
	currentmode->vsync = 0;
	if (screen_is_picasso)
	{
		currentmode->current_width = int(picasso96_state.Width * currprefs.rtg_horiz_zoom_mult);
		currentmode->current_height = int(picasso96_state.Height * currprefs.rtg_vert_zoom_mult);
		currprefs.gfx_apmode[1].gfx_interlaced = false;

		currprefs.gfx_apmode[1].gfx_refreshrate = currprefs.gfx_apmode[0].gfx_refreshrate;
		if (currprefs.gfx_apmode[0].gfx_interlaced)
		{
			currprefs.gfx_apmode[1].gfx_refreshrate *= 2;
		}

		if (currprefs.gfx_apmode[1].gfx_vsync)
			currentmode->vsync = 1 + currprefs.gfx_apmode[1].gfx_vsyncmode;
	}
	else
	{
#endif
		currentmode->current_width = currprefs.gfx_size.width;
		currentmode->current_height = currprefs.gfx_size.height;
		if (currprefs.gfx_apmode[0].gfx_vsync)
			currentmode->vsync = 1 + currprefs.gfx_apmode[0].gfx_vsyncmode;
#ifdef PICASSO96
	}
#endif
	currentmode->current_depth = currprefs.color_mode < 5 ? 16 : 32;
	if (screen_is_picasso)
	{
		int pbits = picasso96_state.BytesPerPixel * 8;
		if (pbits <= 8)
		{
			if (currentmode->current_depth == 32)
				pbits = 32;
			else
				pbits = 16;
		}
		if (pbits == 24)
			pbits = 32;
		currentmode->current_depth = pbits;
	}
	currentmode->amiga_width = currentmode->current_width;
	currentmode->amiga_height = currentmode->current_height;

	scalepicasso = 0;
	if (screen_is_picasso)
	{
		if (isfullscreen() < 0)
		{
			if ((currprefs.gf[1].gfx_filter_autoscale == RTG_MODE_CENTER || currprefs.gf[1].gfx_filter_autoscale == RTG_MODE_SCALE) && (picasso96_state.Width != currentmode->native_width || picasso96_state.Height != currentmode->native_height))
				scalepicasso = 1;
			if (currprefs.gf[1].gfx_filter_autoscale == RTG_MODE_CENTER)
				scalepicasso = currprefs.gf[1].gfx_filter_autoscale;
			/*if (!scalepicasso && currprefs.win32_rtgscaleaspectratio)
				scalepicasso = -1;*/
		}
		else if (isfullscreen() > 0)
		{
			if (false) //(!currprefs.win32_rtgmatchdepth)
			{ // can't scale to different color depth
				if (currentmode->native_width > picasso96_state.Width && currentmode->native_height > picasso96_state.Height)
				{
					if (currprefs.gf[1].gfx_filter_autoscale)
						scalepicasso = 1;
				}
				if (currprefs.gf[1].gfx_filter_autoscale == RTG_MODE_CENTER)
					scalepicasso = currprefs.gf[1].gfx_filter_autoscale;
				/*if (!scalepicasso && currprefs.win32_rtgscaleaspectratio)
					scalepicasso = -1;*/
			}
		}
		else if (isfullscreen() == 0)
		{
			if (currprefs.gf[1].gfx_filter_autoscale == RTG_MODE_INTEGER_SCALE)
			{
				scalepicasso = RTG_MODE_INTEGER_SCALE;
				currentmode->current_width = currprefs.gfx_size.width;
				currentmode->current_height = currprefs.gfx_size.height;
			}
			else if (currprefs.gf[1].gfx_filter_autoscale == RTG_MODE_CENTER)
			{
				if (currprefs.gfx_size.width < picasso96_state.Width || currprefs.gfx_size.height < picasso96_state.Height)
				{
					//if (!currprefs.win32_rtgallowscaling)
					//{
					//	;
					//}
					//else if (currprefs.win32_rtgscaleaspectratio)
					//{
						scalepicasso = -1;
						currentmode->current_width = currprefs.gfx_size.width;
						currentmode->current_height = currprefs.gfx_size.height;
					//}
				}
				else
				{
					scalepicasso = 2;
					currentmode->current_width = currprefs.gfx_size.width;
					currentmode->current_height = currprefs.gfx_size.height;
				}
			}
			else if (currprefs.gf[1].gfx_filter_autoscale == RTG_MODE_SCALE)
			{
				if (currprefs.gfx_size.width > picasso96_state.Width || currprefs.gfx_size.height > picasso96_state.Height)
					scalepicasso = 1;
				if ((currprefs.gfx_size.width != picasso96_state.Width || currprefs.gfx_size.height != picasso96_state.Height))
				{
					scalepicasso = 1;
				}
				else if (currprefs.gfx_size.width < picasso96_state.Width || currprefs.gfx_size.height < picasso96_state.Height)
				{
					// no always scaling and smaller? Back to normal size
					currentmode->current_width = changed_prefs.gfx_size_win.width = picasso96_state.Width;
					currentmode->current_height = changed_prefs.gfx_size_win.height = picasso96_state.Height;
				}
				else if (currprefs.gfx_size.width == picasso96_state.Width || currprefs.gfx_size.height == picasso96_state.Height)
				{
					;
				}
				else if (!scalepicasso) //&& currprefs.win32_rtgscaleaspectratio)
				{
					scalepicasso = -1;
				}
			}
			else
			{
				if ((currprefs.gfx_size.width != picasso96_state.Width || currprefs.gfx_size.height != picasso96_state.Height))
					scalepicasso = 1;
				if (!scalepicasso) // && currprefs.win32_rtgscaleaspectratio)
					scalepicasso = -1;
			}
		}

		if (scalepicasso > 0 && (currprefs.gfx_size.width != picasso96_state.Width || currprefs.gfx_size.height != picasso96_state.Height))
		{
			currentmode->current_width = currprefs.gfx_size.width;
			currentmode->current_height = currprefs.gfx_size.height;
		}
	}
}

static int open_windows(bool mousecapture)
{
	static bool started = false;
	int ret, i;

	inputdevice_unacquire();
	reset_sound();

	updatewinfsmode(&currprefs);

	SDL_FreeSurface(screen);

	//if (!DirectDraw_Start())
	//	return 0;

	init_round = 0;
	ret = -2;
	do
	{
		if (ret < -1)
		{
			updatemodes();
			update_gfxparams();
		}
		ret = doInit();
		init_round++;
		//if (ret < -9)
		//{
		//	DirectDraw_Release();
		//	if (!DirectDraw_Start())
		//		return 0;
		//}
	}
	while (ret < 0);

	if (!ret)
	{
		//DirectDraw_Release();
		return ret;
	}

	//bool startactive = (started && mouseactive) || (!started && !currprefs.win32_start_uncaptured && !currprefs.win32_start_minimized);
	//bool startpaused = !started && ((currprefs.win32_start_minimized && currprefs.win32_iconified_pause) || (currprefs.win32_start_uncaptured && currprefs.win32_inactive_pause && isfullscreen() <= 0));
	//bool startminimized = !started && currprefs.win32_start_minimized && isfullscreen() <= 0;

	//if (!rp_isactive() && mousecapture && startactive)
	//	setmouseactive(-1);

	for (i = 0; i < NUM_LEDS; i++)
		gui_flicker_led(i, -1);
	gui_led(LED_POWER, gui_data.powerled);
	gui_fps(0, 0, 0);
	for (i = 0; i < 4; i++)
	{
		if (currprefs.floppyslots[i].dfxtype >= 0)
			gui_led(LED_DF0 + i, 0);
	}

	inputdevice_acquire(TRUE);

	started = true;
	return ret;
}

static void reopen_gfx(void)
{
	open_windows(false);

	if (isvsync() < 0)
		vblank_calibrate(0, false);

	if (isfullscreen() <= 0)
		updatedisplayarea();
}

void graphics_reset(void)
{
	display_change_requested = 2;
}

int check_prefs_changed_gfx()
{
	int c = 0;

	if (!config_changed && !display_change_requested)
		return 0;

	c |= currprefs.gfx_size_fs.width != changed_prefs.gfx_size_fs.width ? 16 : 0;
	c |= currprefs.gfx_size_fs.height != changed_prefs.gfx_size_fs.height ? 16 : 0;
	c |= ((currprefs.gfx_size_win.width + 7) & ~7) != ((changed_prefs.gfx_size_win.width + 7) & ~7) ? 16 : 0;
	c |= currprefs.gfx_size_win.height != changed_prefs.gfx_size_win.height ? 16 : 0;
	c |= currprefs.color_mode != changed_prefs.color_mode ? 2 | 16 : 0;
	c |= currprefs.gfx_apmode[0].gfx_fullscreen != changed_prefs.gfx_apmode[0].gfx_fullscreen ? 16 : 0;
	c |= currprefs.gfx_apmode[1].gfx_fullscreen != changed_prefs.gfx_apmode[1].gfx_fullscreen ? 16 : 0;
	c |= currprefs.gfx_apmode[0].gfx_vsync != changed_prefs.gfx_apmode[0].gfx_vsync ? 2 | 16 : 0;
	c |= currprefs.gfx_apmode[1].gfx_vsync != changed_prefs.gfx_apmode[1].gfx_vsync ? 2 | 16 : 0;
	c |= currprefs.gfx_apmode[0].gfx_vsyncmode != changed_prefs.gfx_apmode[0].gfx_vsyncmode ? 2 | 16 : 0;
	c |= currprefs.gfx_apmode[1].gfx_vsyncmode != changed_prefs.gfx_apmode[1].gfx_vsyncmode ? 2 | 16 : 0;
	c |= currprefs.gfx_apmode[0].gfx_refreshrate != changed_prefs.gfx_apmode[0].gfx_refreshrate ? 2 | 16 : 0;
	c |= currprefs.gfx_autoresolution != changed_prefs.gfx_autoresolution ? (2 | 8 | 16) : 0;
	c |= currprefs.gfx_api != changed_prefs.gfx_api ? (1 | 8 | 32) : 0;
	c |= currprefs.lightboost_strobo != changed_prefs.lightboost_strobo ? (2 | 16) : 0;

	for (int j = 0; j < 2; j++)
	{
		struct gfx_filterdata* gf = &currprefs.gf[j];
		struct gfx_filterdata* gfc = &changed_prefs.gf[j];

		c |= gf->gfx_filter != gfc->gfx_filter ? (2 | 8) : 0;

		for (int i = 0; i <= 2 * MAX_FILTERSHADERS; i++)
		{
			c |= _tcscmp(gf->gfx_filtershader[i], gfc->gfx_filtershader[i]) ? (2 | 8) : 0;
			c |= _tcscmp(gf->gfx_filtermask[i], gfc->gfx_filtermask[i]) ? (2 | 8) : 0;
		}
		c |= _tcscmp(gf->gfx_filteroverlay, gfc->gfx_filteroverlay) ? (2 | 8) : 0;

		c |= gf->gfx_filter_scanlines != gfc->gfx_filter_scanlines ? (1 | 8) : 0;
		c |= gf->gfx_filter_scanlinelevel != gfc->gfx_filter_scanlinelevel ? (1 | 8) : 0;
		c |= gf->gfx_filter_scanlineratio != gfc->gfx_filter_scanlineratio ? (1 | 8) : 0;

		c |= gf->gfx_filter_horiz_zoom_mult != gfc->gfx_filter_horiz_zoom_mult ? (1) : 0;
		c |= gf->gfx_filter_vert_zoom_mult != gfc->gfx_filter_vert_zoom_mult ? (1) : 0;

		c |= gf->gfx_filter_filtermode != gfc->gfx_filter_filtermode ? (2 | 8) : 0;
		c |= gf->gfx_filter_bilinear != gfc->gfx_filter_bilinear ? (2 | 8) : 0;
		c |= gf->gfx_filter_noise != gfc->gfx_filter_noise ? (1) : 0;
		c |= gf->gfx_filter_blur != gfc->gfx_filter_blur ? (1) : 0;

		c |= gf->gfx_filter_aspect != gfc->gfx_filter_aspect ? (1) : 0;
		c |= gf->gfx_filter_keep_aspect != gfc->gfx_filter_keep_aspect ? (1) : 0;
		c |= gf->gfx_filter_keep_autoscale_aspect != gfc->gfx_filter_keep_autoscale_aspect ? (1) : 0;
		c |= gf->gfx_filter_luminance != gfc->gfx_filter_luminance ? (1) : 0;
		c |= gf->gfx_filter_contrast != gfc->gfx_filter_contrast ? (1) : 0;
		c |= gf->gfx_filter_saturation != gfc->gfx_filter_saturation ? (1) : 0;
		c |= gf->gfx_filter_gamma != gfc->gfx_filter_gamma ? (1) : 0;
	}

	c |= currprefs.rtg_horiz_zoom_mult != changed_prefs.rtg_horiz_zoom_mult ? (1) : 0;
	c |= currprefs.rtg_vert_zoom_mult != changed_prefs.rtg_vert_zoom_mult ? (1) : 0;

	c |= currprefs.gfx_luminance != changed_prefs.gfx_luminance ? (1 | 256) : 0;
	c |= currprefs.gfx_contrast != changed_prefs.gfx_contrast ? (1 | 256) : 0;
	c |= currprefs.gfx_gamma != changed_prefs.gfx_gamma ? (1 | 256) : 0;

	c |= currprefs.gfx_resolution != changed_prefs.gfx_resolution ? (128) : 0;
	c |= currprefs.gfx_vresolution != changed_prefs.gfx_vresolution ? (128) : 0;
	c |= currprefs.gfx_autoresolution_minh != changed_prefs.gfx_autoresolution_minh ? (128) : 0;
	c |= currprefs.gfx_autoresolution_minv != changed_prefs.gfx_autoresolution_minv ? (128) : 0;
	c |= currprefs.gfx_iscanlines != changed_prefs.gfx_iscanlines ? (2 | 8) : 0;
	c |= currprefs.gfx_pscanlines != changed_prefs.gfx_pscanlines ? (2 | 8) : 0;
	c |= currprefs.monitoremu != changed_prefs.monitoremu ? (2 | 8) : 0;

	c |= currprefs.gfx_lores_mode != changed_prefs.gfx_lores_mode ? (2 | 8) : 0;
	c |= currprefs.gfx_scandoubler != changed_prefs.gfx_scandoubler ? (2 | 8) : 0;
	c |= currprefs.gfx_apmode[APMODE_NATIVE].gfx_display != changed_prefs.gfx_apmode[APMODE_NATIVE].gfx_display ? (2 | 4 | 8) : 0;
	c |= currprefs.gfx_apmode[APMODE_RTG].gfx_display != changed_prefs.gfx_apmode[APMODE_RTG].gfx_display ? (2 | 4 | 8) : 0;
	c |= currprefs.gfx_blackerthanblack != changed_prefs.gfx_blackerthanblack ? (2 | 8) : 0;
	c |= currprefs.gfx_apmode[APMODE_NATIVE].gfx_backbuffers != changed_prefs.gfx_apmode[APMODE_NATIVE].gfx_backbuffers ? (2 | 8) : 0;
	c |= currprefs.gfx_apmode[APMODE_NATIVE].gfx_interlaced != changed_prefs.gfx_apmode[APMODE_NATIVE].gfx_interlaced ? (2 | 8) : 0;
	c |= currprefs.gfx_apmode[APMODE_RTG].gfx_backbuffers != changed_prefs.gfx_apmode[APMODE_RTG].gfx_backbuffers ? (2 | 8) : 0;

	if (display_change_requested || c)
	{
		bool setpause = false;
		bool dontcapture = false;
		int keepfsmode =
			currprefs.gfx_apmode[0].gfx_fullscreen == changed_prefs.gfx_apmode[0].gfx_fullscreen &&
			currprefs.gfx_apmode[1].gfx_fullscreen == changed_prefs.gfx_apmode[1].gfx_fullscreen;
		cfgfile_configuration_change(1);

		currprefs.gfx_autoresolution = changed_prefs.gfx_autoresolution;
		currprefs.color_mode = changed_prefs.color_mode;
		currprefs.gfx_api = changed_prefs.gfx_api;
		currprefs.lightboost_strobo = changed_prefs.lightboost_strobo;

		if (changed_prefs.gfx_apmode[0].gfx_fullscreen == GFX_FULLSCREEN)
		{
			if (currprefs.gfx_api != changed_prefs.gfx_api)
				display_change_requested = 1;
		}

		if (display_change_requested)
		{
			if (display_change_requested == 2)
			{
				c = 512;
			}
			else
			{
				c = 2;
				keepfsmode = 0;
				if (display_change_requested <= -1)
				{
					dontcapture = true;
					if (display_change_requested == -2)
						setpause = true;
					/*if (pause_emulation)
						setpause = true;*/
				}
			}
			display_change_requested = 0;
		}

		for (int j = 0; j < 2; j++)
		{
			struct gfx_filterdata* gf = &currprefs.gf[j];
			struct gfx_filterdata* gfc = &changed_prefs.gf[j];
			memcpy(gf, gfc, sizeof(struct gfx_filterdata));
		}

		//		currprefs.rtg_horiz_zoom_mult = changed_prefs.rtg_horiz_zoom_mult;
		//		currprefs.rtg_vert_zoom_mult = changed_prefs.rtg_vert_zoom_mult;

		currprefs.gfx_luminance = changed_prefs.gfx_luminance;
		currprefs.gfx_contrast = changed_prefs.gfx_contrast;
		currprefs.gfx_gamma = changed_prefs.gfx_gamma;

		currprefs.gfx_resolution = changed_prefs.gfx_resolution;
		currprefs.gfx_vresolution = changed_prefs.gfx_vresolution;
		currprefs.gfx_autoresolution_minh = changed_prefs.gfx_autoresolution_minh;
		currprefs.gfx_autoresolution_minv = changed_prefs.gfx_autoresolution_minv;
		currprefs.gfx_iscanlines = changed_prefs.gfx_iscanlines;
		currprefs.gfx_pscanlines = changed_prefs.gfx_pscanlines;
		//		currprefs.monitoremu = changed_prefs.monitoremu;

		currprefs.gfx_lores_mode = changed_prefs.gfx_lores_mode;
		currprefs.gfx_scandoubler = changed_prefs.gfx_scandoubler;
		currprefs.gfx_apmode[APMODE_NATIVE].gfx_display = changed_prefs.gfx_apmode[APMODE_NATIVE].gfx_display;
		currprefs.gfx_apmode[APMODE_RTG].gfx_display = changed_prefs.gfx_apmode[APMODE_RTG].gfx_display;
		currprefs.gfx_blackerthanblack = changed_prefs.gfx_blackerthanblack;
		currprefs.gfx_apmode[APMODE_NATIVE].gfx_backbuffers = changed_prefs.gfx_apmode[APMODE_NATIVE].gfx_backbuffers;
		currprefs.gfx_apmode[APMODE_NATIVE].gfx_interlaced = changed_prefs.gfx_apmode[APMODE_NATIVE].gfx_interlaced;
		currprefs.gfx_apmode[APMODE_RTG].gfx_backbuffers = changed_prefs.gfx_apmode[APMODE_RTG].gfx_backbuffers;

		bool unacquired = false;
		if (c & 64)
		{
			if (!unacquired)
			{
				inputdevice_unacquire();
				unacquired = true;
			}
			updatedisplayarea();
			/*DirectDraw_Fill(NULL, 0);
			DirectDraw_BlitToPrimary(NULL);*/
		}
		if (c & 256)
		{
			init_colors();
			reset_drawing();
		}
		if (c & 128)
		{
			if (currprefs.gfx_autoresolution)
			{
				c |= 2 | 8;
			}
			else
			{
				c |= 16;
				reset_drawing();
				//S2X_reset();
			}
		}
		if (c & 512)
		{
			reopen_gfx();
			graphics_mode_changed = 1;
		}
		if ((c & 16) || ((c & 8) && keepfsmode))
		{
			if (reopen(c & 2, unacquired == false))
			{
				c |= 2;
			}
			else
			{
				unacquired = true;
			}
			graphics_mode_changed = 1;
		}
		if ((c & 32) || ((c & 2) && !keepfsmode))
		{
			if (!unacquired)
			{
				inputdevice_unacquire();
				unacquired = true;
			}
			close_windows();
			graphics_init(dontcapture ? false : true);
			graphics_mode_changed = 1;
		}
		init_custom();
		if (c & 4)
		{
			pause_sound();
			reset_sound();
			resume_sound();
		}

		if (setpause || dontcapture)
		{
			if (!unacquired)
				inputdevice_unacquire();
			unacquired = false;
		}

		if (unacquired)
			inputdevice_acquire(TRUE);

		//if (setpause)
		//	setpaused(1);

		return 1;
	}

	bool changed = false;
	for (int i = 0; i < MAX_CHIPSET_REFRESH_TOTAL; i++)
	{
		if (currprefs.cr[i].rate != changed_prefs.cr[i].rate ||
			currprefs.cr[i].locked != changed_prefs.cr[i].locked)
		{
			memcpy(&currprefs.cr[i], &changed_prefs.cr[i], sizeof(struct chipset_refresh));
			changed = true;
		}
	}
	if (changed)
	{
		init_hz_full();
	}
	if (currprefs.chipset_refreshrate != changed_prefs.chipset_refreshrate)
	{
		currprefs.chipset_refreshrate = changed_prefs.chipset_refreshrate;
		init_hz_full();
		return 1;
	}

	if (currprefs.gf[0].gfx_filter_autoscale != changed_prefs.gf[0].gfx_filter_autoscale ||
		currprefs.gfx_xcenter_pos != changed_prefs.gfx_xcenter_pos ||
		currprefs.gfx_ycenter_pos != changed_prefs.gfx_ycenter_pos ||
		currprefs.gfx_xcenter_size != changed_prefs.gfx_xcenter_size ||
		currprefs.gfx_ycenter_size != changed_prefs.gfx_ycenter_size ||
		currprefs.gfx_xcenter != changed_prefs.gfx_xcenter ||
		currprefs.gfx_ycenter != changed_prefs.gfx_ycenter)
	{
		currprefs.gfx_xcenter_pos = changed_prefs.gfx_xcenter_pos;
		currprefs.gfx_ycenter_pos = changed_prefs.gfx_ycenter_pos;
		currprefs.gfx_xcenter_size = changed_prefs.gfx_xcenter_size;
		currprefs.gfx_ycenter_size = changed_prefs.gfx_ycenter_size;
		currprefs.gfx_xcenter = changed_prefs.gfx_xcenter;
		currprefs.gfx_ycenter = changed_prefs.gfx_ycenter;
		currprefs.gf[0].gfx_filter_autoscale = changed_prefs.gf[0].gfx_filter_autoscale;

		get_custom_limits(NULL, NULL, NULL, NULL, NULL);
		fixup_prefs_dimensions(&changed_prefs);

		return 1;
	}

	currprefs.filesys_limit = changed_prefs.filesys_limit;

	if (currprefs.leds_on_screen != changed_prefs.leds_on_screen ||
		currprefs.keyboard_leds[0] != changed_prefs.keyboard_leds[0] ||
		currprefs.keyboard_leds[1] != changed_prefs.keyboard_leds[1] ||
		currprefs.keyboard_leds[2] != changed_prefs.keyboard_leds[2])
	{
		currprefs.leds_on_screen = changed_prefs.leds_on_screen;
		currprefs.keyboard_leds[0] = changed_prefs.keyboard_leds[0];
		currprefs.keyboard_leds[1] = changed_prefs.keyboard_leds[1];
		currprefs.keyboard_leds[2] = changed_prefs.keyboard_leds[2];

		inputdevice_unacquire();
		currprefs.keyboard_leds_in_use = changed_prefs.keyboard_leds_in_use = (currprefs.keyboard_leds[0] | currprefs.keyboard_leds[1] | currprefs.keyboard_leds[2]) != 0;
		pause_sound();
		resume_sound();
		inputdevice_acquire(TRUE);

		return 1;
	}

	return 0;
}

/* Color management */

static xcolnr xcol8[4096];

static int red_bits, green_bits, blue_bits, alpha_bits;
static int red_shift, green_shift, blue_shift, alpha_shift;
static int alpha;

void init_colors()
{
	int i;
	int red_bits, green_bits, blue_bits;
	int red_shift, green_shift, blue_shift;

	/* Truecolor: */
	red_bits = bits_in_mask(screen->format->Rmask);
	green_bits = bits_in_mask(screen->format->Gmask);
	blue_bits = bits_in_mask(screen->format->Bmask);
	red_shift = bits_in_mask(screen->format->Rmask);
	green_shift = bits_in_mask(screen->format->Gmask);
	blue_shift = bits_in_mask(screen->format->Bmask);
	alpha_bits = 0;
	alpha_shift = 0;

	alloc_colors64k(red_bits, green_bits, blue_bits, red_shift, green_shift, blue_shift, alpha_bits, alpha_shift, alpha, 0);
	notice_new_xcolors();
	for (i = 0; i < 4096; i++)
		xcolors[i] = xcolors[i] * 0x00010001;
}

#ifdef PICASSO96

int picasso_palette()
{
	int i, changed;

	changed = 0;
	for (i = 0; i < 256; i++)
	{
		int r = picasso96_state.CLUT[i].Red;
		int g = picasso96_state.CLUT[i].Green;
		int b = picasso96_state.CLUT[i].Blue;
		uae_u32 v = (doMask256(r, red_bits, red_shift)
				| doMask256(g, green_bits, green_shift)
				| doMask256(b, blue_bits, blue_shift))
			| doMask256(0xff, alpha_bits, alpha_shift);
		if (v != picasso_vidinfo.clut[i])
		{
			//write_log (_T("%d:%08x\n"), i, v);
			picasso_vidinfo.clut[i] = v;
			changed = 1;
		}
	}
	return changed;
}

void DX_Invalidate(int x, int y, int width, int height)
{
	int last, lastx;

	if (width == 0 || height == 0)
		return;
	if (y < 0 || height < 0)
	{
		y = 0;
		height = picasso_vidinfo.height;
	}
	if (x < 0 || width < 0)
	{
		x = 0;
		width = picasso_vidinfo.width;
	}
	last = y + height - 1;
	lastx = x + width - 1;
	p96_double_buffer_first = y;
	p96_double_buffer_last = last;
	p96_double_buffer_firstx = x;
	p96_double_buffer_lastx = lastx;
	p96_double_buffer_needs_flushing = 1;
}

#endif

static void open_screen()
{
	close_windows();
	open_windows(true);
}

static int ifs(struct uae_prefs* p)
{
	int idx = screen_is_picasso ? 1 : 0;
	return p->gfx_apmode[idx].gfx_fullscreen == GFX_FULLSCREEN ? 1 : (p->gfx_apmode[idx].gfx_fullscreen == GFX_FULLWINDOW ? -1 : 0);
}

static int reopen(int full, bool unacquire)
{
	int quick = 0;
	int idx = screen_is_picasso ? 1 : 0;
	struct apmode* ap = picasso_on ? &currprefs.gfx_apmode[1] : &currprefs.gfx_apmode[0];

	updatewinfsmode(&changed_prefs);

	if (changed_prefs.gfx_apmode[0].gfx_fullscreen != currprefs.gfx_apmode[0].gfx_fullscreen && !screen_is_picasso)
		full = 1;
	if (changed_prefs.gfx_apmode[1].gfx_fullscreen != currprefs.gfx_apmode[1].gfx_fullscreen && screen_is_picasso)
		full = 1;

	/* fullscreen to fullscreen? */
	if (isfullscreen() > 0 && currprefs.gfx_apmode[0].gfx_fullscreen == changed_prefs.gfx_apmode[0].gfx_fullscreen &&
		currprefs.gfx_apmode[1].gfx_fullscreen == changed_prefs.gfx_apmode[1].gfx_fullscreen && currprefs.gfx_apmode[0].gfx_fullscreen == GFX_FULLSCREEN)
	{
		quick = 1;
	}
	/* windowed to windowed */
	if (isfullscreen() <= 0 && currprefs.gfx_apmode[0].gfx_fullscreen == changed_prefs.gfx_apmode[0].gfx_fullscreen &&
		currprefs.gfx_apmode[1].gfx_fullscreen == changed_prefs.gfx_apmode[1].gfx_fullscreen)
	{
		quick = 1;
	}

	currprefs.gfx_size_fs.width = changed_prefs.gfx_size_fs.width;
	currprefs.gfx_size_fs.height = changed_prefs.gfx_size_fs.height;
	currprefs.gfx_size_win.width = changed_prefs.gfx_size_win.width;
	currprefs.gfx_size_win.height = changed_prefs.gfx_size_win.height;
	currprefs.gfx_size_win.x = changed_prefs.gfx_size_win.x;
	currprefs.gfx_size_win.y = changed_prefs.gfx_size_win.y;
	currprefs.gfx_apmode[0].gfx_fullscreen = changed_prefs.gfx_apmode[0].gfx_fullscreen;
	currprefs.gfx_apmode[1].gfx_fullscreen = changed_prefs.gfx_apmode[1].gfx_fullscreen;
	currprefs.gfx_apmode[0].gfx_vsync = changed_prefs.gfx_apmode[0].gfx_vsync;
	currprefs.gfx_apmode[1].gfx_vsync = changed_prefs.gfx_apmode[1].gfx_vsync;
	currprefs.gfx_apmode[0].gfx_vsyncmode = changed_prefs.gfx_apmode[0].gfx_vsyncmode;
	currprefs.gfx_apmode[1].gfx_vsyncmode = changed_prefs.gfx_apmode[1].gfx_vsyncmode;
	currprefs.gfx_apmode[0].gfx_refreshrate = changed_prefs.gfx_apmode[0].gfx_refreshrate;
#if 0
	currprefs.gfx_apmode[1].gfx_refreshrate = changed_prefs.gfx_apmode[1].gfx_refreshrate;
#endif
	set_config_changed();

	if (!quick)
		return 1;

	if (unacquire)
	{
		inputdevice_unacquire();
	}

	reopen_gfx();

	return 0;
}

bool vsync_switchmode(int hz)
{
	static struct PicassoResolution* oldmode;
	static int oldhz;
	int w = currentmode->native_width;
	int h = currentmode->native_height;
	int d = currentmode->native_depth / 8;
	struct MultiDisplay* md = getdisplay(&currprefs);
	struct PicassoResolution* found;
	int newh, i, cnt;
	bool preferdouble = 0, preferlace = 0;
	bool lace = false;

	if (currprefs.gfx_apmode[APMODE_NATIVE].gfx_refreshrate > 85)
	{
		preferdouble = 1;
	}
	else if (currprefs.gfx_apmode[APMODE_NATIVE].gfx_interlaced)
	{
		preferlace = 1;
	}

	if (hz >= 55)
		hz = 60;
	else
		hz = 50;

	newh = h * (currprefs.ntscmode ? 60 : 50) / hz;

	found = NULL;
	for (cnt = 0; cnt <= abs(newh - h) + 1 && !found; cnt++)
	{
		for (int dbl = 0; dbl < 2 && !found; dbl++)
		{
			bool doublecheck = false;
			bool lacecheck = false;
			if (preferdouble && dbl == 0)
				doublecheck = true;
			else if (preferlace && dbl == 0)
				lacecheck = true;

			for (int extra = 1; extra >= -1 && !found; extra--)
			{
				for (i = 0; md->DisplayModes[i].depth >= 0 && !found; i++)
				{
					struct PicassoResolution* r = &md->DisplayModes[i];
					if (r->res.width == w && (r->res.height == newh + cnt || r->res.height == newh - cnt) && r->depth == d)
					{
						int j;
						for (j = 0; r->refresh[j] > 0; j++)
						{
							if (doublecheck)
							{
								if (r->refreshtype[j] & REFRESH_RATE_LACE)
									continue;
								if (r->refresh[j] == hz * 2 + extra)
								{
									found = r;
									hz = r->refresh[j];
									break;
								}
							}
							else if (lacecheck)
							{
								if (!(r->refreshtype[j] & REFRESH_RATE_LACE))
									continue;
								if (r->refresh[j] * 2 == hz + extra)
								{
									found = r;
									lace = true;
									hz = r->refresh[j];
									break;
								}
							}
							else
							{
								if (r->refresh[j] == hz + extra)
								{
									found = r;
									hz = r->refresh[j];
									break;
								}
							}
						}
					}
				}
			}
		}
	}
	if (found == oldmode && hz == oldhz)
		return true;
	oldmode = found;
	oldhz = hz;
	if (!found)
	{
		changed_prefs.gfx_apmode[APMODE_NATIVE].gfx_vsync = 0;
		if (currprefs.gfx_apmode[APMODE_NATIVE].gfx_vsync != changed_prefs.gfx_apmode[APMODE_NATIVE].gfx_vsync)
		{
			set_config_changed();
		}
		write_log(_T("refresh rate changed to %d%s but no matching screenmode found, vsync disabled\n"), hz, lace ? _T("i") : _T("p"));
		return false;
	}
	else
	{
		newh = found->res.height;
		changed_prefs.gfx_size_fs.height = newh;
		changed_prefs.gfx_apmode[APMODE_NATIVE].gfx_refreshrate = hz;
		changed_prefs.gfx_apmode[APMODE_NATIVE].gfx_interlaced = lace;
		if (changed_prefs.gfx_size_fs.height != currprefs.gfx_size_fs.height ||
			changed_prefs.gfx_apmode[APMODE_NATIVE].gfx_refreshrate != currprefs.gfx_apmode[APMODE_NATIVE].gfx_refreshrate)
		{
			write_log(_T("refresh rate changed to %d%s, new screenmode %dx%d\n"), hz, lace ? _T("i") : _T("p"), w, newh);
			set_config_changed();
		}
		return true;
	}
}

#ifdef PICASSO96

static int modeswitchneeded(struct winuae_currentmode* wc)
{
	if (isfullscreen() > 0)
	{
		/* fullscreen to fullscreen */
		if (screen_is_picasso)
		{
			if (picasso96_state.BytesPerPixel > 1 && picasso96_state.BytesPerPixel * 8 != wc->current_depth)
				return -1;
			if (picasso96_state.Width < wc->current_width && picasso96_state.Height < wc->current_height)
			{
				if ((currprefs.gf[1].gfx_filter_autoscale == 1 || (currprefs.gf[1].gfx_filter_autoscale == 2)))
					return 0;
			}
			if (picasso96_state.Width != wc->current_width ||
				picasso96_state.Height != wc->current_height)
				return 1;
			if (picasso96_state.Width == wc->current_width &&
				picasso96_state.Height == wc->current_height)
			{
				if (picasso96_state.BytesPerPixel * 8 == wc->current_depth || picasso96_state.BytesPerPixel == 1)
					return 0;
				/*if (!currprefs.win32_rtgmatchdepth)
					return 0;*/
			}
			return 1;
		}
		else
		{
			if (currentmode->current_width != wc->current_width ||
				currentmode->current_height != wc->current_height ||
				currentmode->current_depth != wc->current_depth)
				return -1;
			if (!gfxvidinfo.outbuffer->bufmem_lockable)
				return -1;
		}
	}
	else if (isfullscreen() == 0)
	{
		/* windowed to windowed */
		return -1;
	}
	else
	{
		/* fullwindow to fullwindow */
		updatedisplayarea();
		/*DirectDraw_Fill(NULL, 0);
		DirectDraw_BlitToPrimary(NULL);*/
		if (screen_is_picasso)
		{
			if (currprefs.gf[1].gfx_filter_autoscale && ((wc->native_width > picasso96_state.Width && wc->native_height >= picasso96_state.Height) || (wc->native_height > picasso96_state.Height && wc->native_width >= picasso96_state.Width)))
				return -1;
			if ((picasso96_state.Width != wc->native_width || picasso96_state.Height != wc->native_height))
				return -1;
		}
		return -1;
	}
	return 0;
}

void gfx_set_picasso_state(int on)
{
	struct winuae_currentmode wc;
	struct apmode *newmode, *oldmode;
	int mode;

	if (screen_is_picasso == on)
		return;
	screen_is_picasso = on;

	memcpy(&wc, currentmode, sizeof(wc));

	newmode = &currprefs.gfx_apmode[on ? 1 : 0];
	oldmode = &currprefs.gfx_apmode[on ? 0 : 1];

	updatemodes();
	update_gfxparams();
	clearscreen();

	if (newmode->gfx_fullscreen != oldmode->gfx_fullscreen ||
	(newmode->gfx_fullscreen && (
		newmode->gfx_backbuffers != oldmode->gfx_backbuffers ||
		newmode->gfx_display != oldmode->gfx_display ||
		newmode->gfx_refreshrate != oldmode->gfx_refreshrate ||
		newmode->gfx_strobo != oldmode->gfx_strobo ||
		newmode->gfx_vflip != oldmode->gfx_vflip ||
		newmode->gfx_vsync != oldmode->gfx_vsync)))
	{
		mode = 1;
	}
	else
	{
		mode = modeswitchneeded(&wc);
		if (!mode)
			goto end;
	}
	if (mode < 0)
	{
		open_windows(true);
	}
	else
	{
		open_screen(); // reopen everything
	}
	//if (on && isvsync_rtg() < 0)
	//	vblank_calibrate(0, false);
end:
	return;
#ifdef RETROPLATFORM
	rp_set_hwnd(hAmigaWnd);
#endif
}

void gfx_set_picasso_modeinfo(uae_u32 w, uae_u32 h, uae_u32 depth, RGBFTYPE rgbfmt)
{
	int need;
	if (!screen_is_picasso)
		return;
	clearscreen();
	gfx_set_picasso_colors(rgbfmt);
	updatemodes();
	need = modeswitchneeded(currentmode);
	update_gfxparams();
	if (need > 0)
	{
		open_screen();
	}
	else if (need < 0)
	{
		open_windows(true);
	}
#ifdef RETROPLATFORM
	rp_set_hwnd(hAmigaWnd);
#endif
}

#endif

void gfx_set_picasso_colors(RGBFTYPE rgbfmt)
{
	alloc_colors_picasso(red_bits, green_bits, blue_bits, red_shift, green_shift, blue_shift, rgbfmt);
}

static void gfxmode_reset(void)
{
#ifdef GFXFILTER
	usedfilter = 0;
	if (currprefs.gf[picasso_on].gfx_filter > 0) {
		int i = 0;
		while (uaefilters[i].name) {
			if (uaefilters[i].type == currprefs.gf[picasso_on].gfx_filter) {
				usedfilter = &uaefilters[i];
				break;
			}
			i++;
		}
	}
#endif
}

int machdep_init()
{
	picasso_requested_on = 0;
	picasso_on = 0;
	screen_is_picasso = 0;
	memset(currentmode, 0, sizeof(*currentmode));
#ifdef LOGITECHLCD
	lcd_open();
#endif
	//systray(hHiddenWnd, FALSE);
	return 1;
}

void machdep_free(void)
{
#ifdef LOGITECHLCD
	lcd_close();
#endif
}

int graphics_init(bool mousecapture)
{
	//systray(hHiddenWnd, TRUE);
	//systray(hHiddenWnd, FALSE);
	gfxmode_reset();
	graphics_mode_changed = 1;
	return open_windows(mousecapture);
}

int graphics_setup(void)
{
#ifdef PICASSO96
	InitPicasso96();
#endif
	return 1;
}

void graphics_subshutdown()
{
	if (screen != nullptr)
	{
		SDL_FreeSurface(screen);
		screen = nullptr;
	}
	if (texture != nullptr)
	{
		SDL_DestroyTexture(texture);
	}
}

void graphics_leave(void)
{
	changevblankthreadmode(VBLANKTH_KILL);
	close_windows();
}

void close_windows(void)
{
	reset_sound();
	freevidbuffer(&gfxvidinfo.drawbuffer);
	freevidbuffer(&gfxvidinfo.tempbuffer);
	graphics_subshutdown();

	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(sdlWindow);
	SDL_VideoQuit();
}

static int getbestmode(int nextbest)
{
	int i, startidx;
	struct MultiDisplay* md;
	int ratio;
	int index = -1;

	for (;;)
	{
		md = getdisplay2(&currprefs, index);
		if (!md)
			return 0;
		ratio = currentmode->native_width > currentmode->native_height ? 1 : 0;
		for (i = 0; md->DisplayModes[i].depth >= 0; i++)
		{
			struct PicassoResolution* pr = &md->DisplayModes[i];
			if (pr->res.width == currentmode->native_width && pr->res.height == currentmode->native_height)
				break;
		}
		if (md->DisplayModes[i].depth >= 0)
		{
			if (!nextbest)
				break;
			while (md->DisplayModes[i].res.width == currentmode->native_width && md->DisplayModes[i].res.height == currentmode->native_height)
				i++;
		}
		else
		{
			i = 0;
		}
		// first iterate only modes that have similar aspect ratio
		startidx = i;
		for (; md->DisplayModes[i].depth >= 0; i++)
		{
			struct PicassoResolution* pr = &md->DisplayModes[i];
			int r = pr->res.width > pr->res.height ? 1 : 0;
			if (pr->res.width >= currentmode->native_width && pr->res.height >= currentmode->native_height && r == ratio)
			{
				write_log(_T("FS: %dx%d -> %dx%d %d %d\n"), currentmode->native_width, currentmode->native_height,
					pr->res.width, pr->res.height, ratio, index);
				currentmode->native_width = pr->res.width;
				currentmode->native_height = pr->res.height;
				currentmode->current_width = currentmode->native_width;
				currentmode->current_height = currentmode->native_height;
				goto end;
			}
		}
		// still not match? check all modes
		i = startidx;
		for (; md->DisplayModes[i].depth >= 0; i++)
		{
			struct PicassoResolution* pr = &md->DisplayModes[i];
			int r = pr->res.width > pr->res.height ? 1 : 0;
			if (pr->res.width >= currentmode->native_width && pr->res.height >= currentmode->native_height)
			{
				write_log(_T("FS: %dx%d -> %dx%d\n"), currentmode->native_width, currentmode->native_height,
					pr->res.width, pr->res.height);
				currentmode->native_width = pr->res.width;
				currentmode->native_height = pr->res.height;
				currentmode->current_width = currentmode->native_width;
				currentmode->current_height = currentmode->native_height;
				goto end;
			}
		}
		index++;
	}
end:
	if (index >= 0)
	{
		currprefs.gfx_apmode[screen_is_picasso ? APMODE_RTG : APMODE_NATIVE].gfx_display =
			changed_prefs.gfx_apmode[screen_is_picasso ? APMODE_RTG : APMODE_NATIVE].gfx_display = index;
		//write_log(L"Can't find mode %dx%d ->\n", currentmode->native_width, currentmode->native_height);
		//write_log(L"Monitor switched to '%s'\n", md->adaptername);
	}
	return 1;
}

static int create_windows_2()
{
	static bool firstwindow = true;
	static int prevsbheight;
	int dxfs = 1;
	int fsw = 0;

	int borderless = 0;

	int gap = 0;
	int x, y, w, h;
	struct MultiDisplay* md = getdisplay(&currprefs);
	int sbheight = 0;

	if (sdlWindow)
	{
		SDL_Rect  r;
		int w, h, x, y;
		int nw, nh, nx, ny;
		SDL_GetDisplayBounds(0, &r);

		x = r.x;
		y = r.y;
		w = r.w;
		h = r.h;
		nx = x;
		ny = y;

		if (screen_is_picasso)
		{
			nw = currentmode->current_width;
			nh = currentmode->current_height;
			SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear"); // make the scaled rendering look smoother.
		}
		else
		{
			nw = currprefs.gfx_size_win.width;
			nh = currprefs.gfx_size_win.height;
			SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
		}

		//TODO: probably not needed?
		//if (fsw || dxfs)
		//{
		//	RECT rc = md->rect;
		//	nx = rc.left;
		//	ny = rc.top;
		//	nw = rc.right - rc.left;
		//	nh = rc.bottom - rc.top;
		//}

		w = nw;
		h = nh;
		x = nx;
		y = ny;

		SDL_GetDisplayBounds(0, &mainwin_rect);

		screen = SDL_CreateRGBSurface(0, w, h, 32, 0, 0, 0, 0);
		check_error_sdl(screen == nullptr, "Unable to create a surface");

		SDL_RenderSetLogicalSize(renderer, w, h);

		// Initialize SDL Texture for the renderer
		texture = SDL_CreateTexture(renderer,
			SDL_PIXELFORMAT_RGB565,
			SDL_TEXTUREACCESS_STREAMING,
			w,
			h);
		check_error_sdl(texture == nullptr, "Unable to create texture");
		
		prevsbheight = sbheight;
		return 1;
	}

	window_led_drives = 0;
	window_led_drives_end = 0;

	x = 0;
	y = 0;

	RECT rc;
	getbestmode(0);
	w = currentmode->native_width;
	h = currentmode->native_height;
	rc = md->rect;
	if (rc.left >= 0)
		x = rc.left;
	else
		x = rc.left + (rc.right - rc.left - w);
	if (rc.top >= 0)
		y = rc.top;
	else
		y = rc.top + (rc.bottom - rc.top - h);

	if (!sdlWindow)
	{
		write_log(_T("creation of amiga window failed\n"));
		close_hwnds();
		return 0;
	}

	SDL_GetDisplayBounds(0, &mainwin_rect);

	updatedisplayarea();

	firstwindow = false;
	prevsbheight = sbheight;
	return 1;
}

static int set_ddraw()
{
	int cnt, ret;

	cnt = 3;
	for (;;)
	{
		ret = set_ddraw_2();
		if (cnt-- <= 0)
			return 0;
		if (ret < 0)
		{
			getbestmode(1);
			continue;
		}
		if (ret == 0)
			return 0;
		break;
	}
	return 1;
}

static void allocsoftbuffer(const TCHAR* name, struct vidbuffer* buf, int flags, int width, int height, int depth)
{
	buf->pixbytes = (depth + 7) / 8;
	buf->width_allocated = (width + 7) & ~7;
	buf->height_allocated = height;

	if (!(flags & DM_SWSCALE))
	{
		if (buf != &gfxvidinfo.drawbuffer)
			return;

		buf->bufmem = NULL;
		buf->bufmemend = NULL;
		buf->realbufmem = NULL;
		buf->bufmem_allocated = NULL;
		buf->bufmem_lockable = true;

		write_log(_T("Reserved %s temp buffer (%d*%d*%d)\n"), name, width, height, depth);
	}
	else if (flags & DM_SWSCALE)
	{
		int w = buf->width_allocated;
		int h = buf->height_allocated;
		int size = (w * 2) * (h * 2) * buf->pixbytes;
		buf->rowbytes = w * 2 * buf->pixbytes;
		buf->realbufmem = xcalloc(uae_u8, size);
		buf->bufmem_allocated = buf->bufmem = buf->realbufmem + (h / 2) * buf->rowbytes + (w / 2) * buf->pixbytes;
		buf->bufmemend = buf->realbufmem + size - buf->rowbytes;
		buf->bufmem_lockable = true;

		write_log(_T("Allocated %s temp buffer (%d*%d*%d) = %p\n"), name, width, height, depth, buf->realbufmem);
	}
}

static int create_windows()
{
	if (!create_windows_2())
		return 0;

	return set_ddraw();
}

static int oldtex_w, oldtex_h, oldtex_rtg;

static BOOL doInit()
{
	int fs_warning = -1;
	TCHAR tmpstr[300];
	int tmp_depth;
	int ret = 0;

	remembered_vblank = -1;
	if (wasfullwindow_a == 0)
		wasfullwindow_a = currprefs.gfx_apmode[0].gfx_fullscreen == GFX_FULLWINDOW ? 1 : -1;
	if (wasfullwindow_p == 0)
		wasfullwindow_p = currprefs.gfx_apmode[1].gfx_fullscreen == GFX_FULLWINDOW ? 1 : -1;
	gfxmode_reset();
	freevidbuffer(&gfxvidinfo.drawbuffer);
	freevidbuffer(&gfxvidinfo.tempbuffer);

	for (;;)
	{
		updatemodes();
		currentmode->native_depth = 0;
		tmp_depth = currentmode->current_depth;

		if (currentmode->flags & DM_W_FULLSCREEN)
		{
			RECT rc = getdisplay(&currprefs)->rect;
			currentmode->native_width = rc.right - rc.left;
			currentmode->native_height = rc.bottom - rc.top;
		}

		if (fs_warning >= 0 && isfullscreen() <= 0)
		{
			if (screen_is_picasso)
				changed_prefs.gfx_apmode[1].gfx_fullscreen = currprefs.gfx_apmode[1].gfx_fullscreen = GFX_FULLSCREEN;
			else
				changed_prefs.gfx_apmode[0].gfx_fullscreen = currprefs.gfx_apmode[0].gfx_fullscreen = GFX_FULLSCREEN;
			updatewinfsmode(&currprefs);
			updatewinfsmode(&changed_prefs);
			currentmode->current_depth = tmp_depth;
			updatemodes();
			ret = -2;
			goto oops;
		}
		if (!create_windows())
			goto oops;
#ifdef PICASSO96
		if (screen_is_picasso)
		{
			break;
		}
		else
		{
#endif
			currentmode->native_depth = currentmode->current_depth;

			if (currprefs.gfx_resolution > gfxvidinfo.gfx_resolution_reserved)
				gfxvidinfo.gfx_resolution_reserved = currprefs.gfx_resolution;
			if (currprefs.gfx_vresolution > gfxvidinfo.gfx_vresolution_reserved)
				gfxvidinfo.gfx_vresolution_reserved = currprefs.gfx_vresolution;

			//gfxvidinfo.drawbuffer.gfx_resolution_reserved = RES_SUPERHIRES;

#if defined (GFXFILTER)
			if (currentmode->flags & (DM_D3D | DM_SWSCALE)) {
				if (!currprefs.gfx_autoresolution) {
					currentmode->amiga_width = AMIGA_WIDTH_MAX << currprefs.gfx_resolution;
					currentmode->amiga_height = AMIGA_HEIGHT_MAX << currprefs.gfx_vresolution;
				}
				else {
					currentmode->amiga_width = AMIGA_WIDTH_MAX << gfxvidinfo.gfx_resolution_reserved;
					currentmode->amiga_height = AMIGA_HEIGHT_MAX << gfxvidinfo.gfx_vresolution_reserved;
				}
				if (gfxvidinfo.gfx_resolution_reserved == RES_SUPERHIRES)
					currentmode->amiga_height *= 2;
				if (currentmode->amiga_height > 1280)
					currentmode->amiga_height = 1280;

				gfxvidinfo.drawbuffer.inwidth = gfxvidinfo.drawbuffer.outwidth = currentmode->amiga_width;
				gfxvidinfo.drawbuffer.inheight = gfxvidinfo.drawbuffer.outheight = currentmode->amiga_height;

				if (usedfilter) {
					if ((usedfilter->flags & (UAE_FILTER_MODE_16 | UAE_FILTER_MODE_32)) == (UAE_FILTER_MODE_16 | UAE_FILTER_MODE_32)) {
						currentmode->current_depth = currentmode->native_depth;
					}
					else {
						currentmode->current_depth = (usedfilter->flags & UAE_FILTER_MODE_32) ? 32 : 16;
					}
				}
				currentmode->pitch = currentmode->amiga_width * currentmode->current_depth >> 3;
			}
			else
#endif
			{
				currentmode->amiga_width = currentmode->current_width;
				currentmode->amiga_height = currentmode->current_height;
			}
			gfxvidinfo.drawbuffer.pixbytes = currentmode->current_depth >> 3;
			gfxvidinfo.drawbuffer.bufmem = NULL;
			gfxvidinfo.drawbuffer.linemem = NULL;
			gfxvidinfo.maxblocklines = 0; // flush_screen actually does everything
			gfxvidinfo.drawbuffer.rowbytes = currentmode->pitch;
			break;
#ifdef PICASSO96
		}
#endif
	}

#ifdef PICASSO96
	picasso_vidinfo.rowbytes = 0;
	picasso_vidinfo.pixbytes = currentmode->current_depth / 8;
	picasso_vidinfo.rgbformat = 0;
	picasso_vidinfo.extra_mem = 1;
	picasso_vidinfo.height = currentmode->current_height;
	picasso_vidinfo.width = currentmode->current_width;
	picasso_vidinfo.depth = currentmode->current_depth;
	picasso_vidinfo.offset = 0;
#endif
	if (!scrlinebuf)
		scrlinebuf = xmalloc(uae_u8, max_uae_width * 4);

	gfxvidinfo.drawbuffer.emergmem = scrlinebuf; // memcpy from system-memory to video-memory

	gfxvidinfo.drawbuffer.realbufmem = NULL;
	gfxvidinfo.drawbuffer.bufmem = NULL;
	gfxvidinfo.drawbuffer.bufmem_allocated = NULL;
	gfxvidinfo.drawbuffer.bufmem_lockable = false;

	gfxvidinfo.outbuffer = &gfxvidinfo.drawbuffer;
	gfxvidinfo.inbuffer = &gfxvidinfo.drawbuffer;

	if (!screen_is_picasso)
	{
		if (currprefs.gfx_api == 0 && currprefs.gf[0].gfx_filter == 0)
		{
			allocsoftbuffer(_T("draw"), &gfxvidinfo.drawbuffer, currentmode->flags,
			                          currentmode->native_width, currentmode->native_height, currentmode->current_depth);
		}
		else
		{
			allocsoftbuffer(_T("draw"), &gfxvidinfo.drawbuffer, currentmode->flags,
			                          1600, 1280, currentmode->current_depth);
		}
		if (currprefs.monitoremu)
		{
			allocsoftbuffer(_T("monemu"), &gfxvidinfo.tempbuffer, currentmode->flags,
			                            currentmode->amiga_width > 1024 ? currentmode->amiga_width : 1024,
			                            currentmode->amiga_height > 1024 ? currentmode->amiga_height : 1024,
			                            currentmode->current_depth);
		}

		init_row_map();
	}
	init_colors();

	oldtex_w = oldtex_h = -1;

	//TOOD: Check if initialization of SDL Surface and Texture are needed here?

	target_graphics_buffer_update();

	screen_is_initialized = 1;
	picasso_refresh();
#ifdef RETROPLATFORM
	rp_set_hwnd_delayed();
#endif

	return 1;

oops:
	close_hwnds();
	return ret;
}

bool target_graphics_buffer_update()
{
	static bool	graphicsbuffer_retry;
	int w, h;

	graphicsbuffer_retry = false;
	if (screen_is_picasso) {
		w = picasso96_state.Width > picasso_vidinfo.width ? picasso96_state.Width : picasso_vidinfo.width;
		h = picasso96_state.Height > picasso_vidinfo.height ? picasso96_state.Height : picasso_vidinfo.height;
	}
	else {
		struct vidbuffer *vb = gfxvidinfo.drawbuffer.tempbufferinuse ? &gfxvidinfo.tempbuffer : &gfxvidinfo.drawbuffer;
		gfxvidinfo.outbuffer = vb;
		w = vb->outwidth;
		h = vb->outheight;
	}

	if (oldtex_w == w && oldtex_h == h && oldtex_rtg == screen_is_picasso)
		return false;

	if (!w || !h) {
		oldtex_w = w;
		oldtex_h = h;
		oldtex_rtg = screen_is_picasso;
		return false;
	}

	SDL_DestroyTexture(texture);
	texture = SDL_CreateTexture(renderer,
		SDL_PIXELFORMAT_ARGB8888,
		SDL_TEXTUREACCESS_STREAMING,
		w,
		h);
	if (texture == nullptr)
		return false;

	oldtex_w = w;
	oldtex_h = h;
	oldtex_rtg = screen_is_picasso;

	return true;
}

void updatedisplayarea()
{
	// Update the texture from the surface
	SDL_UpdateTexture(texture, nullptr, screen->pixels, screen->pitch);
	SDL_RenderClear(renderer);
	// Copy the texture on the renderer
	SDL_RenderCopy(renderer, texture, nullptr, nullptr);
	// Update the window surface (show the renderer)
	SDL_RenderPresent(renderer);
}

void updatewinfsmode(struct uae_prefs* p)
{
	struct MultiDisplay* md;

	fixup_prefs_dimensions(p);
	p->gfx_size = p->gfx_size_fs;

	md = getdisplay(p);
	set_config_changed();
}

bool toggle_rtg(int mode)
{
	if (mode == 0)
	{
		if (!picasso_on)
			return false;
	}
	else if (mode > 0)
	{
		if (picasso_on)
			return false;
	}
	if (currprefs.rtgmem_type >= GFXBOARD_HARDWARE)
	{
		return gfxboard_toggle(mode);
	}
	else
	{
		// can always switch from RTG to custom
		if (picasso_requested_on && picasso_on)
		{
			picasso_requested_on = false;
			return true;
		}
		if (picasso_on)
			return false;
		// can only switch from custom to RTG if there is some mode active
		if (picasso_is_active())
		{
			picasso_requested_on = true;
			return true;
		}
	}
	return false;
}

void toggle_fullscreen(int mode)
{
	int* p = picasso_on ? &changed_prefs.gfx_apmode[1].gfx_fullscreen : &changed_prefs.gfx_apmode[0].gfx_fullscreen;
	int wfw = picasso_on ? wasfullwindow_p : wasfullwindow_a;
	int v = *p;

	if (mode < 0)
	{
		// fullscreen <> window (if in fullwindow: fullwindow <> fullscreen)
		if (v == GFX_FULLWINDOW)
			v = GFX_FULLSCREEN;
		else if (v == GFX_WINDOW)
			v = GFX_FULLSCREEN;
		else if (v == GFX_FULLSCREEN)
			if (wfw > 0)
				v = GFX_FULLWINDOW;
			else
				v = GFX_WINDOW;
	}
	else if (mode == 0)
	{
		// fullscreen <> window
		if (v == GFX_FULLSCREEN)
			v = GFX_WINDOW;
		else
			v = GFX_FULLSCREEN;
	}
	else if (mode == 1)
	{
		// fullscreen <> fullwindow
		if (v == GFX_FULLSCREEN)
			v = GFX_FULLWINDOW;
		else
			v = GFX_FULLSCREEN;
	}
	else if (mode == 2)
	{
		// window <> fullwindow
		if (v == GFX_FULLWINDOW)
			v = GFX_WINDOW;
		else
			v = GFX_FULLWINDOW;
	}
	*p = v;
	updatewinfsmode(&changed_prefs);
}

int GetSurfacePixelFormat()
{
	SDL_PixelFormat *fmt;
	fmt = screen->format;
	
	Uint32 r = fmt->Rmask;
	Uint32 g = fmt->Gmask;
	Uint32 b = fmt->Bmask;
	switch (fmt->BitsPerPixel)
	{
	case 8:
		break;

	case 16:
		if (r == 0xF800 && g == 0x07E0 && b == 0x001F)
			return RGBFB_R5G6B5PC;
		if (r == 0x7C00 && g == 0x03E0 && b == 0x001F)
			return RGBFB_R5G5B5PC;
		if (b == 0xF800 && g == 0x07E0 && r == 0x001F)
			return RGBFB_B5G6R5PC;
		if (b == 0x7C00 && g == 0x03E0 && r == 0x001F)
			return RGBFB_B5G5R5PC;
		break;

	case 24:
		if (r == 0xFF0000 && g == 0x00FF00 && b == 0x0000FF)
			return RGBFB_B8G8R8;
		if (r == 0x0000FF && g == 0x00FF00 && b == 0xFF0000)
			return RGBFB_R8G8B8;
		break;

	case 32:
		if (r == 0x00FF0000 && g == 0x0000FF00 && b == 0x000000FF)
			return RGBFB_B8G8R8A8;
		if (r == 0x000000FF && g == 0x0000FF00 && b == 0x00FF0000)
			return RGBFB_R8G8B8A8;
		if (r == 0xFF000000 && g == 0x00FF0000 && b == 0x0000FF00)
			return RGBFB_A8B8G8R8;
		if (r == 0x0000FF00 && g == 0x00FF0000 && b == 0xFF000000)
			return RGBFB_A8R8G8B8;
		break;

	default:
		//write_log(_T("Unknown %d bit format %d %d %d\n"), pfp->dwRGBBitCount, r, g, b);
		break;
	}
	return RGBFB_NONE;
}