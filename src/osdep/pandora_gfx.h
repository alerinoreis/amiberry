#ifndef PANDORA_GFX_H
#define PANDORA_GFX_H

#define RTG_MODE_SCALE 1
#define RTG_MODE_CENTER 2
#define RTG_MODE_INTEGER_SCALE 3

extern SDL_Window* sdlWindow;
extern SDL_Renderer* renderer;
extern SDL_Texture* texture;
extern SDL_Surface* screen;

extern SDL_Surface* gui_screen;
extern SDL_Texture* gui_texture;

int WIN32GFX_IsPicassoScreen(void);
int WIN32GFX_GetWidth(void);
int WIN32GFX_GetHeight(void);
int WIN32GFX_GetDepth(int real);
void WIN32GFX_DisplayChangeRequested(int);
void DX_Invalidate(int x, int y, int width, int height);
int WIN32GFX_AdjustScreenmode(struct MultiDisplay *md, int *pwidth, int *pheight, int *ppixbits);
extern SDL_Cursor normalcursor;

extern int default_freq;
extern int normal_display_change_starting;
extern int window_led_drives, window_led_drives_end, window_led_joy_start;
extern int window_led_hd, window_led_hd_end;
extern int window_led_joys, window_led_joys_end;
extern int scalepicasso;

extern void check_error_sdl(bool check, const char* message);

extern void close_windows(void);
extern void updatewinfsmode(struct uae_prefs *p);
extern int is3dmode(void);
extern void gfx_lock(void);
extern void gfx_unlock(void);

void DX_Fill(int dstx, int dsty, int width, int height, uae_u32 color);
void centerdstrect(RECT *);
struct MultiDisplay *getdisplay(struct uae_prefs *p);
extern int getrefreshrate(int width, int height);

#endif
