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

/* SDL variable for output of emulation */
SDL_Surface* screen = nullptr;

/* Possible screen modes (x and y resolutions) */
#define MAX_SCREEN_MODES 11
static int x_size_table[MAX_SCREEN_MODES] = {640, 640, 720, 800, 800, 960, 1024, 1280, 1280, 1680, 1920};
static int y_size_table[MAX_SCREEN_MODES] = {400, 480, 400, 480, 600, 540, 768, 720, 800, 1050, 1080};

struct winuae_currentmode {
	unsigned int flags;
	int native_width, native_height, native_depth, pitch;
	int current_width, current_height, current_depth;
	int amiga_width, amiga_height;
	int initdone;
	int fullfill;
	int vsync;
	int freq;
};

struct PicassoResolution* DisplayModes;
struct MultiDisplay Displays[MAX_DISPLAYS + 1];

static struct winuae_currentmode currentmodestruct;
static int screen_is_initialized;
static int display_change_requested;
int window_led_drives, window_led_drives_end;
int window_led_hd, window_led_hd_end;
int window_led_joys, window_led_joys_end, window_led_joy_start;
int window_led_msg, window_led_msg_end, window_led_msg_start;
extern int console_logging;
int window_extra_width, window_extra_height;

static struct winuae_currentmode *currentmode = &currentmodestruct;
static int wasfullwindow_a, wasfullwindow_p;

static int vblankbasewait1, vblankbasewait2, vblankbasewait3, vblankbasefull, vblankbaseadjust;
static bool vblankbaselace;
static int vblankbaselace_chipset;
static bool vblankthread_oddeven, vblankthread_oddeven_got;
static int graphics_mode_changed;
int vsync_modechangetimeout = 10;

int screen_is_picasso = 0;

static SDL_Surface* current_screenshot = nullptr;
static char screenshot_filename_default[255] =
{
	'/', 't', 'm', 'p', '/', 'n', 'u', 'l', 'l', '.', 'p', 'n', 'g', '\0'
};
char* screenshot_filename = static_cast<char *>(&screenshot_filename_default[0]);
FILE* screenshot_file = nullptr;
static void CreateScreenshot();
static int save_thumb(char* path);
int delay_savestate_frame = 0;

int graphics_setup(void)
{
#ifdef PICASSO96
	picasso_InitResolutions();
	InitPicasso96();
#endif
	return 1;
}

void InitAmigaVidMode(struct uae_prefs* p)
{
	/* Initialize structure for Amiga video modes */
	gfxvidinfo.pixbytes = 2;
	gfxvidinfo.bufmem = static_cast<uae_u8 *>(screen->pixels);
	gfxvidinfo.outwidth = screen->w ? screen->w : 320; //p->gfx_size.width;
	gfxvidinfo.outheight = screen->h ? screen->h : 256; //p->gfx_size.height;
	gfxvidinfo.rowbytes = screen->pitch;
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

static void open_screen(struct uae_prefs* p)
{
	int width;
	int height;

#ifdef PICASSO96
	if (screen_is_picasso)
	{
		width = picasso_vidinfo.width;
		height = picasso_vidinfo.height;
		SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear"); // make the scaled rendering look smoother.
	}
	else
#endif
	{
		p->gfx_resolution = p->gfx_size.width > 600 ? 1 : 0;
		width = p->gfx_size.width;
		height = p->gfx_size.height;
	}

	graphics_subshutdown();

	screen = SDL_CreateRGBSurface(0, width, height, 16, 0, 0, 0, 0);
	check_error_sdl(screen == nullptr, "Unable to create a surface");

	SDL_RenderSetLogicalSize(renderer, width, height);

	// Initialize SDL Texture for the renderer
	texture = SDL_CreateTexture(renderer,
	                            SDL_PIXELFORMAT_RGB565,
	                            SDL_TEXTUREACCESS_STREAMING,
	                            width,
	                            height);
	check_error_sdl(texture == nullptr, "Unable to create texture");

	// Update the texture from the surface
	SDL_UpdateTexture(texture, nullptr, screen->pixels, screen->pitch);
	SDL_RenderClear(renderer);
	// Copy the texture on the renderer
	SDL_RenderCopy(renderer, texture, nullptr, nullptr);
	// Update the window surface (show the renderer)
	SDL_RenderPresent(renderer);

	if (screen != nullptr)
	{
		InitAmigaVidMode(p);
		init_row_map();
	}
}

void update_display(struct uae_prefs* p)
{
	open_screen(p);
	SDL_ShowCursor(SDL_DISABLE);
	framecnt = 1; // Don't draw frame before reset done
}

int check_prefs_changed_gfx()
{
	int changed = 0;

	if (currprefs.gfx_size.height != changed_prefs.gfx_size.height ||
		currprefs.gfx_size.width != changed_prefs.gfx_size.width ||
		currprefs.gfx_size_fs.width != changed_prefs.gfx_size_fs.width ||
		currprefs.gfx_resolution != changed_prefs.gfx_resolution)
	{
		cfgfile_configuration_change(1);
		currprefs.gfx_size.height = changed_prefs.gfx_size.height;
		currprefs.gfx_size.width = changed_prefs.gfx_size.width;
		currprefs.gfx_size_fs.width = changed_prefs.gfx_size_fs.width;
		currprefs.gfx_resolution = changed_prefs.gfx_resolution;
		update_display(&currprefs);
		changed = 1;
	}
	if (currprefs.leds_on_screen != changed_prefs.leds_on_screen ||
		currprefs.pandora_hide_idle_led != changed_prefs.pandora_hide_idle_led ||
		currprefs.pandora_vertical_offset != changed_prefs.pandora_vertical_offset)
	{
		currprefs.leds_on_screen = changed_prefs.leds_on_screen;
		currprefs.pandora_hide_idle_led = changed_prefs.pandora_hide_idle_led;
		currprefs.pandora_vertical_offset = changed_prefs.pandora_vertical_offset;
		changed = 1;
	}
	if (currprefs.chipset_refreshrate != changed_prefs.chipset_refreshrate)
	{
		currprefs.chipset_refreshrate = changed_prefs.chipset_refreshrate;
		init_hz_normal();
		changed = 1;
	}

	currprefs.filesys_limit = changed_prefs.filesys_limit;

	return changed;
}


int lockscr()
{
	SDL_LockSurface(screen);
	return 1;
}


void unlockscr()
{
	SDL_UnlockSurface(screen);
}


void wait_for_vsync()
{
}

static int flushymin, flushymax;
#define FLUSH_DIFF 50

static void flushit(struct vidbuffer *vb, int lineno)
{
	if (!currprefs.gfx_api)
		return;
	if (flushymin > lineno) {
		if (flushymin - lineno > FLUSH_DIFF && flushymax != 0) {
			D3D_flushtexture(flushymin, flushymax);
			flushymin = currentmode->amiga_height;
			flushymax = 0;
		}
		else {
			flushymin = lineno;
		}
	}
	if (flushymax < lineno) {
		if (lineno - flushymax > FLUSH_DIFF && flushymax != 0) {
			D3D_flushtexture(flushymin, flushymax);
			flushymin = currentmode->amiga_height;
			flushymax = 0;
		}
		else {
			flushymax = lineno;
		}
	}
}

void flush_line(struct vidbuffer *vb, int lineno)
{
	flushit(vb, lineno);
}

void flush_block(struct vidbuffer *vb, int first, int last)
{
	flushit(vb, first);
	flushit(vb, last);
}

void flush_screen(struct vidbuffer *vb, int a, int b)
{
}

void flush_screen()
{
	if (savestate_state == STATE_DOSAVE)
	{
		if (delay_savestate_frame > 0)
			--delay_savestate_frame;
		else
		{
			CreateScreenshot();
			save_thumb(screenshot_filename);
			savestate_state = 0;
		}
	}

	// Update the texture from the surface
	SDL_UpdateTexture(texture, nullptr, screen->pixels, screen->pitch);
	// Copy the texture on the renderer
	SDL_RenderCopy(renderer, texture, nullptr, nullptr);
	// Update the window surface (show the renderer)
	SDL_RenderPresent(renderer);

	init_row_map();
}

static void graphics_subinit()
{
	if (screen == nullptr)
	{
		fprintf(stderr, "Unable to set video mode: %s\n", SDL_GetError());
	}
	else
	{
		SDL_ShowCursor(SDL_DISABLE);
		InitAmigaVidMode(&currprefs);
	}
}

STATIC_INLINE int bitsInMask(unsigned long mask)
{
	/* count bits in mask */
	int n = 0;
	while (mask)
	{
		n += mask & 1;
		mask >>= 1;
	}
	return n;
}

STATIC_INLINE int maskShift(unsigned long mask)
{
	/* determine how far mask is shifted */
	int n = 0;
	while (!(mask & 1))
	{
		n++;
		mask >>= 1;
	}
	return n;
}

/* Color management */

static xcolnr xcol8[4096];

static int red_bits, green_bits, blue_bits, alpha_bits;
static int red_shift, green_shift, blue_shift, alpha_shift;
static int alpha;

void init_colors()
{
	int i;

	/* Truecolor: */
	red_bits = bitsInMask(screen->format->Rmask);
	green_bits = bitsInMask(screen->format->Gmask);
	blue_bits = bitsInMask(screen->format->Bmask);
	red_shift = maskShift(screen->format->Rmask);
	green_shift = maskShift(screen->format->Gmask);
	blue_shift = maskShift(screen->format->Bmask);

	alloc_colors64k(red_bits, green_bits, blue_bits, red_shift, green_shift, blue_shift, 0);
	notice_new_xcolors();
	/*for (i = 0; i < 4096; i++)
		xcolors[i] = xcolors[i] * 0x00010001;*/
}

/*
 * Find the colour depth of the display
 */
static int get_display_depth()
{
	//    const SDL_VideoInfo *vid_info;

	//    int depth = 0;
	int depth = 16;

	//    if ((vid_info = SDL_GetVideoInfo()))
	//    {
	//        depth = vid_info->vfmt->BitsPerPixel;

	/* Don't trust the answer if it's 16 bits; the display
	 * could actually be 15 bits deep. We'll count the bits
	 * ourselves */
	//        if (depth == 16)
	//            depth = bitsInMask (vid_info->vfmt->Rmask) + bitsInMask (vid_info->vfmt->Gmask) + bitsInMask (vid_info->vfmt->Bmask);
	//    }

	return depth;
}

int GetSurfacePixelFormat()
{
	int depth = get_display_depth();
	int unit = depth + 1 & 0xF8;

	return (unit == 8 ? RGBFB_CHUNKY
		        : depth == 15 && unit == 16 ? RGBFB_R5G5B5
		        : depth == 16 && unit == 16 ? RGBFB_R5G6B5
		        : unit == 24 ? RGBFB_B8G8R8
		        : unit == 32 ? RGBFB_R8G8B8A8
		        : RGBFB_NONE);
}

static bool graphics_init()
{
	graphics_subinit();

	init_colors();

	return true;
}

void graphics_leave()
{
	graphics_subshutdown();

	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(sdlWindow);

	SDL_VideoQuit();
}

#define  systemRedShift      (screen->format->Rshift)
#define  systemGreenShift    (screen->format->Gshift)
#define  systemBlueShift     (screen->format->Bshift)
#define  systemRedMask       (screen->format->Rmask)
#define  systemGreenMask     (screen->format->Gmask)
#define  systemBlueMask      (screen->format->Bmask)

static int save_png(SDL_Surface* surface, char* path)
{
	int w = surface->w;
	int h = surface->h;
	unsigned char* pix = static_cast<unsigned char *>(surface->pixels);
	unsigned char writeBuffer[1024 * 3];
	FILE* f = fopen(path, "wb");
	if (!f) return 0;
	png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING,
	                                                                   nullptr,
	                                                                   nullptr,
	                                                                   nullptr);
	if (!png_ptr)
	{
		fclose(f);
		return 0;
	}

	png_infop info_ptr = png_create_info_struct(png_ptr);

	if (!info_ptr)
	{
		png_destroy_write_struct(&png_ptr,NULL);
		fclose(f);
		return 0;
	}

	png_init_io(png_ptr, f);

	png_set_IHDR(png_ptr,
	             info_ptr,
	             w,
	             h,
	             8,
	             PNG_COLOR_TYPE_RGB,
	             PNG_INTERLACE_NONE,
	             PNG_COMPRESSION_TYPE_DEFAULT,
	             PNG_FILTER_TYPE_DEFAULT);

	png_write_info(png_ptr, info_ptr);

	unsigned char* b = writeBuffer;

	int sizeX = w;
	int sizeY = h;
	int y;
	int x;

	unsigned short* p = reinterpret_cast<unsigned short *>(pix);
	for (y = 0; y < sizeY; y++)
	{
		for (x = 0; x < sizeX; x++)
		{
			unsigned short v = p[x];

			*b++ = ((v & systemRedMask) >> systemRedShift) << 3; // R
			*b++ = ((v & systemGreenMask) >> systemGreenShift) << 2; // G
			*b++ = ((v & systemBlueMask) >> systemBlueShift) << 3; // B
		}
		p += surface->pitch / 2;
		png_write_row(png_ptr, writeBuffer);
		b = writeBuffer;
	}

	png_write_end(png_ptr, info_ptr);

	png_destroy_write_struct(&png_ptr, &info_ptr);

	fclose(f);
	return 1;
}

static void CreateScreenshot()
{
	int w, h;

	if (current_screenshot != nullptr)
	{
		SDL_FreeSurface(current_screenshot);
		current_screenshot = nullptr;
	}

	w = screen->w;
	h = screen->h;
	current_screenshot = SDL_CreateRGBSurfaceFrom(screen->pixels,
	                                              w,
	                                              h,
	                                              screen->format->BitsPerPixel,
	                                              screen->pitch,
	                                              screen->format->Rmask,
	                                              screen->format->Gmask,
	                                              screen->format->Bmask,
	                                              screen->format->Amask);
}

static int save_thumb(char* path)
{
	int ret = 0;
	if (current_screenshot != nullptr)
	{
		ret = save_png(current_screenshot, path);
		SDL_FreeSurface(current_screenshot);
		current_screenshot = nullptr;
	}
	return ret;
}

bool vsync_switchmode(int hz)
{
	//	int changed_height = changed_prefs.gfx_size.height;
	//	
	//	if (hz >= 55)
	//		hz = 60;
	//	else
	//		hz = 50;
	//
	//  if(hz == 50 && currVSyncRate == 60)
	//  {
	//    // Switch from NTSC -> PAL
	//    switch(changed_height) {
	//      case 200: changed_height = 240; break;
	//      case 216: changed_height = 262; break;
	//      case 240: changed_height = 270; break;
	//      case 256: changed_height = 270; break;
	//      case 262: changed_height = 270; break;
	//      case 270: changed_height = 270; break;
	//    }
	//  }
	//  else if(hz == 60 && currVSyncRate == 50)
	//  {
	//    // Switch from PAL -> NTSC
	//    switch(changed_height) {
	//      case 200: changed_height = 200; break;
	//      case 216: changed_height = 200; break;
	//      case 240: changed_height = 200; break;
	//      case 256: changed_height = 216; break;
	//      case 262: changed_height = 216; break;
	//      case 270: changed_height = 240; break;
	//    }
	//  }
	//
	//  if(changed_height == currprefs.gfx_size.height && hz == currprefs.chipset_refreshrate)
	//    return true;
	//  
	//  changed_prefs.gfx_size.height = changed_height;

	return true;
}

bool target_graphics_buffer_update()
{
	bool rate_changed = false;

	if (currprefs.gfx_size.height != changed_prefs.gfx_size.height)
	{
		update_display(&changed_prefs);
		rate_changed = true;
	}

	if (rate_changed)
	{
		fpscounter_reset();
		time_per_frame = 1000 * 1000 / (currprefs.chipset_refreshrate);
	}

	return true;
}

#ifdef PICASSO96

int picasso_palette(struct MyCLUTEntry *CLUT)
{
	int i, changed;

	changed = 0;
	for (i = 0; i < 256; i++) {
		int r = CLUT[i].Red;
		int g = CLUT[i].Green;
		int b = CLUT[i].Blue;
		uae_u32 v = (doMask256(r, red_bits, red_shift)
			| doMask256(g, green_bits, green_shift)
			| doMask256(b, blue_bits, blue_shift))
			| doMask256(0xff, alpha_bits, alpha_shift);
		if (v != picasso_vidinfo.clut[i]) {
			//write_log (_T("%d:%08x\n"), i, v);
			picasso_vidinfo.clut[i] = v;
			changed = 1;
		}
	}
	return changed;
}

static int _cdecl resolution_compare(const void *a, const void *b)
{
	struct PicassoResolution *ma = (struct PicassoResolution *)a;
	struct PicassoResolution *mb = (struct PicassoResolution *)b;
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

static void sortmodes(struct MultiDisplay *md)
{
	int	i, idx = -1;
	int pw = -1, ph = -1;

	i = 0;
	while (md->DisplayModes[i].depth >= 0)
		i++;
	qsort(md->DisplayModes, i, sizeof(struct PicassoResolution), resolution_compare);
	for (i = 0; md->DisplayModes[i].depth >= 0; i++) {
		int j, k;
		for (j = 0; md->DisplayModes[i].refresh[j]; j++) {
			for (k = j + 1; md->DisplayModes[i].refresh[k]; k++) {
				if (md->DisplayModes[i].refresh[j] > md->DisplayModes[i].refresh[k]) {
					int t = md->DisplayModes[i].refresh[j];
					md->DisplayModes[i].refresh[j] = md->DisplayModes[i].refresh[k];
					md->DisplayModes[i].refresh[k] = t;
					t = md->DisplayModes[i].refreshtype[j];
					md->DisplayModes[i].refreshtype[j] = md->DisplayModes[i].refreshtype[k];
					md->DisplayModes[i].refreshtype[k] = t;
				}
			}
		}
		if (md->DisplayModes[i].res.height != ph || md->DisplayModes[i].res.width != pw) {
			ph = md->DisplayModes[i].res.height;
			pw = md->DisplayModes[i].res.width;
			idx++;
		}
		md->DisplayModes[i].residx = idx;
	}
}

static void modesList(struct MultiDisplay *md)
{
	int i, j;

	i = 0;
	while (md->DisplayModes[i].depth >= 0) {
		write_log(_T("%d: %s%s ("), i, md->DisplayModes[i].rawmode ? _T("!") : _T(""), md->DisplayModes[i].name);
		j = 0;
		while (md->DisplayModes[i].refresh[j] > 0) {
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

void picasso_InitResolutions()
{
	struct MultiDisplay* md1;
	int i, count = 0;
	char tmp[200];
	int bit_idx;
	int bits[] = {8, 16, 32};

	Displays[0].primary = 1;
	Displays[0].disabled = 0;
	Displays[0].rect.left = 0;
	Displays[0].rect.top = 0;
	Displays[0].rect.right = 800;
	Displays[0].rect.bottom = 480;
	sprintf(tmp, "%s (%d*%d)", "Display", Displays[0].rect.right, Displays[0].rect.bottom);
	Displays[0].name = my_strdup(tmp);
	Displays[0].name2 = my_strdup("Display");

	md1 = Displays;
	DisplayModes = md1->DisplayModes = xmalloc (struct PicassoResolution, MAX_PICASSO_MODES);
	for (i = 0; i < MAX_SCREEN_MODES && count < MAX_PICASSO_MODES; i++)
	{
		for (bit_idx = 0; bit_idx < 3; ++bit_idx)
		{
			int bitdepth = bits[bit_idx];
			int bit_unit = (bitdepth + 1) & 0xF8;
			int rgbFormat = (bitdepth == 8 ? RGBFB_CLUT : (bitdepth == 16 ? RGBFB_R5G6B5 : RGBFB_R8G8B8A8));
			int pixelFormat = 1 << rgbFormat;
			pixelFormat |= RGBFF_CHUNKY;
			//
			//            if (SDL_VideoModeOK (x_size_table[i], y_size_table[i], 16, SDL_SWSURFACE))
			//            {
			DisplayModes[count].res.width = x_size_table[i];
			DisplayModes[count].res.height = y_size_table[i];
			DisplayModes[count].depth = bit_unit >> 3;
			DisplayModes[count].refresh[0] = 50;
			DisplayModes[count].refresh[1] = 60;
			DisplayModes[count].refresh[2] = 0;
			DisplayModes[count].colormodes = pixelFormat;
			sprintf(DisplayModes[count].name, "%dx%d, %d-bit",
			        DisplayModes[count].res.width, DisplayModes[count].res.height, DisplayModes[count].depth * 8);

			count++;
			//            }
		}
	}
	DisplayModes[count].depth = -1;
	sortmodes();
	modesList();
	DisplayModes = Displays[0].DisplayModes;
}
#endif

#ifdef PICASSO96
void gfx_set_picasso_state(int on)
{
	if (on == screen_is_picasso)
		return;

	screen_is_picasso = on;
	open_screen(&currprefs);
	picasso_vidinfo.rowbytes = screen->pitch;
}

void gfx_set_picasso_modeinfo(uae_u32 w, uae_u32 h, uae_u32 depth, RGBFTYPE rgbfmt)
{
	depth >>= 3;
	if ((unsigned(picasso_vidinfo.width) == w) &&
		(unsigned(picasso_vidinfo.height) == h) &&
		(unsigned(picasso_vidinfo.depth) == depth) &&
		(picasso_vidinfo.selected_rgbformat == rgbfmt))
		return;

	picasso_vidinfo.selected_rgbformat = rgbfmt;
	picasso_vidinfo.width = w;
	picasso_vidinfo.height = h;
	picasso_vidinfo.depth = 2; // Native depth
	picasso_vidinfo.extra_mem = 1;

	picasso_vidinfo.pixbytes = 2; // Native bytes
	if (screen_is_picasso)
	{
		open_screen(&currprefs);
		picasso_vidinfo.rowbytes = screen->pitch;
		picasso_vidinfo.rgbformat = RGBFB_R5G6B5;
	}
}

uae_u8* gfx_lock_picasso()
{
	SDL_LockSurface(screen);
	picasso_vidinfo.rowbytes = screen->pitch;
	return static_cast<uae_u8 *>(screen->pixels);
}

void gfx_unlock_picasso()
{
	SDL_UnlockSurface(screen);
}

#endif // PICASSO96
