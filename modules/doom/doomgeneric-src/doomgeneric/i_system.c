//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//



#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <stdarg.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef ORIGCODE
#include "SDL.h"
#endif

#include "config.h"

#include "deh_str.h"
#include "doomtype.h"
#include "m_argv.h"
#include "m_config.h"
#include "m_misc.h"
#include "i_joystick.h"
#include "i_sound.h"
#include "i_timer.h"
#include "i_video.h"

#include "i_system.h"

#include "w_wad.h"
#include "z_zone.h"

#ifdef __MACOSX__
#include <CoreFoundation/CFUserNotification.h>
#endif

// Zone allocation: grab as much PSRAM as we can, leaving a reserve for
// non-zone allocations (lumpinfo, hash table, misc) that happen after zone init.
// Empirically only ~54KB of non-zone PSRAM is needed, so 128KB is generous.
#define ZONE_RESERVE_KB   128       /* leave this much free after zone alloc */
#define DEFAULT_RAM_KB    (5*1024)  /* default/max zone size to attempt (5MB) */
#define MIN_RAM_KB        (2*1024)  /* absolute minimum zone size (2MB) */


typedef struct atexit_listentry_s atexit_listentry_t;

struct atexit_listentry_s
{
    atexit_func_t func;
    boolean run_on_error;
    atexit_listentry_t *next;
};

static atexit_listentry_t *exit_funcs = NULL;

void I_AtExit(atexit_func_t func, boolean run_on_error)
{
    atexit_listentry_t *entry;

    entry = malloc(sizeof(*entry));

    entry->func = func;
    entry->run_on_error = run_on_error;
    entry->next = exit_funcs;
    exit_funcs = entry;
}

// Tactile feedback function, probably used for the Logitech Cyberman

void I_Tactile(int on, int off, int total)
{
}

// Zone memory auto-allocation: query the host for the largest free PSRAM
// block, then allocate (largest - reserve).  Falls back to progressively
// smaller attempts if the first one fails.

extern uint32_t host_psram_largest_free(void);

static byte *AutoAllocMemory(int *size, int default_kb, int min_kb)
{
    byte *zonemem = NULL;

    // Ask the host how much contiguous PSRAM is actually available
    uint32_t largest = host_psram_largest_free();
    uint32_t reserve = ZONE_RESERVE_KB * 1024;
    int try_bytes;

    if (largest > reserve + (uint32_t)(min_kb * 1024))
    {
        // Target: largest block minus reserve, rounded down to 64KB boundary
        try_bytes = (int)((largest - reserve) & ~(64*1024 - 1));
    }
    else
    {
        try_bytes = default_kb * 1024;
    }

    // Try progressively smaller until one succeeds
    while (zonemem == NULL)
    {
        if (try_bytes < min_kb * 1024)
        {
            I_Error("Unable to allocate zone memory (need %dKB, largest=%u)",
                    min_kb, (unsigned)host_psram_largest_free());
        }

        printf("Z_Init: trying %d.%d MiB (%d bytes, avail=%u)...\n",
               try_bytes / (1024*1024),
               (try_bytes % (1024*1024)) * 10 / (1024*1024),
               try_bytes, (unsigned)largest);
        zonemem = malloc(try_bytes);
        printf("Z_Init: malloc returned %p\n", zonemem);

        if (zonemem == NULL)
        {
            try_bytes -= 64 * 1024; // step down 64KB
        }
    }

    *size = try_bytes;
    return zonemem;
}

byte *I_ZoneBase (int *size)
{
    byte *zonemem;
    int min_kb, default_kb;
    int p;

    //!
    // @arg <mb>
    //
    // Specify the heap size, in MiB.
    //

    p = M_CheckParmWithArgs("-mb", 1);

    if (p > 0)
    {
        default_kb = atoi(myargv[p+1]) * 1024;
        min_kb = default_kb;
    }
    else
    {
        default_kb = DEFAULT_RAM_KB;
        min_kb = MIN_RAM_KB;
    }

    zonemem = AutoAllocMemory(size, default_kb, min_kb);

    printf("zone memory: %p, %x allocated for zone\n",
           zonemem, *size);

    return zonemem;
}

void I_PrintBanner(char *msg)
{
    int i;
    int spaces = 35 - (strlen(msg) / 2);

    for (i=0; i<spaces; ++i)
        putchar(' ');

    puts(msg);
}

void I_PrintDivider(void)
{
    int i;

    for (i=0; i<75; ++i)
    {
        putchar('=');
    }

    putchar('\n');
}

void I_PrintStartupBanner(char *gamedescription)
{
    I_PrintDivider();
    I_PrintBanner(gamedescription);
    I_PrintDivider();
    
    printf(
    " " PACKAGE_NAME " is free software, covered by the GNU General Public\n"
    " License.  There is NO warranty; not even for MERCHANTABILITY or FITNESS\n"
    " FOR A PARTICULAR PURPOSE. You are welcome to change and distribute\n"
    " copies under certain conditions. See the source for more information.\n");

    I_PrintDivider();
}

// 
// I_ConsoleStdout
//
// Returns true if stdout is a real console, false if it is a file
//

boolean I_ConsoleStdout(void)
{
#ifdef _WIN32
    // SDL "helpfully" always redirects stdout to a file.
    return 0;
#else
#if ORIGCODE
    return isatty(fileno(stdout));
#else
	return 0;
#endif
#endif
}

//
// I_Init
//
/*
void I_Init (void)
{
    I_CheckIsScreensaver();
    I_InitTimer();
    I_InitJoystick();
}
void I_BindVariables(void)
{
    I_BindVideoVariables();
    I_BindJoystickVariables();
    I_BindSoundVariables();
}
*/

//
// I_Quit
//

void I_Quit (void)
{
    atexit_listentry_t *entry;

    // Run through all exit functions
 
    entry = exit_funcs; 

    while (entry != NULL)
    {
        entry->func();
        entry = entry->next;
    }

#if ORIGCODE
    SDL_Quit();

    exit(0);
#endif
}

#if !defined(_WIN32) && !defined(__MACOSX__) && !defined(__DJGPP__)
#define ZENITY_BINARY "/usr/bin/zenity"

// returns non-zero if zenity is available

static int ZenityAvailable(void)
{
    return system(ZENITY_BINARY " --help >/dev/null 2>&1") == 0;
}

// Escape special characters in the given string so that they can be
// safely enclosed in shell quotes.

static char *EscapeShellString(char *string)
{
    char *result;
    char *r, *s;

    // In the worst case, every character might be escaped.
    result = malloc(strlen(string) * 2 + 3);
    r = result;

    // Enclosing quotes.
    *r = '"';
    ++r;

    for (s = string; *s != '\0'; ++s)
    {
        // From the bash manual:
        //
        //  "Enclosing characters in double quotes preserves the literal
        //   value of all characters within the quotes, with the exception
        //   of $, `, \, and, when history expansion is enabled, !."
        //
        // Therefore, escape these characters by prefixing with a backslash.

        if (strchr("$`\\!", *s) != NULL)
        {
            *r = '\\';
            ++r;
        }

        *r = *s;
        ++r;
    }

    // Enclosing quotes.
    *r = '"';
    ++r;
    *r = '\0';

    return result;
}

// Open a native error box with a message using zenity

static int ZenityErrorBox(char *message)
{
    int result;
    char *escaped_message;
    char *errorboxpath;
    static size_t errorboxpath_size;

    if (!ZenityAvailable())
    {
        return 0;
    }

    escaped_message = EscapeShellString(message);

    errorboxpath_size = strlen(ZENITY_BINARY) + strlen(escaped_message) + 19;
    errorboxpath = malloc(errorboxpath_size);
    M_snprintf(errorboxpath, errorboxpath_size, "%s --error --text=%s",
               ZENITY_BINARY, escaped_message);

    result = system(errorboxpath);

    free(errorboxpath);
    free(escaped_message);

    return result;
}

#endif /* !defined(_WIN32) && !defined(__MACOSX__) && !defined(__DJGPP__) */


//
// I_Error
//

static boolean already_quitting = false;

void I_Error (char *error, ...)
{
    char msgbuf[512];
    va_list argptr;
    atexit_listentry_t *entry;
    boolean exit_gui_popup;

    if (already_quitting)
    {
        fprintf(stderr, "Warning: recursive call to I_Error detected.\n");
#if ORIGCODE
        exit(-1);
#endif
    }
    else
    {
        already_quitting = true;
    }

    // Message first.
    va_start(argptr, error);
    //fprintf(stderr, "\nError: ");
    vfprintf(stderr, error, argptr);
    fprintf(stderr, "\n\n");
    va_end(argptr);
    fflush(stderr);

    // Write a copy of the message into buffer.
    va_start(argptr, error);
    memset(msgbuf, 0, sizeof(msgbuf));
    M_vsnprintf(msgbuf, sizeof(msgbuf), error, argptr);
    va_end(argptr);

    // Shutdown. Here might be other errors.

    entry = exit_funcs;

    while (entry != NULL)
    {
        if (entry->run_on_error)
        {
            entry->func();
        }

        entry = entry->next;
    }

    exit_gui_popup = !M_ParmExists("-nogui");

    // Pop up a GUI dialog box to show the error message, if the
    // game was not run from the console (and the user will
    // therefore be unable to otherwise see the message).
    if (exit_gui_popup && !I_ConsoleStdout())
#ifdef _WIN32
    {
        wchar_t wmsgbuf[512];

        MultiByteToWideChar(CP_ACP, 0,
                            msgbuf, strlen(msgbuf) + 1,
                            wmsgbuf, sizeof(wmsgbuf));

        MessageBoxW(NULL, wmsgbuf, L"", MB_OK);
    }
#elif defined(__MACOSX__)
    {
        CFStringRef message;
	int i;

	// The CoreFoundation message box wraps text lines, so replace
	// newline characters with spaces so that multiline messages
	// are continuous.

	for (i = 0; msgbuf[i] != '\0'; ++i)
        {
            if (msgbuf[i] == '\n')
            {
                msgbuf[i] = ' ';
            }
        }

        message = CFStringCreateWithCString(NULL, msgbuf,
                                            kCFStringEncodingUTF8);

        CFUserNotificationDisplayNotice(0,
                                        kCFUserNotificationCautionAlertLevel,
                                        NULL,
                                        NULL,
                                        NULL,
                                        CFSTR(PACKAGE_STRING),
                                        message,
                                        NULL);
    }
#elif defined(__DJGPP__)
    {
        printf("%s\n", msgbuf);
        exit(-1);
    }

#else
    {
        ZenityErrorBox(msgbuf);
    }
#endif

    // abort();
#if ORIGCODE
    SDL_Quit();

    exit(-1);
#else
    exit(-1);
#endif
}

//
// Read Access Violation emulation.
//
// From PrBoom+, by entryway.
//

// C:\>debug
// -d 0:0
//
// DOS 6.22:
// 0000:0000  (57 92 19 00) F4 06 70 00-(16 00)
// DOS 7.1:
// 0000:0000  (9E 0F C9 00) 65 04 70 00-(16 00)
// Win98:
// 0000:0000  (9E 0F C9 00) 65 04 70 00-(16 00)
// DOSBox under XP:
// 0000:0000  (00 00 00 F1) ?? ?? ?? 00-(07 00)

#define DOS_MEM_DUMP_SIZE 10

static const unsigned char mem_dump_dos622[DOS_MEM_DUMP_SIZE] = {
  0x57, 0x92, 0x19, 0x00, 0xF4, 0x06, 0x70, 0x00, 0x16, 0x00};
static const unsigned char mem_dump_win98[DOS_MEM_DUMP_SIZE] = {
  0x9E, 0x0F, 0xC9, 0x00, 0x65, 0x04, 0x70, 0x00, 0x16, 0x00};
static const unsigned char mem_dump_dosbox[DOS_MEM_DUMP_SIZE] = {
  0x00, 0x00, 0x00, 0xF1, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00};
static unsigned char mem_dump_custom[DOS_MEM_DUMP_SIZE];

static const unsigned char *dos_mem_dump = mem_dump_dos622;

boolean I_GetMemoryValue(unsigned int offset, void *value, int size)
{
    static boolean firsttime = true;

    if (firsttime)
    {
        int p, i, val;

        firsttime = false;
        i = 0;

        //!
        // @category compat
        // @arg <version>
        //
        // Specify DOS version to emulate for NULL pointer dereference
        // emulation.  Supported versions are: dos622, dos71, dosbox.
        // The default is to emulate DOS 7.1 (Windows 98).
        //

        p = M_CheckParmWithArgs("-setmem", 1);

        if (p > 0)
        {
            if (!strcasecmp(myargv[p + 1], "dos622"))
            {
                dos_mem_dump = mem_dump_dos622;
            }
            if (!strcasecmp(myargv[p + 1], "dos71"))
            {
                dos_mem_dump = mem_dump_win98;
            }
            else if (!strcasecmp(myargv[p + 1], "dosbox"))
            {
                dos_mem_dump = mem_dump_dosbox;
            }
            else
            {
                for (i = 0; i < DOS_MEM_DUMP_SIZE; ++i)
                {
                    ++p;

                    if (p >= myargc || myargv[p][0] == '-')
                    {
                        break;
                    }

                    M_StrToInt(myargv[p], &val);
                    mem_dump_custom[i++] = (unsigned char) val;
                }

                dos_mem_dump = mem_dump_custom;
            }
        }
    }

    switch (size)
    {
    case 1:
        *((unsigned char *) value) = dos_mem_dump[offset];
        return true;
    case 2:
        *((unsigned short *) value) = dos_mem_dump[offset]
                                    | (dos_mem_dump[offset + 1] << 8);
        return true;
    case 4:
        *((unsigned int *) value) = dos_mem_dump[offset]
                                  | (dos_mem_dump[offset + 1] << 8)
                                  | (dos_mem_dump[offset + 2] << 16)
                                  | (dos_mem_dump[offset + 3] << 24);
        return true;
    }

    return false;
}

