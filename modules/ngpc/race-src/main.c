//---------------------------------------------------------------------------
//	This program is free software; you can redistribute it and/or modify
//	it under the terms of the GNU General Public License as published by
//	the Free Software Foundation; either version 2 of the License, or
//	(at your option) any later version. See also the license.txt file for
//	additional informations.
//---------------------------------------------------------------------------

#include "types.h"
#include "main.h"

#include <libretro.h>
#include <streams/file_stream.h>

#include "flash.h"
#include "race-memory.h"
#include "tlcs900h.h"

#include "graphics.h"
#include "tlcs900h.h"

#include "neopopsound.h"

#ifdef DRZ80
#include "DrZ80_support.h"
#else
#if defined(CZ80)
#include "cz80_support.h"
#else
#include "z80.h"
#endif
#endif

extern int finscan;
extern int language;
extern int tipo_consola;

extern retro_log_printf_t log_cb;

/* standard VRAM table adresses */
unsigned char *sprite_table           = NULL;
unsigned char *pattern_table          = NULL;
unsigned short *patterns              = NULL;
unsigned short *tile_table_front      = NULL;
unsigned short *tile_table_back       = NULL;
unsigned short *palette_table         = NULL;
unsigned char *bw_palette_table       = NULL;
unsigned char *sprite_palette_numbers = NULL;

/* VDP registers
 *
 * where is the vdp rendering now on the lcd display?
 */

unsigned char *scanlineY     = NULL;
/* frame 0/1 priority registers */
unsigned char *frame0Pri     = NULL;
unsigned char *frame1Pri     = NULL;
/* windowing registers */
unsigned char *wndTopLeftX   = NULL;
unsigned char *wndTopLeftY   = NULL; 
unsigned char *wndSizeX      = NULL;
unsigned char *wndSizeY      = NULL;
/* scrolling registers */
unsigned char *scrollSpriteX = NULL;
unsigned char *scrollSpriteY = NULL;
unsigned char *scrollFrontX  = NULL;
unsigned char *scrollFrontY  = NULL;
unsigned char *scrollBackX   = NULL;
unsigned char *scrollBackY   = NULL;
/* background color selection register and table */
unsigned char *bgSelect      = NULL;
unsigned short *bgTable      = NULL;
unsigned char *oowSelect     = NULL;
unsigned short *oowTable     = NULL;
/* machine constants */
unsigned char *color_switch  = NULL;

unsigned char *rasterY       = NULL;

int		m_bIsActive;
EMUINFO		m_emuInfo;

void mainemuinit(void)
{
   sprite_table           = get_address(0x00008800);
   pattern_table          = get_address(0x0000A000);
   patterns               = (unsigned short*)pattern_table;
   tile_table_front       = (unsigned short *)get_address(0x00009000);
   tile_table_back        = (unsigned short *)get_address(0x00009800);
   palette_table          = (unsigned short *)get_address(0x00008200);
   bw_palette_table       = get_address(0x00008100);
   sprite_palette_numbers = get_address(0x00008C00);

   /* VDP registers
    *
    * where is the vdp rendering now on the lcd display?
    */
   scanlineY              = get_address(0x00008009);
   /* frame 0/1 priority registers */
   frame0Pri              = get_address(0x00008000);
   frame1Pri              = get_address(0x00008030);
   /* windowing registers */
   wndTopLeftX            = get_address(0x00008002);
   wndTopLeftY            = get_address(0x00008003);
   wndSizeX               = get_address(0x00008004);
   wndSizeY               = get_address(0x00008005);
   /* scrolling registers */
   scrollSpriteX          = get_address(0x00008020);
   scrollSpriteY          = get_address(0x00008021);
   scrollFrontX           = get_address(0x00008032);
   scrollFrontY           = get_address(0x00008033);
   scrollBackX            = get_address(0x00008034);
   scrollBackY            = get_address(0x00008035);
   /* background color selection register and table */
   bgSelect               = get_address(0x00008118);
   bgTable                = (unsigned short *)get_address(0x000083E0);
   oowSelect              = get_address(0x00008012);
   oowTable               = (unsigned short *)get_address(0x000083F0);
/* machine constants */
   color_switch           = get_address(0x00006F91);
   rasterY                = get_address(0x00008035);

   /* Initialize CPU memory */
   mem_init();
   graphics_init();

   /* initialize the TLCS-900H CPU */
   tlcs_init();

   /* (Re)seed the deterministic real-time clock base. This runs on both load
    * and reset, so the emulated clock starts from a known point and then
    * advances by counted frames. */
   rtc_reset();

#if defined(CZ80)
   Z80_Init();
#endif
#if defined(DRZ80) || defined(CZ80)
   Z80_Reset();
#else
   z80Init();
#endif

   /* If NeoGeo Pocket Color ROM, act if we are a NeoGeo Pocket Color */
   tlcsMemWriteB(0x6F91,tlcsMemReadB(0x00200023));
   if (tipo_consola==1)
      tlcsMemWriteB(0x6F91,0x00);

   /* Pretend we're running in English mode */

   /* NOTA setting_ngp_language 00 Ingles - 01 Jap */
   if (setting_ngp_language == 0)
      tlcsMemWriteB(0x00006F87,0x01);
   if (setting_ngp_language == 1)
      tlcsMemWriteB(0x00006F87,0x00);

   /* kludges & fixes */
   switch (tlcsMemReadW(0x00200020))
   {
      case 0x0059:	/* Sonic          */
      case 0x0061:	/* Metal Slug 2nd */
         *get_address(0x0020001F) = 0xFF;
         break;
   }
   ngpSoundOff();
}

static int initRom(void)
{
   int i, m;
   char	*license_info = " BY SNK CORPORATION";
   int rom_found = 1;

   finscan=198;

   if (mainrom[0x000020] == 0x65 || mainrom[0x000020] == 0x93)
      finscan=199;

   /* first stop the current emulation */
   m_emuInfo.machine = NGPC;
   m_bIsActive = 0;

   /* check NEOGEO POCKET
    * check license info */
   for (i=0;i<19;i++)
   {
      if (mainrom[0x000009 + i] != license_info[i])
         rom_found = 0;
   }

   if (rom_found)
   {
      i = mainrom[0x000023];
      if (i == 0x10 || i == 0x00)
      {
         /* initialize emulation */
         if (i == 0x10)
            m = NGPC;
         else
         {
            /* fix for missing Mono/Color setting in Cool Coom Jam SAMPLE ROM */
            if (mainrom[0x000020] == 0x34 && mainrom[0x000021] == 0x12)
               m = NGPC;
            else
               m = NGP;
         }
         if (tipo_consola==1)
            m = NGP;

	 m_emuInfo.machine = m;

         mainemuinit();
         /* start running the emulation loop */
	 m_bIsActive = 1;
         return 1;
      }

      if (log_cb)
         log_cb(RETRO_LOG_ERROR, "Not a valid or unsupported ROM file.\n");
   }
   else
   {
	   if (log_cb)
		   log_cb(RETRO_LOG_ERROR, "Not a valid or unsupported ROM file. rom_found == 0\n");
   }

   return 0;
}

static void initSysInfo(void)
{
	m_bIsActive       = 0;

	m_emuInfo.machine = NGPC;
	m_emuInfo.romSize = 0;

	strcpy(m_emuInfo.RomFileName, "");
}

int handleInputFile(const char *romName,
		const unsigned char *romData, int romSize)
{
	initSysInfo();  /* initialize it all */

	if (romData)
	{
		int size = romSize > MAINROM_SIZE_MAX ?
				MAINROM_SIZE_MAX : romSize;

		/* Fill unused cart space with open-bus (0xFF) so reads past the
		 * actual ROM are deterministic and do not retain stale data from
		 * a previously loaded game. */
		memset(mainrom, 0xFF, MAINROM_SIZE_MAX);

		m_emuInfo.romSize = size;
		memcpy(mainrom, romData, size);
		strncpy(m_emuInfo.RomFileName, romName,
				sizeof(m_emuInfo.RomFileName) - 1);
		m_emuInfo.RomFileName[sizeof(m_emuInfo.RomFileName) - 1] = '\0';
	}
	else
	{
		int64_t size   = 0;
		/* get ROM from binary ROM file */
		RFILE *romFile = filestream_open(romName,
				RETRO_VFS_FILE_ACCESS_READ,
				RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if(!romFile)
		{
			if (log_cb)
				log_cb(RETRO_LOG_ERROR, "Couldn't open %s file\n", romName);
			return 0;
		}

		/* Fill unused cart space with open-bus (0xFF) before reading so a
		 * ROM smaller than the cart leaves deterministic bytes and does
		 * not retain data from a previously loaded game. */
		memset(mainrom, 0xFF, MAINROM_SIZE_MAX);

		size = filestream_read(romFile, mainrom, MAINROM_SIZE_MAX);
		filestream_close(romFile);

		if (size <= 0)
		{
			if (log_cb)
				log_cb(RETRO_LOG_ERROR, "Couldn't read %s file\n", romName);
			return 0;
		}

		m_emuInfo.romSize = (int)size;
		strncpy(m_emuInfo.RomFileName, romName,
				sizeof(m_emuInfo.RomFileName) - 1);
		m_emuInfo.RomFileName[sizeof(m_emuInfo.RomFileName) - 1] = '\0';
	}

	if (!initRom())
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "initRom couldn't handle %s file\n", romName);
		return 0;
	}

	setFlashSize(m_emuInfo.romSize);
	return 1;
}
