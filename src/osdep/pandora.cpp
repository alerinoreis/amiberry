/*
 * UAE - The Un*x Amiga Emulator
 *
 * Pandora interface
 *
 */

#include <algorithm>
#include <vector>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <asm/sigcontext.h>
#include <signal.h>
#include "sysconfig.h"
#include "sysdeps.h"
#include "config.h"
#include "autoconf.h"
#include "uae.h"
#include "options.h"
#include "threaddep/thread.h"
#include "gui.h"
#include "include/memory.h"
#include "newcpu.h"
#include "custom.h"
#include "xwin.h"
#include "drawing.h"
#include "inputdevice.h"
#include "keybuf.h"
#include "keyboard.h"
#include "disk.h"
#include "savestate.h"
#include "traps.h"
#include "bsdsocket.h"
#include "blkdev.h"
#include "native2amiga.h"
#include "rtgmodes.h"
#include "uaeresource.h"
#include "rommgr.h"
#include "akiko.h"
#include "SDL.h"
#include "pandora_rp9.h"
#include "gfxboard.h"

extern void signal_segv(int signum, siginfo_t* info, void* ptr);
extern void gui_force_rtarea_hdchange();

static int delayed_mousebutton = 0;
static int doStylusRightClick;

extern int loadconfig_old(struct uae_prefs* p, const char* orgpath);
extern void SetLastActiveConfig(const char* filename);

/* Keyboard */
//int customControlMap[SDLK_LAST];

TCHAR start_path_data[MAX_DPATH];
TCHAR currentDir[MAX_DPATH];

#ifdef CAPSLOCK_DEBIAN_WORKAROUND
#include <linux/kd.h>
#include <sys/ioctl.h>
unsigned char kbd_led_status;
char kbd_flags;
#endif

static char config_path[MAX_DPATH];
static char rom_path[MAX_DPATH];
static char rp9_path[MAX_DPATH];
char last_loaded_config[MAX_DPATH] = {'\0'};

int max_uae_width;
int max_uae_height;

extern "C" int main(int argc, char* argv[]);

void reinit_amiga()
{
	write_log("reinit_amiga() called\n");
	DISK_free();
#ifdef CD32
	akiko_free();
#endif
#ifdef FILESYS
	filesys_cleanup();
	hardfile_reset();
#endif
#ifdef AUTOCONFIG
#if defined (BSDSOCKET)
	bsdlib_reset();
#endif
	expansion_cleanup();
#endif
	device_func_reset();
	memory_cleanup();

	currprefs = changed_prefs;
	/* force sound settings change */
	currprefs.produce_sound = 0;

	framecnt = 1;
#ifdef AUTOCONFIG
	rtarea_setup();
#endif
#ifdef FILESYS
	rtarea_init();
	uaeres_install();
	hardfile_install();
#endif
	keybuf_init();

#ifdef AUTOCONFIG
	expansion_init();
#endif
#ifdef FILESYS
	filesys_install();
#endif
	memory_init();
	memory_reset();

#ifdef AUTOCONFIG
#if defined (BSDSOCKET)
	bsdlib_install();
#endif
	emulib_install();
	native2amiga_install();
#endif

	custom_init(); /* Must come after memory_init */
	DISK_init();

	reset_frame_rate_hack();
	init_m68k();
}

static int sleep_millis2(int ms, bool main)
{
	int ret = 0;
	//TODO: This is very simplified for now
	usleep(ms * 1000);
	return ret;
}
int sleep_millis_main(int ms)
{
	return sleep_millis2(ms, true);
	
}

int sleep_millis(int ms)
{
	return sleep_millis2(ms, false);
}

void logging_init()
{
#ifdef WITH_LOGGING
    static int started;
    static int first;
    char debugfilename[MAX_DPATH];

    if (first > 1)
    {
        write_log ("***** RESTART *****\n");
        return;
    }
    if (first == 1)
    {
        if (debugfile)
            fclose (debugfile);
        debugfile = 0;
    }

    sprintf(debugfilename, "%s/amiberry_log.txt", start_path_data);
    if(!debugfile)
        debugfile = fopen(debugfilename, "wt");

    first++;
    write_log ( "Amiberry Logfile\n\n");
#endif
}

void logging_cleanup()
{
#ifdef WITH_LOGGING
    if(debugfile)
        fclose(debugfile);
    debugfile = 0;
#endif
}


void stripslashes(TCHAR* p)
{
	while (_tcslen (p) > 0 && (p[_tcslen (p) - 1] == '\\' || p[_tcslen (p) - 1] == '/'))
		p[_tcslen (p) - 1] = 0;
}

void fixtrailing(TCHAR* p)
{
	if (_tcslen(p) == 0)
		return;
	if (p[_tcslen(p) - 1] == '/' || p[_tcslen(p) - 1] == '\\')
		return;
	_tcscat(p, "/");
}

void getpathpart(TCHAR* outpath, int size, const TCHAR* inpath)
{
	_tcscpy (outpath, inpath);
	TCHAR* p = _tcsrchr (outpath, '/');
	if (p)
		p[0] = 0;
	fixtrailing(outpath);
}

void getfilepart(TCHAR* out, int size, const TCHAR* path)
{
	out[0] = 0;
	const TCHAR* p = _tcsrchr (path, '/');
	if (p)
	_tcscpy (out, p + 1);
	else
	_tcscpy (out, path);
}

uae_u8* target_load_keyfile(struct uae_prefs* p, const char* path, int* sizep, char* name)
{
	return nullptr;
}

void target_run()
{
}

void target_quit()
{
}

void target_fixup_options(struct uae_prefs* p)
{
	if (p->rtgboards[0].rtgmem_type >= GFXBOARD_HARDWARE) {
		p->rtg_hardwareinterrupt = false;
		p->rtg_hardwaresprite = false;
//		p->win32_rtgmatchdepth = false;
		if (gfxboard_need_byteswap(&p->rtgboards[0]))
			p->color_mode = 5;
//		if (p->ppc_model && !p->gfx_api) {
//			error_log(_T("Graphics board and PPC: Direct3D enabled."));
//			p->gfx_api = 1;
//		}
	}
}

void target_default_options(struct uae_prefs* p, int type)
{
	p->pandora_horizontal_offset = 0;
	p->pandora_vertical_offset = 0;
	p->pandora_hide_idle_led = 0;

	p->pandora_tapDelay = 10;
	p->pandora_customControls = 0;

//	p->picasso96_modeflags = RGBFF_CLUT | RGBFF_R5G6B5 | RGBFF_R8G8B8A8;

	//    memset(customControlMap, 0, sizeof(customControlMap));
}

void target_save_options(struct zfile* f, struct uae_prefs* p)
{
	cfgfile_write(f, "pandora.cpu_speed", "%d", p->pandora_cpu_speed);
	cfgfile_write(f, "pandora.hide_idle_led", "%d", p->pandora_hide_idle_led);
	cfgfile_write(f, "pandora.tap_delay", "%d", p->pandora_tapDelay);
	cfgfile_write(f, "pandora.custom_controls", "%d", p->pandora_customControls);
	//	cfgfile_write(f, "pandora.custom_up", "%d", customControlMap[VK_UP]);
	//	cfgfile_write(f, "pandora.custom_down", "%d", customControlMap[VK_DOWN]);
	//	cfgfile_write(f, "pandora.custom_left", "%d", customControlMap[VK_LEFT]);
	//	cfgfile_write(f, "pandora.custom_right", "%d", customControlMap[VK_RIGHT]);
	//	cfgfile_write(f, "pandora.custom_a", "%d", customControlMap[VK_A]);
	//	cfgfile_write(f, "pandora.custom_b", "%d", customControlMap[VK_B]);
	//	cfgfile_write(f, "pandora.custom_x", "%d", customControlMap[VK_X]);
	//	cfgfile_write(f, "pandora.custom_y", "%d", customControlMap[VK_Y]);
	//	cfgfile_write(f, "pandora.custom_l", "%d", customControlMap[VK_L]);
	//	cfgfile_write(f, "pandora.custom_r", "%d", customControlMap[VK_R]);
	cfgfile_write(f, "pandora.move_x", "%d", p->pandora_horizontal_offset);
	cfgfile_write(f, "pandora.move_y", "%d", p->pandora_vertical_offset);
}

void target_restart()
{
}

TCHAR* target_expand_environment(const TCHAR* path)
{
	return strdup(path);
}

int target_parse_option(struct uae_prefs* p, const char* option, const char* value)
{
	int result = (cfgfile_intval(option, value, "cpu_speed", &p->pandora_cpu_speed, 1)
		|| cfgfile_intval(option, value, "hide_idle_led", &p->pandora_hide_idle_led, 1)
		|| cfgfile_intval(option, value, "tap_delay", &p->pandora_tapDelay, 1)
		|| cfgfile_intval(option, value, "custom_controls", &p->pandora_customControls, 1)
		//	              || cfgfile_intval(option, value, "custom_up", &customControlMap[VK_UP], 1)
		//	              || cfgfile_intval(option, value, "custom_down", &customControlMap[VK_DOWN], 1)
		//	              || cfgfile_intval(option, value, "custom_left", &customControlMap[VK_LEFT], 1)
		//	              || cfgfile_intval(option, value, "custom_right", &customControlMap[VK_RIGHT], 1)
		//	              || cfgfile_intval(option, value, "custom_a", &customControlMap[VK_A], 1)
		//	              || cfgfile_intval(option, value, "custom_b", &customControlMap[VK_B], 1)
		//	              || cfgfile_intval(option, value, "custom_x", &customControlMap[VK_X], 1)
		//	              || cfgfile_intval(option, value, "custom_y", &customControlMap[VK_Y], 1)
		//	              || cfgfile_intval(option, value, "custom_l", &customControlMap[VK_L], 1)
		//	              || cfgfile_intval(option, value, "custom_r", &customControlMap[VK_R], 1)
		|| cfgfile_intval(option, value, "move_x", &p->pandora_horizontal_offset, 1)
		|| cfgfile_intval(option, value, "move_y", &p->pandora_vertical_offset, 1)
	);
	return result;
}

void fetch_datapath(char* out, int size)
{
	strncpy(out, start_path_data, size);
	strncat(out, "/", size);
}

void fetch_saveimagepath(char* out, int size, int dir)
{
	strncpy(out, start_path_data, size);
	strncat(out, "/savestates/", size);
}

void fetch_configurationpath(char* out, int size)
{
	strncpy(out, config_path, size);
}

void set_configurationpath(char* newpath)
{
	strncpy(config_path, newpath, MAX_DPATH);
}

void fetch_rompath(char* out, int size)
{
	strncpy(out, rom_path, size);
}

void set_rompath(char* newpath)
{
	strncpy(rom_path, newpath, MAX_DPATH);
}

void fetch_rp9path(char* out, int size)
{
	strncpy(out, rp9_path, size);
}

void fetch_savestatepath(char* out, int size)
{
	strncpy(out, start_path_data, size);
	strncat(out, "/savestates/", size);
}

void fetch_screenshotpath(char* out, int size)
{
	strncpy(out, start_path_data, size);
	strncat(out, "/screenshots/", size);
}

int target_cfgfile_load(struct uae_prefs* p, const char* filename, int type, int isdefault)
{
	int i;
	int result = 0;

	if (emulating && changed_prefs.cdslots[0].inuse)
		gui_force_rtarea_hdchange();

	discard_prefs(p, type);
	default_prefs(p, true, type);

	const char* ptr = strstr(filename, ".rp9");
	if (ptr > nullptr)
	{
		// Load rp9 config
		result = rp9_parse_file(p, filename);
		if (result)
			extractFileName(filename, last_loaded_config);
	}
	else
	{
		ptr = strstr(filename, ".uae");
		if (ptr > nullptr)
		{
			int type = CONFIG_TYPE_HARDWARE | CONFIG_TYPE_HOST;
			result = cfgfile_load(p, filename, &type, 0, 1);
		}
		if (result)
			extractFileName(filename, last_loaded_config);
		else
			result = loadconfig_old(p, filename);
	}

	if (result)
	{
		for (i = 0; i < p->nr_floppies; ++i)
		{
			if (!DISK_validate_filename(p, p->floppyslots[i].df, 0, nullptr, nullptr, nullptr))
				p->floppyslots[i].df[0] = 0;
			disk_insert(i, p->floppyslots[i].df);
			if (strlen(p->floppyslots[i].df) > 0)
				AddFileToDiskList(p->floppyslots[i].df, 1);
		}

		if (!isdefault)
			inputdevice_updateconfig(nullptr, p);

		SetLastActiveConfig(filename);

		if (count_HDs(p) > 0) // When loading a config with HDs, always do a hardreset
			gui_force_rtarea_hdchange();
	}

	return result;
}

int check_configfile(char* file)
{
	char tmp[MAX_PATH];

	FILE* f = fopen(file, "rt");
	if (f)
	{
		fclose(f);
		return 1;
	}

	strcpy(tmp, file);
	char* ptr = strstr(tmp, ".uae");
	if (ptr > nullptr)
	{
		*(ptr + 1) = '\0';
		strcat(tmp, "conf");
		f = fopen(tmp, "rt");
		if (f)
		{
			fclose(f);
			return 2;
		}
	}

	return 0;
}

void extractFileName(const char* str, char* buffer)
{
	const char* p = str + strlen(str) - 1;
	while (*p != '/' && p > str)
		p--;
	p++;
	strcpy(buffer, p);
}

void extractPath(char* str, char* buffer)
{
	strcpy(buffer, str);
	char* p = buffer + strlen(buffer) - 1;
	while (*p != '/' && p > buffer)
		p--;
	p[1] = '\0';
}

void removeFileExtension(char* filename)
{
	char* p = filename + strlen(filename) - 1;
	while (p > filename && *p != '.')
	{
		*p = '\0';
		--p;
	}
	*p = '\0';
}

void ReadDirectory(const char* path, vector<string>* dirs, vector<string>* files)
{
	DIR* dir;
	struct dirent* dent;

	if (dirs != nullptr)
		dirs->clear();
	if (files != nullptr)
		files->clear();

	dir = opendir(path);
	if (dir != nullptr)
	{
		while ((dent = readdir(dir)) != nullptr)
		{
			if (dent->d_type == DT_DIR)
			{
				if (dirs != nullptr)
					dirs->push_back(dent->d_name);
			}
			else if (files != nullptr)
				files->push_back(dent->d_name);
		}
		if (dirs != nullptr && dirs->size() > 0 && (*dirs)[0] == ".")
			dirs->erase(dirs->begin());
		closedir(dir);
	}

	if (dirs != nullptr)
		std::sort(dirs->begin(), dirs->end());
	if (files != nullptr)
		std::sort(files->begin(), files->end());
}

void saveAdfDir()
{
	char path[MAX_DPATH];
	int i;

	snprintf(path, MAX_DPATH, "%s/conf/adfdir.conf", start_path_data);
	FILE* f = fopen(path, "w");
	if (!f)
		return;

	char buffer[MAX_DPATH];
	snprintf(buffer, MAX_DPATH, "path=%s\n", currentDir);
	fputs(buffer, f);

	snprintf(buffer, MAX_DPATH, "config_path=%s\n", config_path);
	fputs(buffer, f);

	snprintf(buffer, MAX_DPATH, "rom_path=%s\n", rom_path);
	fputs(buffer, f);

	snprintf(buffer, MAX_DPATH, "ROMs=%d\n", lstAvailableROMs.size());
	fputs(buffer, f);
	for (i = 0; i < lstAvailableROMs.size(); ++i)
	{
		snprintf(buffer, MAX_DPATH, "ROMName=%s\n", lstAvailableROMs[i]->Name);
		fputs(buffer, f);
		snprintf(buffer, MAX_DPATH, "ROMPath=%s\n", lstAvailableROMs[i]->Path);
		fputs(buffer, f);
		snprintf(buffer, MAX_DPATH, "ROMType=%d\n", lstAvailableROMs[i]->ROMType);
		fputs(buffer, f);
	}

	snprintf(buffer, MAX_DPATH, "MRUDiskList=%d\n", lstMRUDiskList.size());
	fputs(buffer, f);
	for (i = 0; i < lstMRUDiskList.size(); ++i)
	{
		snprintf(buffer, MAX_DPATH, "Diskfile=%s\n", lstMRUDiskList[i].c_str());
		fputs(buffer, f);
	}

	snprintf(buffer, MAX_DPATH, "MRUCDList=%d\n", lstMRUCDList.size());
	fputs(buffer, f);
	for (i = 0; i < lstMRUCDList.size(); ++i)
	{
		snprintf(buffer, MAX_DPATH, "CDfile=%s\n", lstMRUCDList[i].c_str());
		fputs(buffer, f);
	}

	fclose(f);
}

void get_string(FILE* f, char* dst, int size)
{
	char buffer[MAX_PATH];
	fgets(buffer, MAX_PATH, f);
	int i = strlen(buffer);
	while (i > 0 && (buffer[i - 1] == '\t' || buffer[i - 1] == ' '
		|| buffer[i - 1] == '\r' || buffer[i - 1] == '\n'))
		buffer[--i] = '\0';
	strncpy(dst, buffer, size);
}

void loadAdfDir()
{
	char path[MAX_DPATH];
	int i;

	strcpy(currentDir, start_path_data);
	snprintf(config_path, MAX_DPATH, "%s/conf/", start_path_data);
	snprintf(rom_path, MAX_DPATH, "%s/kickstarts/", start_path_data);
	snprintf(rp9_path, MAX_DPATH, "%s/rp9/", start_path_data);

	snprintf(path, MAX_DPATH, "%s/conf/adfdir.conf", start_path_data);
	FILE* f1 = fopen(path, "rt");
	if (f1)
	{
		fscanf(f1, "path=");
		get_string(f1, currentDir, sizeof(currentDir));
		if (!feof(f1))
		{
			fscanf(f1, "config_path=");
			get_string(f1, config_path, sizeof(config_path));
			fscanf(f1, "rom_path=");
			get_string(f1, rom_path, sizeof(rom_path));

			int numROMs;
			fscanf(f1, "ROMs=%d\n", &numROMs);
			for (i = 0; i < numROMs; ++i)
			{
				AvailableROM* tmp;
				tmp = new AvailableROM();
				fscanf(f1, "ROMName=");
				get_string(f1, tmp->Name, sizeof(tmp->Name));
				fscanf(f1, "ROMPath=");
				get_string(f1, tmp->Path, sizeof(tmp->Path));
				fscanf(f1, "ROMType=%d\n", &(tmp->ROMType));
				lstAvailableROMs.push_back(tmp);
			}

			lstMRUDiskList.clear();
			int numDisks = 0;
			char disk[MAX_PATH];
			fscanf(f1, "MRUDiskList=%d\n", &numDisks);
			for (i = 0; i < numDisks; ++i)
			{
				fscanf(f1, "Diskfile=");
				get_string(f1, disk, sizeof(disk));
				FILE * f = fopen(disk, "rb");
				if (f != NULL)
				{
					fclose(f);
					lstMRUDiskList.push_back(disk);
				}
			}

			lstMRUCDList.clear();
			int numCD = 0;
			char cd[MAX_PATH];
			fscanf(f1, "MRUCDList=%d\n", &numCD);
			for (i = 0; i < numCD; ++i)
			{
				fscanf(f1, "CDfile=");
				get_string(f1, cd, sizeof(cd));
				FILE * f = fopen(cd, "rb");
				if (f != NULL)
				{
					fclose(f);
					lstMRUCDList.push_back(cd);
				}
			}
		}
		fclose(f1);
	}
}

void target_reset()
{
}

uae_u32 emulib_target_getcpurate(uae_u32 v, uae_u32* low)
{
	*low = 0;
	if (v == 1)
	{
		*low = 1e+9; /* We have nano seconds */
		return 0;
	}
	
	if (v == 2)
	{
		int64_t time;
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		time = (int64_t(ts.tv_sec) * 1000000000) + ts.tv_nsec;
		*low = uae_u32(time & 0xffffffff);
		return uae_u32(time >> 32);
	}
	return 0;
}

int main(int argc, char* argv[])
{
	struct sigaction action;
	max_uae_width = 1920;
	max_uae_height = 1080;

	// Get startup path
	getcwd(start_path_data, MAX_DPATH);
	loadAdfDir();
	rp9_init();

	snprintf(savestate_fname, MAX_PATH, "%s/saves/default.ads", start_path_data);
	logging_init();

	memset(&action, 0, sizeof(action));
	action.sa_sigaction = signal_segv;
	action.sa_flags = SA_SIGINFO;
	if (sigaction(SIGSEGV, &action, nullptr) < 0)
	{
		printf("Failed to set signal handler (SIGSEGV).\n");
		abort();
	}
	if (sigaction(SIGILL, &action, nullptr) < 0)
	{
		printf("Failed to set signal handler (SIGILL).\n");
		abort();
	}

	alloc_AmigaMem();
	RescanROMs();

#ifdef CAPSLOCK_DEBIAN_WORKAROUND
	// set capslock state based upon current "real" state
	ioctl(0, KDGKBLED, &kbd_flags);
	ioctl(0, KDGETLED, &kbd_led_status);
	if ((kbd_flags & 07) & LED_CAP)
	{
		// record capslock pressed
		kbd_led_status |= LED_CAP;
		inputdevice_do_keyboard(AK_CAPSLOCK, 1);
	}
	else
	{
		// record capslock as not pressed
		kbd_led_status &= ~LED_CAP;
		inputdevice_do_keyboard(AK_CAPSLOCK, 0);
	}
	ioctl(0, KDSETLED, kbd_led_status);
#endif

	real_main(argc, argv);

#ifdef CAPSLOCK_DEBIAN_WORKAROUND 
	// restore keyboard LEDs to normal state
	ioctl(0, KDSETLED, 0xFF);
#endif

	ClearAvailableROMList();
	romlist_clear();
	free_keyring();
	free_AmigaMem();
	lstMRUDiskList.clear();
	lstMRUCDList.clear();
	rp9_cleanup();

	logging_cleanup();

	return 0;
}

int handle_msgpump()
{
	int got = 0;
	SDL_Event rEvent;
	int keycode;
	int modifier;
	int handled = 0;
	int i;

	if (delayed_mousebutton)
	{
		--delayed_mousebutton;
		if (delayed_mousebutton == 0)
			setmousebuttonstate(0, 0, 1);
	}

	while (SDL_PollEvent(&rEvent))
	{
		got = 1;
		const Uint8* keystate = SDL_GetKeyboardState(nullptr);

		switch (rEvent.type)
		{
		case SDL_QUIT:
			uae_quit();
			break;

		case SDL_JOYBUTTONDOWN:
			if (currprefs.button_for_menu != -1 && rEvent.jbutton.button == currprefs.button_for_menu)
				inputdevice_add_inputcode(AKS_ENTERGUI, 1);
			if (currprefs.button_for_quit != -1 && rEvent.jbutton.button == currprefs.button_for_quit)
				inputdevice_add_inputcode(AKS_QUIT, 1);
			break;

		case SDL_KEYDOWN:
			// Menu button or key pressed
			if (currprefs.key_for_menu != 0 && rEvent.key.keysym.scancode == currprefs.key_for_menu)
			{
				inputdevice_add_inputcode(AKS_ENTERGUI, 1);
				break;
			}
			if (currprefs.key_for_quit != 0 && rEvent.key.keysym.sym == currprefs.key_for_quit)
			{
				inputdevice_add_inputcode(AKS_QUIT, 1);
				break;
			}
			if (keystate[SDL_SCANCODE_LCTRL] && keystate[SDL_SCANCODE_LGUI] && (keystate[SDL_SCANCODE_RGUI] || keystate[SDL_SCANCODE_MENU]))
			{
				uae_reset(0, 1);
				break;
			}

			switch (rEvent.key.keysym.scancode)
			{
#ifdef CAPSLOCK_DEBIAN_WORKAROUND
			case SDL_SCANCODE_CAPSLOCK: // capslock
				// Treat CAPSLOCK as a toggle. If on, set off and vice/versa
				ioctl(0, KDGKBLED, &kbd_flags);
				ioctl(0, KDGETLED, &kbd_led_status);
				if ((kbd_flags & 07) & LED_CAP)
				{
					// On, so turn off
					kbd_led_status &= ~LED_CAP;
					kbd_flags &= ~LED_CAP;
					inputdevice_do_keyboard(AK_CAPSLOCK, 0);
				}
				else
				{
					// Off, so turn on
					kbd_led_status |= LED_CAP;
					kbd_flags |= LED_CAP;
					inputdevice_do_keyboard(AK_CAPSLOCK, 1);
				}
				ioctl(0, KDSETLED, kbd_led_status);
				ioctl(0, KDSKBLED, kbd_flags);
				break;
#endif

			case SDL_SCANCODE_LSHIFT: // Shift key
				inputdevice_do_keyboard(AK_RSH, 1);
				break;
			case SDL_SCANCODE_RSHIFT:
				inputdevice_do_keyboard(AK_LSH, 1);
				break;

			case SDL_SCANCODE_RGUI:
			case SDL_SCANCODE_MENU:
				inputdevice_do_keyboard(AK_RAMI, 1);
				break;
			case SDL_SCANCODE_LGUI:
				inputdevice_do_keyboard(AK_LAMI, 1);
				break;

			case SDL_SCANCODE_LALT:
				inputdevice_do_keyboard(AK_LALT, 1);
				break;
			case SDL_SCANCODE_RALT:
				inputdevice_do_keyboard(AK_RALT, 1);
				break;

			case SDL_SCANCODE_LCTRL:
			case SDL_SCANCODE_RCTRL:
				inputdevice_do_keyboard(AK_CTRL, 1);
				break;

				//            case VK_L: // Left shoulder button
				//            case VK_R:  // Right shoulder button
				//                if(currprefs.input_tablet > TABLET_OFF)
				//                {
				//                    // Holding left or right shoulder button -> stylus does right mousebutton
				//                    doStylusRightClick = 1;
				//                }
				// Fall through...

			default:
				//				if (currprefs.pandora_customControls)
				//				{
				////					keycode = customControlMap[rEvent.key.keysym.sym];
				//					if (keycode < 0)
				//					{
				//					    // Simulate mouse or joystick
				//						SimulateMouseOrJoy(keycode, 1);
				//						break;
				//					}
				//					else if (keycode > 0)
				//					{
				//					    // Send mapped key press
				//						inputdevice_do_keyboard(keycode, 1);
				//						break;
				//					}
				//				}
				//				else
				modifier = rEvent.key.keysym.mod;

				//                keycode = translate_pandora_keys(rEvent.key.keysym.sym, &modifier);
				//                if(keycode)
				//                {
				//                    if(modifier == KMOD_SHIFT)
				//                        inputdevice_do_keyboard(AK_LSH, 1);
				//                    else
				//                        inputdevice_do_keyboard(AK_LSH, 0);
				//					
				//                    inputdevice_do_keyboard(keycode, 1);
				//                }
				//                else
				//                {
				inputdevice_translatekeycode(0, rEvent.key.keysym.sym, 1);
				//                }
				break;
			}
			break;

		case SDL_KEYUP:
			switch (rEvent.key.keysym.scancode)
			{
			case SDL_SCANCODE_LSHIFT: // Shift key
				inputdevice_do_keyboard(AK_RSH, 0);
				break;
			case SDL_SCANCODE_RSHIFT:
				inputdevice_do_keyboard(AK_LSH, 0);
				break;

			case SDL_SCANCODE_RGUI:
			case SDL_SCANCODE_MENU:
				inputdevice_do_keyboard(AK_RAMI, 0);
				break;
			case SDL_SCANCODE_LGUI:
				inputdevice_do_keyboard(AK_LAMI, 0);
				break;

			case SDL_SCANCODE_LALT:
				inputdevice_do_keyboard(AK_LALT, 0);
				break;
			case SDL_SCANCODE_RALT:
				inputdevice_do_keyboard(AK_RALT, 0);
				break;

			case SDL_SCANCODE_LCTRL:
			case SDL_SCANCODE_RCTRL:
				inputdevice_do_keyboard(AK_CTRL, 0);
				break;

				//            case VK_L: // Left shoulder button
				//            case VK_R:  // Right shoulder button
				//                if(currprefs.input_tablet > TABLET_OFF)
				//                {
				//                    // Release left or right shoulder button -> stylus does left mousebutton
				//                    doStylusRightClick = 0;
				//                }
				// Fall through...

			default:
				//				if (currprefs.pandora_customControls)
				//				{
				////					keycode = customControlMap[rEvent.key.keysym.sym];
				//					if (keycode < 0)
				//					{
				//					    // Simulate mouse or joystick
				//						SimulateMouseOrJoy(keycode, 0);
				//						break;
				//					}
				//					else if (keycode > 0)
				//					{
				//					    // Send mapped key release
				//						inputdevice_do_keyboard(keycode, 0);
				//						break;
				//					}
				//				}

				modifier = rEvent.key.keysym.mod;
				//                keycode = translate_pandora_keys(rEvent.key.keysym.sym, &modifier);
				//                if(keycode)
				//                {
				//                    inputdevice_do_keyboard(keycode, 0);
				//                    if(modifier == KMOD_SHIFT)
				//                        inputdevice_do_keyboard(AK_LSH, 0);
				//                }
				//                else
				//                {
				inputdevice_translatekeycode(0, rEvent.key.keysym.sym, 0);
				//                }
				break;
			}
			break;

		case SDL_MOUSEBUTTONDOWN:
			if (currprefs.jports[0].id == JSEM_MICE || currprefs.jports[1].id == JSEM_MICE)
			{
				if (rEvent.button.button == SDL_BUTTON_LEFT)
				{
					if (currprefs.input_tablet > TABLET_OFF && !doStylusRightClick)
					{
						// Delay mousebutton, we need new position first...
						delayed_mousebutton = currprefs.pandora_tapDelay << 1;
					}
					else
					{
						setmousebuttonstate(0, doStylusRightClick, 1);
					}
				}
				else if (rEvent.button.button == SDL_BUTTON_RIGHT)
					setmousebuttonstate(0, 1, 1);
			}
			break;

		case SDL_MOUSEBUTTONUP:
			if (currprefs.jports[0].id == JSEM_MICE || currprefs.jports[1].id == JSEM_MICE)
			{
				if (rEvent.button.button == SDL_BUTTON_LEFT)
				{
					setmousebuttonstate(0, doStylusRightClick, 0);
				}
				else if (rEvent.button.button == SDL_BUTTON_RIGHT)
					setmousebuttonstate(0, 1, 0);
			}
			break;

		case SDL_MOUSEMOTION:
			if (currprefs.input_tablet == TABLET_OFF)
			{
				if (currprefs.jports[0].id == JSEM_MICE || currprefs.jports[1].id == JSEM_MICE)
				{
					int x, y;
					int mouseScale = currprefs.input_joymouse_multiplier / 2;
					x = rEvent.motion.xrel;
					y = rEvent.motion.yrel;

					setmousestate(0, 0, x * mouseScale, 0);
					setmousestate(0, 1, y * mouseScale, 0);
				}
			}
			break;
		}
	}
	return got;
}

static uaecptr clipboard_data;

void amiga_clipboard_die()
{
}

void amiga_clipboard_init()
{
}

void amiga_clipboard_task_start(uaecptr data)
{
	clipboard_data = data;
}

uae_u32 amiga_clipboard_proc_start()
{
	return clipboard_data;
}

void amiga_clipboard_got_data(uaecptr data, uae_u32 size, uae_u32 actual)
{
}

int amiga_clipboard_want_data()
{
	return 0;
}

void clipboard_vsync()
{
}
