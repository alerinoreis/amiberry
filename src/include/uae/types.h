/*
 * Common types used throughout the UAE source code
 * Copyright (C) 2014 Frode Solheim
 *
 * Licensed under the terms of the GNU General Public License version 2.
 * See the file 'COPYING' for full license text.
 */

#ifndef UAE_TYPES_H
#define UAE_TYPES_H

/* This file is intended to be included by external libraries as well,
 * so don't pull in too much UAE-specific stuff. */

#if 0
#include "config.h"
#endif

/* Define uae_ integer types. Prefer long long int for uae_x64 since we can
 * then use the %lld format specifier for both 32-bit and 64-bit instead of
 * the ugly PRIx64 macros. */

#include <stdint.h>

typedef int8_t uae_s8;
typedef uint8_t uae_u8;

typedef int16_t uae_s16;
typedef uint16_t uae_u16;

typedef int32_t uae_s32;
typedef uint32_t uae_u32;

#ifndef uae_s64
typedef long long int uae_s64;
#endif
#ifndef uae_u64
typedef unsigned long long int uae_u64;
#endif

#ifdef HAVE___UINT128_T
#define HAVE_UAE_U128
typedef __uint128_t uae_u128;
#endif

/* Parts of the UAE/WinUAE code uses the bool type (from C++).
 * Including stdbool.h lets this be valid also when compiling with C. */

#ifndef __cplusplus
#include <stdbool.h>
#endif

/* Use uaecptr to represent 32-bit (or 24-bit) addresses into the Amiga
 * address space. This is a 32-bit unsigned int regarless of host arch. */

typedef uae_u32 uaecptr;

/* Define UAE character types. WinUAE uses TCHAR (typedef for wchar_t) for
 * many strings. FS-UAE always uses char-based strings in UTF-8 format.
 * Defining SIZEOF_TCHAR lets us determine whether to include overloaded
 * functions in some cases or not. */

typedef char uae_char;

//#ifdef _WIN32
//#include <tchar.h>
//#ifdef UNICODE
//#define SIZEOF_TCHAR 2
//#else
//#define SIZEOF_TCHAR 1
//#endif
//#else
typedef char TCHAR;
#define SIZEOF_TCHAR 1
//#endif

#ifndef _T
#if SIZEOF_TCHAR == 1
#define _T(x) x
#else
#define _T(x) Lx
#endif
#endif

#ifndef FALSE
#define FALSE 0
#endif
#ifndef TRUE
#define TRUE (!FALSE)
#endif

// Amiberry
#define strcmpi(x,y) strcasecmp(x,y)
#define stricmp(x,y) strcasecmp(x,y)

#define A_ZIP
//#define A_RAR
#define A_7Z
#define A_LHA
#define A_LZX
#define A_DMS
#define A_WRP

#ifndef MAX_PATH
#define MAX_PATH 256
#endif

#define WORDS_BIGENDIAN 1

#define M68K_SPEED_7MHZ_CYCLES 0
#define M68K_SPEED_14MHZ_CYCLES 1024
#define M68K_SPEED_25MHZ_CYCLES 128

typedef unsigned int WPARAM;
typedef long LPARAM;
typedef int SOCKET;
#define INVALID_SOCKET -1

typedef int BOOL;
typedef unsigned char boolean;

typedef unsigned short USHORT;

#define Sleep(x) usleep(x*1000)

/* Some defines to make it easier to compare files with WinUAE */
#define _tzset()            tzset()
#define _tcsftime(w,x,y,z)  strftime(w,x,y,z)
#define _timezone           timezone
#define _daylight           daylight
#define _ftime(x)           ftime(x)
#define _tfopen(x,y)        fopen(x,y)
#define _ftelli64(x)        ftello64(x)
#define _fseeki64(x,y,z)    fseeko64(x,y,z)
#define _stat64             stat64
#define _wunlink(x)         unlink(x)
#define _tcslen(x)          strlen(x)
#define _tcscpy(x,y)        strcpy(x,y)
#define _tcsncpy(x,y,z)     strncpy(x,y,z)
#define _tcscat(x,y)        strcat(x,y)
#define _tcsncat(x,y,z)     strncat(x,y,z)
#define _tcscmp(x,y)        strcmp(x,y)
#define _tcsicmp(x,y)       strcmpi(x,y)
#define _tcsncmp(x,y,z)     strncmp(x,y,z)
#define _tcsnicmp(x,y,z)    strncasecmp(x,y,z)
#define _tcschr(x,y)        strchr(x,y)
#define _tcsrchr(x,y)       strrchr(x,y)
#define _tcsstr(x,y)        strstr(x,y)
#define _tcscspn(x,y)       strcspn(x,y)
#define _totupper(x)        toupper(x)
#define _totlower(x)        tolower(x)
#define _istupper(x)        isupper(x)
#define _istspace(x)        isspace(x)
#define _istdigit(x)        isdigit(x)
#define _tstoi(x)           atoi(x)
#define _tstol(x)           atol(x)
#define _tstoi64(x)         atoll(x)
#define _tstof(x)           atof(x)
#define _tcstol(x,y,z)      strtol(x,y,z)
#define _tcstod(x,y)        strtod(x,y)
#define _stprintf           sprintf
#define _vstprintf(x,y,z)   vsprintf(x,y,z)
#define _vsntprintf(w,x,y,z)  vsnprintf(w,x,y,z)
#define _strtoui64(x,y,z)   strtoll(x,y,z)
#define _istalnum(x)        isalnum(x)
#define _tcsspn(x,y)		    strspn(x,y)

#endif /* UAE_TYPES_H */
