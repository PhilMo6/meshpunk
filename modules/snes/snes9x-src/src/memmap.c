/* This file is part of Snes9x. See LICENSE file. */

#include <stdio.h>
#include <string.h>
#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif
#include <ctype.h>

#include "snes9x.h"
#include "memmap.h"
#include "cpuexec.h"
#include "ppu.h"
#include "fxemu.h"
#include "sa1.h"
#include "display.h"
#include "apu.h"
#include "dsp.h"
#include "srtc.h"

#ifdef __W32_HEAP
#include <malloc.h>
#endif

#ifdef _MSC_VER
/* Necessary to build on MSVC */
#define strnicmp _strnicmp
#endif

#define MAP_HIROM_SRAM_OR_NONE (Memory.SRAMSize == 0 ? (uint8_t*) MAP_NONE : (uint8_t*) MAP_HIROM_SRAM)
#define MAP_LOROM_SRAM_OR_NONE (Memory.SRAMSize == 0 ? (uint8_t*) MAP_NONE : (uint8_t*) MAP_LOROM_SRAM)
#define MAP_RONLY_SRAM_OR_NONE (Memory.SRAMSize == 0 ? (uint8_t*) MAP_NONE : (uint8_t*) MAP_RONLY_SRAM)

static uint8_t *bytes0x2000; //  [0x2000];

static bool AllASCII(const uint8_t* b, int32_t size)
{
   int32_t i;
   for (i = 0; i < size; i++)
      if (b[i] < 32 || b[i] > 126)
         return false;
   return true;
}

static int32_t ScoreHiROM(bool skip_header, int32_t romoff)
{
   int32_t o = skip_header ? 0xff00 + 0x200 : 0xff00;
   const uint8_t *base = &Memory.ROM[o + romoff];
   int32_t score = 0;

   if (base[0xd5] & 0x1)
      score += 2;

   /* Mode23 is SA-1 */
   if (base[0xd5] == 0x23)
      score -= 2;

   if (base[0xd4] == 0x20)
      score += 2;

   if ((base[0xdc] + (base[0xdd] << 8) + base[0xde] + (base[0xdf] << 8)) == 0xffff)
   {
      score += 2;
      if (0 != (base[0xde] + (base[0xdf] << 8)))
         score++;
   }

   if (base[0xda] == 0x33)
      score += 2;
   if ((base[0xd5] & 0xf) < 4)
      score += 2;
   if (!(base[0xfd] & 0x80))
      score -= 6;
   if ((base[0xfc] | (base[0xfd] << 8)) > 0xFFB0)
      score -= 2; /* reduced after looking at a scan by Cowering */
   if (Memory.CalculatedSize > 1024 * 1024 * 3)
      score += 4;
   if ((1 << (base[0xd7] - 7)) > 48)
      score -= 1;
   if (!AllASCII(&base[0xb0], 6))
      score -= 1;
   if (!AllASCII(&base[0xc0], ROM_NAME_LEN))
      score -= 1;

   return score;
}

static int32_t ScoreLoROM(bool skip_header, int32_t romoff)
{
   int32_t o = skip_header ? 0x7f00 + 0x200 : 0x7f00;
   const uint8_t *base = &Memory.ROM[o + romoff];
   int32_t score = 0;

   if (!(base[0xd5] & 0x1))
      score += 3;

   /* Mode23 is SA-1 */
   if (base[0xd5] == 0x23)
      score += 2;

   if ((base[0xdc] + (base[0xdd] << 8) + base[0xde] + (base[0xdf] << 8)) == 0xffff)
   {
      score += 2;
      if (0 != (base[0xde] + (base[0xdf] << 8)))
         score++;
   }

   if (base[0xda] == 0x33)
      score += 2;
   if ((base[0xd5] & 0xf) < 4)
      score += 2;
   if (Memory.CalculatedSize <= 1024 * 1024 * 16)
      score += 2;
   if (!(base[0xfd] & 0x80))
      score -= 6;
   if ((base[0xfc] | (base[0xfd] << 8)) > 0xFFB0)
      score -= 2; /* reduced per Cowering suggestion */
   if ((1 << (base[0xd7] - 7)) > 48)
      score -= 1;
   if (!AllASCII(&base[0xb0], 6))
      score -= 1;
   if (!AllASCII(&base[0xc0], ROM_NAME_LEN))
      score -= 1;

   return score;
}

static void Sanitize(char* str, size_t bufsize)
{
   str[bufsize - 1] = 0;
   for (char *ptr = str; *ptr; ++ptr)
      if (*ptr < 32 || *ptr >= 127)
         *ptr = '?';
}

/**********************************************************************************************/
/* S9xInitMemory()                                                                                     */
/* This function allocates and zeroes all the memory needed by the emulator                   */
/**********************************************************************************************/
bool S9xInitMemory(void)
{
   Memory.RAM   = (uint8_t*)malloc(RAM_SIZE);
   Memory.SRAM  = (uint8_t*)malloc(SRAM_SIZE);
   Memory.VRAM  = (uint8_t*)malloc(VRAM_SIZE);
   Memory.FillRAM = (uint8_t*)malloc(0x8000);

   Memory.Map = (uint8_t**)calloc(MEMMAP_NUM_BLOCKS, sizeof(uint8_t*));
   Memory.MapInfo = (SMapInfo*)calloc(MEMMAP_NUM_BLOCKS, sizeof(SMapInfo));

   IPPU.ScreenColors = (uint16_t *)calloc(256 * 9, sizeof(uint16_t));
   IPPU.DirectColors = IPPU.ScreenColors + 256;
   IPPU.TileCache = (uint8_t*) calloc(MAX_2BIT_TILES, 128);
   IPPU.TileCached = (uint8_t*) calloc(MAX_2BIT_TILES, 1);

   bytes0x2000 = (uint8_t *)malloc(0x2000);

   // Try to find the biggest (commercial) ROM size that can fit in our available memory.
   /* MESHPUNK: fine-grained rungs. A coarse ladder strands carts: dropping
      straight from 0x400000 to 0x200000 leaves a 3MB cart nowhere to go even
      when a 3MB block is free. Where the module image lands moves the largest
      free block by hundreds of KB from one launch to the next, so the rung
      that fits is not predictable. Each rung costs one failed malloc. */
   const size_t AllocSizes[] = {0x600000, 0x400000, 0x300000, 0x280000,
                                0x200000, 0x100000, 0x80000, 0};
   for (const size_t *size = AllocSizes; *size && !Memory.ROM; ++size)
   {
      Memory.ROM_AllocSize = *size + 0x10000 + 0x200; // Extra 64KB for mapping purposes
      Memory.ROM = (uint8_t *)malloc(Memory.ROM_AllocSize);
   }

   if (!Memory.RAM || !Memory.SRAM || !Memory.VRAM || !Memory.ROM || !Memory.Map || !Memory.MapInfo
      || !IPPU.ScreenColors || !IPPU.TileCache || !IPPU.TileCached || !bytes0x2000)
   {
      S9xDeinitMemory();
      return false;
   }

   return true;
}

void S9xDeinitMemory(void)
{
   free(Memory.RAM);
   Memory.RAM = NULL;

   free(Memory.SRAM);
   Memory.SRAM = NULL;

   free(Memory.VRAM);
   Memory.VRAM = NULL;

   if (Memory.ROM)
      free(Memory.ROM - Memory.ROM_Offset);
   Memory.ROM = NULL;
   Memory.ROM_Offset = 0;
   Memory.ROM_AllocSize = 0;

   free(Memory.FillRAM);
   Memory.FillRAM = NULL;

   free(Memory.Map);
   Memory.Map = NULL;

   free(Memory.MapInfo);
   Memory.MapInfo = NULL;

   free(IPPU.ScreenColors);
   IPPU.ScreenColors = NULL;

   free(IPPU.TileCached);
   IPPU.TileCached = NULL;

   free(IPPU.TileCache);
   IPPU.TileCache = NULL;

   free(bytes0x2000);
   bytes0x2000 = NULL;
}

/**********************************************************************************************/
/* LoadROM()                                                                                  */
/* This function loads a Snes-Backup image                                                    */
/**********************************************************************************************/

bool LoadROM(const char* filename)
{
   size_t TotalFileSize = 0;
   FILE *fp;

   printf("Loading ROM: '%s'\n", filename ?: "(Memory.ROM)");

   if (Memory.ROM_Offset)
   {
      Memory.ROM -= Memory.ROM_Offset;
      Memory.ROM_Offset = 0;
   }

   if (filename == NULL)
   {
      TotalFileSize = Memory.ROM_AllocSize;
   }
   else if ((fp = fopen(filename, "rb")))
   {
      fseek(fp, 0, SEEK_END);
      TotalFileSize = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      fread(Memory.ROM, Memory.ROM_AllocSize, 1, fp);
      fclose(fp);
   }
   else
   {
      printf("Failed to open %s\n", filename);
      return false;
   }

   if (TotalFileSize > Memory.ROM_AllocSize)
   {
      printf("WARNING: ROM TOO BIG (%u)!\n", (unsigned)TotalFileSize);
      TotalFileSize = Memory.ROM_AllocSize;
      return false; // comment to try to run it anyway
   }
   else if (TotalFileSize < 1024)
   {
      return false; /* it ends here */
   }

   if ((TotalFileSize & 0x7FF) == 512)
   {
      printf("Skipping header\n");
      Memory.ROM += 512;
      Memory.ROM_Offset += 512;
      TotalFileSize -= 512;
   }

   Memory.CalculatedSize = TotalFileSize & ~0x1FFF; /* round down to lower 0x2000 */
   Memory.ExtendedFormat = NOPE;

   ApplyROMPatches();

   /* CalculatedSize is now set, so rescore */
   int32_t hi_score = ScoreHiROM(false, 0);
   int32_t lo_score = ScoreLoROM(false, 0);

   /* you might be a Jumbo! */
   if ((Memory.CalculatedSize > 0x400000 &&
         !(Memory.ROM[0x7FD5] == 0x32 && ((Memory.ROM[0x7FD6] & 0xF0) == 0x40)) && /* exclude S-DD1 */
         !(Memory.ROM[0xFFD5] == 0x3A && ((Memory.ROM[0xFFD6] & 0xF0) == 0xF0))) /* exclude SPC7110 */)
   {
      int32_t swappedlorom = ScoreLoROM(false, 0x400000);
      int32_t swappedhirom = ScoreHiROM(false, 0x400000);

      /* set swapped here. */
      if (MAX(swappedlorom, swappedhirom) >= MAX(lo_score, hi_score))
      {
         hi_score = swappedhirom;
         lo_score = swappedlorom;
         Memory.ExtendedFormat = BIGFIRST;
      }
      else
      {
         Memory.ExtendedFormat = SMALLFIRST;
      }
   }

   Memory.LoROM = (Settings.ForceLoROM || (!Settings.ForceHiROM && lo_score >= hi_score));
   Memory.HiROM = !Memory.LoROM;

   /* More */
   if (!Settings.ForceHiROM &&
       !Settings.ForceLoROM &&
       !Settings.ForceInterleaved &&
       !Settings.ForceInterleaved2 &&
       !Settings.ForceNotInterleaved &&
       !Settings.ForceSuperFX &&
       !Settings.ForceNoSuperFX &&
       !Settings.ForceDSP1 &&
       !Settings.ForceNoDSP1 &&
       !Settings.ForceSA1 &&
       !Settings.ForceNoSA1 &&
       !Settings.ForceC4 &&
       !Settings.ForceNoC4 &&
       !Settings.ForceSDD1 &&
       !Settings.ForceNoSDD1)
   {
      if (strncmp ((char *) &Memory.ROM [0x7fc0], "YUYU NO QUIZ DE GO!GO!", 22) == 0 || strncmp ((char *) &Memory.ROM [0x7fc0], "SP MOMOTAROU DENTETSU2", 22) == 0 || strncmp ((char *) &Memory.ROM [0x7fc0], "SUPER FORMATION SOCCE", 21) == 0)
      {
         Memory.LoROM = true;
         Memory.HiROM = false;
      }
      /* BS Zooっと麻雀 */
      else if ((strncmp ((char *) &Memory.ROM [0xffc0], "Ｚｏｏっと麻雀！", 16) == 0)|| (strncmp ((char *) &Memory.ROM [0xffc0], "Zooっと麻雀!IVT", 15) == 0))
      {
         Memory.LoROM = false;
         Memory.HiROM = true;
      }
      /* 再BS探偵倶楽部 */
      else if (strncmp ((char *) &Memory.ROM [0xffc0], "再BS探偵倶楽部", 14) == 0)
      {
         Memory.LoROM = false;
         Memory.HiROM = true;
      }
      /* BATMAN--REVENGE JOKER (USA) */
      else if (strncmp ((char *) &Memory.ROM [0xffc0], "BATMAN--REVENGE JOKER", 21) == 0)
      {
         Memory.LoROM = true;
         Memory.HiROM = false;
      }
      /* THE DUEL: TEST DRIVE */
      else if (strncmp ((char *) &Memory.ROM [0x7fc0], "THE DUEL: TEST DRIVE", 20) == 0)
      {
         Memory.LoROM = true;
         Memory.HiROM = false;
      }
      /* ポパイ いじわる魔女シーハッグの巻 */
      else if (strncmp ((char *) &Memory.ROM [0x7fc0], "POPEYE IJIWARU MAJO", 19) == 0)
      {
         Memory.LoROM = true;
         Memory.HiROM = false;
      }
      /* Pop'nツインビー サンプル版 */
      else if(strncmp ((char *) &Memory.ROM [0x7fc0], "POPN TWINBEE", 12) == 0)
      {
         Memory.LoROM = true;
         Memory.HiROM = false;
      }
      else if(Memory.CalculatedSize == 0x100000 && strncmp ((char *) &Memory.ROM [0xffc0], "WWF SUPER WRESTLEMANIA", 22) == 0)
      {
         Memory.LoROM = true;
         Memory.HiROM = false;
      }
   }

   bool Tales = Memory.ExtendedFormat == SMALLFIRST;
   InitROM(Tales);

   /* MESHPUNK: no S-DD1, SPC7110 or SETA emulation exists in this port —
    * booting such a cart executes garbage (the S-CPU reads open bus where
    * the chip's registers and RAM should be). Refuse the load; the launcher
    * shows the user-facing message from its own header check. */
   if (Settings.SDD1 || Settings.SPC7110 || Settings.SETA)
   {
      printf("Unsupported coprocessor (%s) — refusing cart\n",
             Settings.SDD1 ? "S-DD1" :
             Settings.SPC7110 ? "SPC7110" : "SETA ST01x");
      return false;
   }

   S9xReset();
   return true;
}

void ParseSNESHeader(uint8_t* RomHeader)
{
   Memory.SRAMSize = RomHeader [0x28];
   memcpy(Memory.ROMName, &RomHeader[0x10], ROM_NAME_LEN);
   Memory.ROMName[ROM_NAME_LEN] = 0;
   Memory.ROMSpeed = RomHeader [0x25];
   Memory.ROMType = RomHeader [0x26];
   Memory.ROMSize = RomHeader [0x27];
   Memory.ROMChecksum = RomHeader [0x2e] + (RomHeader [0x2f] << 8);
   Memory.ROMComplementChecksum = RomHeader [0x2c] + (RomHeader [0x2d] << 8);
   Memory.ROMRegion = RomHeader[0x29];
   memcpy(Memory.ROMId, &RomHeader [0x2], 4);
   Memory.ROMId[4] = 0;
   if (RomHeader[0x2A] == 0x33)
      memcpy(Memory.CompanyId, &RomHeader[0], 2);
   else
      sprintf(Memory.CompanyId, "%02X", RomHeader[0x2A]);
   Memory.CompanyId[2] = 0;
}

void InitROM(bool Interleaved)
{
   uint8_t* RomHeader;
   uint32_t sum1 = 0;
   uint32_t sum2 = 0;

   Settings.MultiPlayer5Master = Settings.MultiPlayer5;
   Settings.MouseMaster = Settings.Mouse;
   Settings.SuperScopeMaster = Settings.SuperScope;
   Settings.DSP1Master = Settings.ForceDSP1;
   Settings.SuperFX = false;
   Settings.DSP = 0;
   Settings.SA1 = false;
   Settings.C4 = false;
   Settings.SDD1 = false;
   Settings.SRTC = false;
   Settings.SPC7110 = false;
   Settings.SPC7110RTC = false;
   Settings.BS = false;
   Settings.OBC1 = false;
   Settings.SETA = false;
   Memory.CalculatedChecksum = 0;

   RomHeader = Memory.ROM + 0x7FB0;

   if (Memory.ExtendedFormat == BIGFIRST)
      RomHeader += 0x400000;

   if (Memory.HiROM)
      RomHeader += 0x8000;

   for (size_t i = 0; i < MEMMAP_NUM_BLOCKS; i++)
   {
      Memory.MapInfo[i].Type = 0;
   }

   ParseSNESHeader(RomHeader);

   /* Detect and initialize chips - detection codes are compatible with NSRT */

   /* DSP1/2/3/4 */
   if (Memory.ROMType == 0x03)
   {
      if (Memory.ROMSpeed == 0x30)
         Settings.DSP = 4; /* DSP4 */
      else
         Settings.DSP = 1; /* DSP1 */
   }
   else if (Memory.ROMType == 0x05)
   {
      if (Memory.ROMSpeed == 0x20)
         Settings.DSP = 2; /* DSP2 */
      else if (Memory.ROMSpeed == 0x30 && RomHeader[0x2a] == 0xb2)
         Settings.DSP = 3; /* DSP3 */
      else
         Settings.DSP = 1; /* DSP1 */
   }

   if (Settings.DSP)
      S9xInitDSP();

   if(!Settings.ForceNoDSP1 && Settings.DSP)
      Settings.DSP1Master = true;

   if (Memory.HiROM)
   {
      /* Enable S-RTC (Real Time Clock) emulation for Dai Kaijyu Monogatari 2 */
      Settings.SRTC = ((Memory.ROMType & 0xf0) >> 4) == 5;

      if (((Memory.ROMSpeed & 0x0F) == 0x0A) && ((Memory.ROMType & 0xF0) == 0xF0))
      {
         Settings.SPC7110 = true;
         if ((Memory.ROMType & 0x0F) == 0x09)
            Settings.SPC7110RTC = true;
      }

      if ((Memory.ROMSpeed & ~0x10) == 0x25)
         TalesROMMap(Interleaved);
      else
         HiROMMap();
   }
   else
   {
      Settings.SuperFX = Settings.ForceSuperFX;

      if (Memory.ROMType == 0x25)
         Settings.OBC1 = true;

      if ((Memory.ROMType & 0xf0) == 0x10)
         Settings.SuperFX = !Settings.ForceNoSuperFX;

      /* OBC1 hack ROM */
      if (strncmp(Memory.ROMName, "METAL COMBAT", 12) == 0 && Memory.ROMType == 0x13 && Memory.ROMSpeed == 0x42)
      {
         Settings.OBC1 = true;
         Settings.SuperFX = Settings.ForceSuperFX;
         Memory.ROMSpeed = 0x30;
      }

      Settings.SDD1 = Settings.ForceSDD1;
      if ((Memory.ROMType & 0xf0) == 0x40)
         Settings.SDD1 = !Settings.ForceNoSDD1;

      /* MESHPUNK: SA-1 detection, snes9x2005's predicate. SETA (ST010/011/
       * ST018) stays detection-only: LoadROM refuses those carts. ROMSpeed
       * 0x3A with type 0xF5 is SPC7110 (HiROM branch), hence the exclusion
       * on ST018. */
      if (Settings.ForceSA1 ||
          (!Settings.ForceNoSA1 && (Memory.ROMSpeed & ~0x10) == 0x23 &&
           (Memory.ROMType & 0xf) > 3 && (Memory.ROMType & 0xf0) == 0x30))
         Settings.SA1 = true;
      if (Memory.ROMType == 0xf6 ||
          (Memory.ROMType == 0xf5 && (Memory.ROMSpeed & 0x0f) != 0x0a))
         Settings.SETA = true;

      if (((Memory.ROMType & 0xF0) == 0xF0) & ((Memory.ROMSpeed & 0x0F) != 5))
      {
         Memory.SRAMSize = 2;
      }
      Settings.C4 = Settings.ForceC4;
      if ((Memory.ROMType & 0xf0) == 0xf0 && (strncmp(Memory.ROMName, "MEGAMAN X", 9) == 0 || strncmp(Memory.ROMName, "ROCKMAN X", 9) == 0))
         Settings.C4 = !Settings.ForceNoC4;

      if (Settings.SuperFX)
      {
         /* GSU instructions per scanline at 10.7MHz with the 0.417
          * clocks-per-instruction factor folded in; S9xResetSuperFX scales
          * it by the cart's line timing. */
         Settings.SuperFXSpeedPerLine = 4378500;
         /* FX carts carry no usable SRAM size in the header this port reads,
          * so it arrives as 0 and masks the GSU RAM to nothing. That RAM is
          * the chip's framebuffer, so a zero mask is a black screen. */
         if (!Memory.SRAMSize)
            Memory.SRAMSize = 5;
         SuperFXROMMap();
      }
      else if (Settings.SA1)
         SA1ROMMap();
      else if ((Memory.ROMSpeed & ~0x10) == 0x25)
         TalesROMMap(Interleaved);
      else if (Memory.ExtendedFormat)
         JumboLoROMMap(Interleaved);
      else if (strncmp((char*) &Memory.ROM [0x7fc0], "SOUND NOVEL-TCOOL", 17) == 0 || strncmp((char*) &Memory.ROM [0x7fc0], "DERBY STALLION 96", 17) == 0)
      {
         LoROM24MBSMap();
         Settings.DSP1Master = false;
      }
      else if (strncmp((char*) &Memory.ROM [0x7fc0], "THOROUGHBRED BREEDER3", 21) == 0 || strncmp((char*) &Memory.ROM [0x7fc0], "RPG-TCOOL 2", 11) == 0)
      {
         SRAM512KLoROMMap();
         Settings.DSP1Master = false;
      }
      else if (strncmp((char*) &Memory.ROM [0x7fc0], "DEZAEMON  ", 10) == 0)
      {
         SRAM1024KLoROMMap();
         Settings.DSP1Master = false;
      }
      else if ((strncmp((char *) &Memory.ROM [0x7fc0], "ROCKMAN X  ", 11) == 0)|| (strncmp((char *) &Memory.ROM [0x7fc0], "MEGAMAN X  ", 11) == 0)|| (strncmp((char *) &Memory.ROM [0x7fc0], "demon's blazon", 14) == 0)|| (strncmp((char *) &Memory.ROM [0x7fc0], "demon's crest", 13) == 0))
         CapcomProtectLoROMMap();
      else if ((Memory.ROMSpeed & ~0x10) == 0x22 && strncmp(Memory.ROMName, "Super Street Fighter", 20) != 0)
         AlphaROMMap();
      else if (strncmp ((char *) &Memory.ROM [0x7fc0], "HITOMI3", 7) == 0)
      {
         Memory.SRAMSize = 3;
         LoROMMap();
      }
      else
         LoROMMap();
   }

   if (Memory.SRAMSize > 5)
   {
      printf("WARNING: SRAM was reduced to 64K by me. If you see this message, tell me about it.\n");
      Memory.SRAMSize = 5;
   }

   Memory.SRAMMask = Memory.SRAMSize ? ((1 << (Memory.SRAMSize + 3)) * 128) - 1 : 0;

   /*
   if (!Memory.CalculatedChecksum)
   {
      int32_t i;
      uint32_t remainder;
      int32_t power2 = 0;
      int32_t sub = 0;
      int32_t size = Memory.CalculatedSize;

      while (size >>= 1)
         power2++;

      size      = 1 << power2;
      remainder = Memory.CalculatedSize - size;

      for (i = 0; i < size; i++)
         sum1 += Memory.ROM [i];

      for (i = 0; i < (int32_t) remainder; i++)
         sum2 += Memory.ROM [size + i];

      if (remainder)
         sum1 += sum2 * (size / remainder);

      sum1 &= 0xffff;
      Memory.CalculatedChecksum = sum1;
   }
   */

   if (Settings.ForceNTSC)
      Settings.PAL = false;
   else if (Settings.ForcePAL)
      Settings.PAL = true;
   else
      Settings.PAL = !(Memory.ROMRegion == 13 || Memory.ROMRegion == 1 || Memory.ROMRegion == 0);

   if (Settings.PAL)
   {
      Settings.FrameTime = Settings.FrameTimePAL;
      Memory.ROMFramesPerSecond = 50;
   }
   else
   {
      Settings.FrameTime = Settings.FrameTimeNTSC;
      Memory.ROMFramesPerSecond = 60;
   }

   if (strlen(Memory.ROMName))
   {
      char* p = Memory.ROMName + strlen(Memory.ROMName) - 1;

      while (p > Memory.ROMName && *(p - 1) == ' ')
         p--;
      *p = 0;
   }

#ifndef USE_BLARGG_APU
   IAPU.OneCycle = ONE_APU_CYCLE;
#endif
   Settings.Shutdown = true;
   memset(bytes0x2000, 0, 0x2000);
   ResetSpeedMap();
   ApplyROMFixes();

   // Do this at the end, ApplyROMFixes needs the raw values
   Sanitize(Memory.ROMName, sizeof(Memory.ROMName));
   Sanitize(Memory.ROMId, sizeof(Memory.ROMId));
   Sanitize(Memory.CompanyId, sizeof(Memory.CompanyId));

   Settings.ForceHeader = false;
   Settings.ForceHiROM = false;
   Settings.ForceLoROM = false;
   Settings.ForceInterleaved = false;
   Settings.ForceNoHeader = false;
   Settings.ForceNotInterleaved = false;
   Settings.ForceInterleaved2 = false;

   printf("Rom loaded: name: %s, id: %s, company: %s, size: %dKB\n",
      Memory.ROMName, Memory.ROMId, Memory.CompanyId, (int)(Memory.CalculatedSize / 1024));
}

void FixROMSpeed(void)
{
   int32_t c;

   if (CPU.FastROMSpeed == 0)
      CPU.FastROMSpeed = SLOW_ONE_CYCLE;

   for (c = 0x800; c < 0x1000; c++)
      if (c & 0x8 || c & 0x400)
         Memory.MapInfo[c].Speed = (uint8_t) CPU.FastROMSpeed;
}

void ResetSpeedMap(void)
{
   int32_t i;
   for (i = 0; i < MEMMAP_NUM_BLOCKS; i++)
   {
      Memory.MapInfo[i].Speed = SLOW_ONE_CYCLE;
   }
   for (i = 0; i < 0x400; i += 0x10)
   {
      Memory.MapInfo[i + 2].Speed = Memory.MapInfo[0x800 + i + 2].Speed = ONE_CYCLE;
      Memory.MapInfo[i + 3].Speed = Memory.MapInfo[0x800 + i + 3].Speed = ONE_CYCLE;
      Memory.MapInfo[i + 4].Speed = Memory.MapInfo[0x800 + i + 4].Speed = ONE_CYCLE;
      Memory.MapInfo[i + 5].Speed = Memory.MapInfo[0x800 + i + 5].Speed = ONE_CYCLE;
   }
   FixROMSpeed();
}

void map_index(uint32_t bank_s, uint32_t bank_e, uint32_t addr_s, uint32_t addr_e, intptr_t index, int32_t type)
{
   uint32_t c, i, p;
   // bool isROM, isRAM;

   // isROM = !((type == MAP_TYPE_I_O) || (type == MAP_TYPE_RAM));
   // isRAM = !((type == MAP_TYPE_I_O) || (type == MAP_TYPE_ROM));

   for (c = bank_s; c <= bank_e; c++)
   {
      for (i = addr_s; i <= addr_e; i += 0x1000)
      {
         p = (c << 4) | (i >> 12);
         Memory.Map[p] = (uint8_t*) index;
         Memory.MapInfo[p].Type = type;
      }
   }
}

void WriteProtectROM(void)
{
}

void MapRAM(void)
{
   int32_t c, i;

   if (Memory.LoROM && !Settings.SDD1)
   {
      /* Banks 70->7d and f0->fe 0x0000-0x7FFF, S-RAM */
      for (c = 0; c < 0x0f; c++)
      {
         for (i = 0; i < 8; i++)
         {
            Memory.Map [(c << 4) + 0xF00 + i] = Memory.Map [(c << 4) + 0x700 + i] = MAP_LOROM_SRAM_OR_NONE;
            Memory.MapInfo[(c << 4) + 0xF00 + i].Type = Memory.MapInfo[(c << 4) + 0x700 + i].Type = MAP_TYPE_RAM;
         }
      }
      if(Memory.CalculatedSize <= 0x200000)
      {
         /* Banks 70->7d 0x8000-0xffff S-RAM */
         for (c = 0; c < 0x0e; c++)
         {
            for(i = 8; i < 16; i++)
            {
               Memory.Map [(c << 4) + 0x700 + i] = MAP_LOROM_SRAM_OR_NONE;
               Memory.MapInfo[(c << 4) + 0x700 + i].Type = MAP_TYPE_RAM;
            }
         }
      }
   }
   else if(Memory.LoROM && Settings.SDD1)
   {
      /* Banks 70->7d 0x0000-0x7FFF, S-RAM */
      for (c = 0; c < 0x0f; c++)
      {
         for (i = 0; i < 8; i++)
         {
            Memory.Map [(c << 4) + 0x700 + i] = MAP_LOROM_SRAM_OR_NONE;
            Memory.MapInfo[(c << 4) + 0x700 + i].Type = MAP_TYPE_RAM;
         }
      }
   }
   /* Banks 7e->7f, RAM */
   for (c = 0; c < 16; c++)
   {
      Memory.Map [c + 0x7e0] = Memory.RAM;
      Memory.Map [c + 0x7f0] = Memory.RAM + 0x10000;
      Memory.MapInfo[c + 0x7e0].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 0x7f0].Type = MAP_TYPE_RAM;
   }
   WriteProtectROM();
}

void MapExtraRAM(void)
{
   int32_t c;

   /* Banks 7e->7f, RAM */
   for (c = 0; c < 16; c++)
   {
      Memory.Map [c + 0x7e0] = Memory.RAM;
      Memory.Map [c + 0x7f0] = Memory.RAM + 0x10000;
      Memory.MapInfo[c + 0x7e0].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 0x7f0].Type = MAP_TYPE_RAM;
   }

   /* Banks 70->73, S-RAM */
   for (c = 0; c < 16; c++)
   {
      Memory.Map [c + 0x700] = Memory.SRAM;
      Memory.Map [c + 0x710] = Memory.SRAM + 0x8000;
      Memory.Map [c + 0x720] = Memory.SRAM + 0x10000;
      Memory.Map [c + 0x730] = Memory.SRAM + 0x18000;

      Memory.MapInfo[c + 0x700].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 0x710].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 0x720].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 0x730].Type = MAP_TYPE_RAM;
   }
}

void LoROMMap(void)
{
   int32_t c;
   int32_t i;

   /* Banks 00->3f and 80->bf */
   for (c = 0; c < 0x400; c += 16)
   {
      Memory.Map [c + 0] = Memory.Map [c + 0x800] = Memory.RAM;
      Memory.Map [c + 1] = Memory.Map [c + 0x801] = Memory.RAM;
      Memory.MapInfo[c + 0].Type = Memory.MapInfo[c + 0x800].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 1].Type = Memory.MapInfo[c + 0x801].Type = MAP_TYPE_RAM;

      Memory.Map [c + 2] = Memory.Map [c + 0x802] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 3] = Memory.Map [c + 0x803] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 4] = Memory.Map [c + 0x804] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 5] = Memory.Map [c + 0x805] = (uint8_t*) MAP_CPU;
      if (Settings.C4)
      {
         Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) MAP_C4;
         Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) MAP_C4;
      }
      else if(Settings.OBC1)
      {
         Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) MAP_OBC_RAM;
         Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) MAP_OBC_RAM;
      }
      else
      {
         Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) bytes0x2000 - 0x6000;
         Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) bytes0x2000 - 0x6000;
      }

      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = Memory.Map [i + 0x800] = &Memory.ROM [(c << 11) % Memory.CalculatedSize] - 0x8000;
         Memory.MapInfo[i].Type = Memory.MapInfo[i + 0x800].Type = MAP_TYPE_ROM;
      }
   }

   /* Banks 40->7f and c0->ff */
   for (c = 0; c < 0x400; c += 16)
   {
      for (i = c; i < c + 8; i++)
         Memory.Map [i + 0x400] = Memory.Map [i + 0xc00] = &Memory.ROM [(c << 11) % Memory.CalculatedSize];

      for (i = c + 8; i < c + 16; i++)
         Memory.Map [i + 0x400] = Memory.Map [i + 0xc00] = &Memory.ROM [((c << 11) + 0x200000) % Memory.CalculatedSize] - 0x8000;

      for (i = c; i < c + 16; i++)
         Memory.MapInfo[i + 0x400].Type = Memory.MapInfo[i + 0xC00].Type = MAP_TYPE_ROM;
   }

   if (Settings.DSP)
      DSPMap();

   MapRAM();
   WriteProtectROM();
}

/* MESHPUNK: SuperFX cart map. memmap.h declares SuperFXROMMap but this port
 * never defined it, so FX carts ran as plain LoROM with the chip inert.
 * Layout follows snes9x's Map_SuperFXLoROMMap. */
void SuperFXROMMap(void)
{
   int32_t c, i;
   uint32_t lim;

   /* The GSU sees the cart as linear 64KB banks while the CPU sees LoROM's
    * 32KB halves, so the image is replicated at ROM+2MB with each 32KB
    * block repeated TWICE per 64KB bank — the upper half is not padding,
    * and leaving it stale makes the chip fetch noise, never reach a STOP
    * and hold its GO flag forever. Bounded to the ROM allocation: snes9x
    * assumes a 6MB buffer and this port sizes to the cart (4.06MB for a
    * 2MB game). Banks past the limit are never fetched by carts that fit. */
   lim = Memory.ROM_AllocSize > 0x208000 ? Memory.ROM_AllocSize : 0x208000;
   for (c = 0; c < 64; c++)
   {
      if ((uint32_t)(0x208000 + c * 0x10000 + 0x8000) > lim)
         break;
      memcpy(&Memory.ROM[0x200000 + c * 0x10000], &Memory.ROM[c * 0x8000], 0x8000);
      memcpy(&Memory.ROM[0x208000 + c * 0x10000], &Memory.ROM[c * 0x8000], 0x8000);
   }

   /* Banks 00-3f and 80-bf: LoROM, with GSU RAM in the $6000-$7fff window */
   for (c = 0; c < 0x400; c += 16)
   {
      Memory.Map [c + 0] = Memory.Map [c + 0x800] = Memory.RAM;
      Memory.Map [c + 1] = Memory.Map [c + 0x801] = Memory.RAM;
      Memory.MapInfo[c + 0].Type = Memory.MapInfo[c + 0x800].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 1].Type = Memory.MapInfo[c + 0x801].Type = MAP_TYPE_RAM;

      Memory.Map [c + 2] = Memory.Map [c + 0x802] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 3] = Memory.Map [c + 0x803] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 4] = Memory.Map [c + 0x804] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 5] = Memory.Map [c + 0x805] = (uint8_t*) MAP_CPU;

      Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) Memory.SRAM - 0x6000;
      Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) Memory.SRAM - 0x6000;
      Memory.MapInfo[c + 6].Type = Memory.MapInfo[c + 0x806].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 7].Type = Memory.MapInfo[c + 0x807].Type = MAP_TYPE_RAM;

      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = Memory.Map [i + 0x800] =
            &Memory.ROM [(c << 11) % Memory.CalculatedSize] - 0x8000;
         Memory.MapInfo[i].Type = Memory.MapInfo[i + 0x800].Type = MAP_TYPE_ROM;
      }
   }

   /* Banks 40-7f and c0-ff: HiROM-style, the ROM seen linearly at 64KB */
   for (c = 0; c < 0x400; c += 16)
   {
      for (i = c; i < c + 16; i++)
      {
         Memory.Map [i + 0x400] = Memory.Map [i + 0xc00] =
            &Memory.ROM [(c << 12) % Memory.CalculatedSize];
         Memory.MapInfo[i + 0x400].Type = Memory.MapInfo[i + 0xc00].Type = MAP_TYPE_ROM;
      }
   }

   /* MapRAM re-maps banks 70-7d for LoROM carts, so the GSU RAM banks below
    * have to be installed after it. */
   MapRAM();

   /* Bank 70 is the first GSU RAM bank, 71 the second. */
   for (c = 0; c < 16; c++)
   {
      Memory.Map [c + 0x700] = Memory.SRAM;
      Memory.Map [c + 0x710] = Memory.SRAM + 0x10000;
      Memory.MapInfo[c + 0x700].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 0x710].Type = MAP_TYPE_RAM;
   }

   SuperFX.pvRegisters = &Memory.FillRAM[0x3000];
   SuperFX.nRamBanks   = 2; /* 128KB; SRAM_SIZE is sized to match */
   SuperFX.pvRam       = Memory.SRAM;
   SuperFX.nRomBanks   = (2 * 1024 * 1024) / (32 * 1024);
   SuperFX.pvRom       = Memory.ROM;
   SuperFX.vFlags      = 0;

   WriteProtectROM();
}

/* MESHPUNK: SA-1 cart map, following snes9x2005's SA1ROMMap adapted to this
 * port's conventions: write protection is MapInfo Type (not a WriteMap), so
 * SA1.WriteMap's ROM blocks are pointed at MAP_NONE from the Type info
 * below. The SA-1's own Map/WriteMap keep the donor's raw-pointer scheme —
 * sa1.c's accessors dispatch on them directly. */
void SA1ROMMap(void)
{
   int32_t c;
   int32_t i;

   /* Banks 00->3f and 80->bf */
   for (c = 0; c < 0x400; c += 16)
   {
      Memory.Map [c + 0] = Memory.Map [c + 0x800] = Memory.RAM;
      Memory.Map [c + 1] = Memory.Map [c + 0x801] = Memory.RAM;
      Memory.MapInfo[c + 0].Type = Memory.MapInfo[c + 0x800].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 1].Type = Memory.MapInfo[c + 0x801].Type = MAP_TYPE_RAM;

      Memory.Map [c + 2] = Memory.Map [c + 0x802] = (uint8_t*) MAP_PPU;
      /* I-RAM: the 4KB block at $3000 maps straight onto FillRAM so both
       * CPUs read and write it as memory ($3000-$37FF is the real 2KB). */
      Memory.Map [c + 3] = Memory.Map [c + 0x803] = (uint8_t*) &Memory.FillRAM [0x3000] - 0x3000;
      Memory.Map [c + 4] = Memory.Map [c + 0x804] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 5] = Memory.Map [c + 0x805] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) MAP_BWRAM;
      Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) MAP_BWRAM;
      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = Memory.Map [i + 0x800] =
            &Memory.ROM [(c << 11) % Memory.CalculatedSize] - 0x8000;
         Memory.MapInfo[i].Type = Memory.MapInfo[i + 0x800].Type = MAP_TYPE_ROM;
      }
   }

   /* Banks 40->7f: the BW-RAM image, seen linearly (writable: Type stays
    * I/O-default, which S9xSetByte does not protect). */
   for (c = 0; c < 0x400; c += 16)
      for (i = c; i < c + 16; i++)
         Memory.Map [i + 0x400] = (uint8_t*) &Memory.SRAM [(c << 12) & 0x1ffff];

   /* Banks c0->ff: the ROM seen linearly */
   for (c = 0; c < 0x400; c += 16)
   {
      for (i = c; i < c + 16; i++)
      {
         Memory.Map [i + 0xc00] = &Memory.ROM [(c << 12) % Memory.CalculatedSize];
         Memory.MapInfo[i + 0xc00].Type = MAP_TYPE_ROM;
      }
   }

   /* Banks 7e->7f: WRAM */
   for (c = 0; c < 16; c++)
   {
      Memory.Map [c + 0x7e0] = Memory.RAM;
      Memory.Map [c + 0x7f0] = Memory.RAM + 0x10000;
      Memory.MapInfo[c + 0x7e0].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 0x7f0].Type = MAP_TYPE_RAM;
   }
   WriteProtectROM();

   /* Now copy the map and correct it for the SA-1 CPU. */
   memcpy((void*) SA1.Map, (void*) Memory.Map, MEMMAP_NUM_BLOCKS * sizeof(uint8_t*));
   memcpy((void*) SA1.WriteMap, (void*) Memory.Map, MEMMAP_NUM_BLOCKS * sizeof(uint8_t*));

   /* SA-1 stores must never land in the ROM image. */
   for (c = 0; c < MEMMAP_NUM_BLOCKS; c++)
      if (Memory.MapInfo[c].Type == MAP_TYPE_ROM)
         SA1.WriteMap[c] = (uint8_t*) MAP_NONE;

   /* SA-1 side of banks 00->3f/80->bf: block 0 is I-RAM at $0000 (its zero
    * page), block 1 unmapped. */
   for (c = 0; c < 0x400; c += 16)
   {
      SA1.Map [c + 0] = SA1.Map [c + 0x800] = &Memory.FillRAM [0x3000];
      SA1.Map [c + 1] = SA1.Map [c + 0x801] = (uint8_t*) MAP_NONE;
      SA1.WriteMap [c + 0] = SA1.WriteMap [c + 0x800] = &Memory.FillRAM [0x3000];
      SA1.WriteMap [c + 1] = SA1.WriteMap [c + 0x801] = (uint8_t*) MAP_NONE;
   }

   /* Banks 60->6f: the virtual-bitmap view of BW-RAM (SA-1 only). */
   for (c = 0; c < 0x100; c++)
      SA1.Map [c + 0x600] = SA1.WriteMap [c + 0x600] = (uint8_t*) MAP_BWRAM_BITMAP;

   /* MESHPUNK: the SA-1 has no bus to WRAM — banks 7E/7F are open bus on
    * silicon (snes9x 1.62 removed this access as well). */
   for (c = 0; c < 32; c++)
      SA1.Map [c + 0x7e0] = SA1.WriteMap [c + 0x7e0] = (uint8_t*) MAP_NONE;

   Memory.BWRAM = Memory.SRAM;
}

void DSPMap(void)
{
   switch (Settings.DSP)
   {
      case 1:
         if (Memory.HiROM)
         {
            map_index(0x00, 0x1f, 0x6000, 0x7fff, MAP_DSP, MAP_TYPE_I_O);
            map_index(0x80, 0x9f, 0x6000, 0x7fff, MAP_DSP, MAP_TYPE_I_O);
            break;
         }
         else if (Memory.CalculatedSize > 0x100000)
         {
            map_index(0x60, 0x6f, 0x0000, 0x7fff, MAP_DSP, MAP_TYPE_I_O);
            map_index(0xe0, 0xef, 0x0000, 0x7fff, MAP_DSP, MAP_TYPE_I_O);
            break;
         }
         else
         {
            map_index(0x20, 0x3f, 0x8000, 0xffff, MAP_DSP, MAP_TYPE_I_O);
            map_index(0xa0, 0xbf, 0x8000, 0xffff, MAP_DSP, MAP_TYPE_I_O);
            break;
         }
      case 2:
         map_index(0x20, 0x3f, 0x6000, 0x6fff, MAP_DSP, MAP_TYPE_I_O);
         map_index(0x20, 0x3f, 0x8000, 0xbfff, MAP_DSP, MAP_TYPE_I_O);
         map_index(0xa0, 0xbf, 0x6000, 0x6fff, MAP_DSP, MAP_TYPE_I_O);
         map_index(0xa0, 0xbf, 0x8000, 0xbfff, MAP_DSP, MAP_TYPE_I_O);
         break;
      case 3:
         map_index(0x20, 0x3f, 0x8000, 0xffff, MAP_DSP, MAP_TYPE_I_O);
         map_index(0xa0, 0xbf, 0x8000, 0xffff, MAP_DSP, MAP_TYPE_I_O);
         break;
      case 4:
         map_index(0x30, 0x3f, 0x8000, 0xffff, MAP_DSP, MAP_TYPE_I_O);
         map_index(0xb0, 0xbf, 0x8000, 0xffff, MAP_DSP, MAP_TYPE_I_O);
         break;
   }
}

void HiROMMap(void)
{
   int32_t i;
   int32_t c;

   /* Banks 00->3f and 80->bf */
   for (c = 0; c < 0x400; c += 16)
   {
      Memory.Map [c + 0] = Memory.Map [c + 0x800] = Memory.RAM;
      Memory.MapInfo[c + 0].Type = Memory.MapInfo[c + 0x800].Type = MAP_TYPE_RAM;
      Memory.Map [c + 1] = Memory.Map [c + 0x801] = Memory.RAM;
      Memory.MapInfo[c + 1].Type = Memory.MapInfo[c + 0x801].Type = MAP_TYPE_RAM;

      Memory.Map [c + 2] = Memory.Map [c + 0x802] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 3] = Memory.Map [c + 0x803] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 4] = Memory.Map [c + 0x804] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 5] = Memory.Map [c + 0x805] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) MAP_NONE;
      Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) MAP_NONE;

      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = Memory.Map [i + 0x800] = &Memory.ROM [(c << 12) % Memory.CalculatedSize];
         Memory.MapInfo[i].Type = Memory.MapInfo[i + 0x800].Type = MAP_TYPE_ROM;
      }
   }

   /* Banks 30->3f and b0->bf, address ranges 6000->7fff is S-RAM. */
   for (c = 0; c < 16; c++)
   {
      Memory.Map [0x306 + (c << 4)] = MAP_HIROM_SRAM_OR_NONE;
      Memory.Map [0x307 + (c << 4)] = MAP_HIROM_SRAM_OR_NONE;
      Memory.Map [0xb06 + (c << 4)] = MAP_HIROM_SRAM_OR_NONE;
      Memory.Map [0xb07 + (c << 4)] = MAP_HIROM_SRAM_OR_NONE;
      Memory.MapInfo[0x306 + (c << 4)].Type = MAP_TYPE_RAM;
      Memory.MapInfo[0x307 + (c << 4)].Type = MAP_TYPE_RAM;
      Memory.MapInfo[0xb06 + (c << 4)].Type = MAP_TYPE_RAM;
      Memory.MapInfo[0xb07 + (c << 4)].Type = MAP_TYPE_RAM;
   }

   /* Banks 40->7f and c0->ff */
   for (c = 0; c < 0x400; c += 16)
   {
      for (i = c; i < c + 16; i++)
      {
         Memory.Map [i + 0x400] = Memory.Map [i + 0xc00] = &Memory.ROM [(c << 12) % Memory.CalculatedSize];
         Memory.MapInfo[i + 0x400].Type = Memory.MapInfo[i + 0xC00].Type = MAP_TYPE_ROM;
      }
   }

   if (Settings.DSP)
      DSPMap();

   MapRAM();
   WriteProtectROM();
}

void TalesROMMap(bool Interleaved)
{
   int32_t c;
   int32_t i;
   int32_t sum = 0;

   uint32_t OFFSET0 = 0x400000;
   uint32_t OFFSET1 = 0x400000;
   uint32_t OFFSET2 = 0x000000;

   if (Interleaved)
   {
      OFFSET0 = 0x000000;
      OFFSET1 = 0x000000;
      OFFSET2 = Memory.CalculatedSize - 0x400000; /* changed to work with interleaved DKJM2. */
   }

   /* Banks 00->3f and 80->bf */
   for (c = 0; c < 0x400; c += 16)
   {
      Memory.Map [c + 0] = Memory.Map [c + 0x800] = Memory.RAM;
      Memory.Map [c + 1] = Memory.Map [c + 0x801] = Memory.RAM;
      Memory.MapInfo[c + 0].Type = Memory.MapInfo[c + 0x800].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 1].Type = Memory.MapInfo[c + 0x801].Type = MAP_TYPE_RAM;

      Memory.Map [c + 2] = Memory.Map [c + 0x802] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 3] = Memory.Map [c + 0x803] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 4] = Memory.Map [c + 0x804] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 5] = Memory.Map [c + 0x805] = (uint8_t*) MAP_CPU;

      /* makes more sense to map the range here. */
      /* ToP seems to use sram to skip intro??? */
      if (c >= 0x200)
      {
         Memory.Map [c + 6] = Memory.Map [c + 0x806] = MAP_HIROM_SRAM_OR_NONE;
         Memory.Map [c + 7] = Memory.Map [c + 0x807] = MAP_HIROM_SRAM_OR_NONE;
         Memory.MapInfo[6 + c].Type = Memory.MapInfo[7 + c].Type = Memory.MapInfo[0x806 + c].Type = Memory.MapInfo[0x807 + c].Type = MAP_TYPE_RAM;
      }
      else
      {
         Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) MAP_NONE;
         Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) MAP_NONE;
      }
      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = &Memory.ROM [((c << 12) % (Memory.CalculatedSize - 0x400000)) + OFFSET0];
         Memory.Map [i + 0x800] = &Memory.ROM [((c << 12) % 0x400000) + OFFSET2];
         Memory.MapInfo[i].Type = MAP_TYPE_ROM;
         Memory.MapInfo[i + 0x800].Type = MAP_TYPE_ROM;
      }
   }

   /* Banks 40->7f and c0->ff */
   for (c = 0; c < 0x400; c += 16)
   {
      for (i = c; i < c + 8; i++)
      {
         Memory.Map [i + 0x400] = &Memory.ROM [((c << 12) % (Memory.CalculatedSize - 0x400000)) + OFFSET1];
         Memory.Map [i + 0x408] = &Memory.ROM [((c << 12) % (Memory.CalculatedSize - 0x400000)) + OFFSET1];
         Memory.Map [i + 0xc00] = &Memory.ROM [((c << 12) % 0x400000) + OFFSET2];
         Memory.Map [i + 0xc08] = &Memory.ROM [((c << 12) % 0x400000) + OFFSET2];
         Memory.MapInfo[i + 0x400].Type = MAP_TYPE_ROM;
         Memory.MapInfo[i + 0x408].Type = MAP_TYPE_ROM;
         Memory.MapInfo[i + 0xC00].Type = MAP_TYPE_ROM;
         Memory.MapInfo[i + 0xC08].Type = MAP_TYPE_ROM;
      }
   }

   Memory.ROMChecksum = *(Memory.Map[8] + 0xFFDE) + (*(Memory.Map[8] + 0xFFDF) << 8);
   Memory.ROMComplementChecksum = *(Memory.Map[8] + 0xFFDC) + (*(Memory.Map[8] + 0xFFDD) << 8);

   for (i = 0x40; i < 0x80; i++)
   {
      uint8_t* bank_low = (uint8_t*)Memory.Map[i << 4];
      uint8_t* bank_high = (uint8_t*)Memory.Map[(i << 4) + 0x800];
      for (c = 0; c < 0x10000; c++)
      {
         sum += bank_low[c];
         sum += bank_high[c];
      }
   }

   Memory.CalculatedChecksum = sum & 0xFFFF;
   MapRAM();
   WriteProtectROM();
}

void AlphaROMMap(void)
{
   int32_t c;
   int32_t i;

   /* Banks 00->3f and 80->bf */
   for (c = 0; c < 0x400; c += 16)
   {
      Memory.Map [c + 0] = Memory.Map [c + 0x800] = Memory.RAM;
      Memory.Map [c + 1] = Memory.Map [c + 0x801] = Memory.RAM;
      Memory.MapInfo[c + 0].Type = Memory.MapInfo[c + 0x800].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 1].Type = Memory.MapInfo[c + 0x801].Type = MAP_TYPE_RAM;

      Memory.Map [c + 2] = Memory.Map [c + 0x802] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 3] = Memory.Map [c + 0x803] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 4] = Memory.Map [c + 0x804] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 5] = Memory.Map [c + 0x805] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) MAP_NONE;
      Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) MAP_NONE;

      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = Memory.Map [i + 0x800] = &Memory.ROM [(c << 11) % Memory.CalculatedSize] - 0x8000;
         Memory.MapInfo[i].Type = MAP_TYPE_ROM;
      }
   }

   /* Banks 40->7f and c0->ff */

   for (c = 0; c < 0x400; c += 16)
   {
      for (i = c; i < c + 16; i++)
      {
         Memory.Map [i + 0x400] = &Memory.ROM [(c << 12) % Memory.CalculatedSize];
         Memory.Map [i + 0xc00] = &Memory.ROM [(c << 12) % Memory.CalculatedSize];
         Memory.MapInfo[i + 0x400].Type = Memory.MapInfo[i + 0xC00].Type = MAP_TYPE_ROM;
      }
   }

   MapRAM();
   WriteProtectROM();
}

void LoROM24MBSMap(void)
{
   int32_t c;
   int32_t i;

   /* Banks 00->3f and 80->bf */
   for (c = 0; c < 0x400; c += 16)
   {
      Memory.Map [c + 0] = Memory.Map [c + 0x800] = Memory.RAM;
      Memory.Map [c + 1] = Memory.Map [c + 0x801] = Memory.RAM;
      Memory.MapInfo[c + 0].Type = Memory.MapInfo[c + 0x800].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 1].Type = Memory.MapInfo[c + 0x801].Type = MAP_TYPE_RAM;

      Memory.Map [c + 2] = Memory.Map [c + 0x802] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 3] = Memory.Map [c + 0x803] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 4] = Memory.Map [c + 0x804] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 5] = Memory.Map [c + 0x805] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) MAP_NONE;
      Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) MAP_NONE;

      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = Memory.Map [i + 0x800] = &Memory.ROM [(c << 11) % Memory.CalculatedSize] - 0x8000;
         Memory.MapInfo[i].Type = Memory.MapInfo[i + 0x800].Type = MAP_TYPE_ROM;
      }
   }

   /* Banks 00->3f and 80->bf */
   for (c = 0; c < 0x200; c += 16)
   {
      Memory.Map [c + 0x800] = Memory.RAM;
      Memory.Map [c + 0x801] = Memory.RAM;
      Memory.MapInfo[c + 0x800].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 0x801].Type = MAP_TYPE_RAM;

      Memory.Map [c + 0x802] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 0x803] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 0x804] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 0x805] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 0x806] = (uint8_t*) MAP_NONE;
      Memory.Map [c + 0x807] = (uint8_t*) MAP_NONE;

      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i + 0x800] = &Memory.ROM [((c << 11) + 0x200000) % Memory.CalculatedSize] - 0x8000;
         Memory.MapInfo[i + 0x800].Type = MAP_TYPE_ROM;
      }
   }

   /* Banks 40->7f and c0->ff */
   for (c = 0; c < 0x400; c += 16)
   {
      for (i = c; i < c + 8; i++)
         Memory.Map [i + 0x400] = Memory.Map [i + 0xc00] = &Memory.ROM [((c << 11) + 0x200000) % Memory.CalculatedSize];

      for (i = c + 8; i < c + 16; i++)
         Memory.Map [i + 0x400] = Memory.Map [i + 0xc00] = &Memory.ROM [((c << 11) + 0x200000) % Memory.CalculatedSize] - 0x8000;

      for (i = c; i < c + 16; i++)
         Memory.MapInfo[i + 0x400].Type = Memory.MapInfo[i + 0xC00].Type = MAP_TYPE_ROM;
   }

   MapExtraRAM();
   WriteProtectROM();
}

void SRAM512KLoROMMap(void)
{
   int32_t c;
   int32_t i;

   /* Banks 00->3f and 80->bf */
   for (c = 0; c < 0x400; c += 16)
   {
      Memory.Map [c + 0] = Memory.Map [c + 0x800] = Memory.RAM;
      Memory.Map [c + 1] = Memory.Map [c + 0x801] = Memory.RAM;
      Memory.MapInfo[c + 0].Type = Memory.MapInfo[c + 0x800].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 1].Type = Memory.MapInfo[c + 0x801].Type = MAP_TYPE_RAM;

      Memory.Map [c + 2] = Memory.Map [c + 0x802] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 3] = Memory.Map [c + 0x803] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 4] = Memory.Map [c + 0x804] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 5] = Memory.Map [c + 0x805] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) MAP_NONE;
      Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) MAP_NONE;
      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = Memory.Map [i + 0x800] = &Memory.ROM [(c << 11) % Memory.CalculatedSize] - 0x8000;
         Memory.MapInfo[i].Type = Memory.MapInfo[i + 0x800].Type = MAP_TYPE_ROM;
      }
   }

   /* Banks 40->7f and c0->ff */
   for (c = 0; c < 0x400; c += 16)
   {
      for (i = c; i < c + 8; i++)
         Memory.Map [i + 0x400] = Memory.Map [i + 0xc00] = &Memory.ROM [((c << 11) + 0x200000) % Memory.CalculatedSize];

      for (i = c + 8; i < c + 16; i++)
         Memory.Map [i + 0x400] = Memory.Map [i + 0xc00] = &Memory.ROM [((c << 11) + 0x200000) % Memory.CalculatedSize] - 0x8000;

      for (i = c; i < c + 16; i++)
         Memory.MapInfo[i + 0x400].Type = Memory.MapInfo[i + 0xC00].Type = MAP_TYPE_ROM;
   }

   MapExtraRAM();
   WriteProtectROM();
}

void SRAM1024KLoROMMap(void)
{
   int32_t c;
   int32_t i;

   /* Banks 00->3f and 80->bf */
   for (c = 0; c < 0x400; c += 16)
   {
      Memory.Map [c + 0] = Memory.Map [c + 0x800] = Memory.RAM;
      Memory.Map [c + 1] = Memory.Map [c + 0x801] = Memory.RAM;
      Memory.MapInfo[c + 0].Type = Memory.MapInfo[c + 0x800].Type = Memory.MapInfo[c + 0x400].Type = Memory.MapInfo[c + 0xc00].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 1].Type = Memory.MapInfo[c + 0x801].Type = Memory.MapInfo[c + 0x401].Type = Memory.MapInfo[c + 0xc01].Type = MAP_TYPE_RAM;

      Memory.Map [c + 2] = Memory.Map [c + 0x802] = Memory.Map [c + 0x402] = Memory.Map [c + 0xc02] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 3] = Memory.Map [c + 0x803] = Memory.Map [c + 0x403] = Memory.Map [c + 0xc03] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 4] = Memory.Map [c + 0x804] = Memory.Map [c + 0x404] = Memory.Map [c + 0xc04] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 5] = Memory.Map [c + 0x805] = Memory.Map [c + 0x405] = Memory.Map [c + 0xc05] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 6] = Memory.Map [c + 0x806] = Memory.Map [c + 0x406] = Memory.Map [c + 0xc06] = (uint8_t*) MAP_NONE;
      Memory.Map [c + 7] = Memory.Map [c + 0x807] = Memory.Map [c + 0x407] = Memory.Map [c + 0xc07] = (uint8_t*) MAP_NONE;
      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = Memory.Map [i + 0x800] = Memory.Map [i + 0x400] = Memory.Map [i + 0xc00] = &Memory.ROM [(c << 11) % Memory.CalculatedSize] - 0x8000;
         Memory.MapInfo[i].Type = Memory.MapInfo[i + 0x800].Type = Memory.MapInfo[i + 0x400].Type = Memory.MapInfo[i + 0xC00].Type = MAP_TYPE_ROM;
      }
   }

   MapExtraRAM();
   WriteProtectROM();
}

void CapcomProtectLoROMMap(void)
{
   int32_t c;
   int32_t i;

   /* Banks 00->3f and 80->bf */
   for (c = 0; c < 0x400; c += 16)
   {
      Memory.Map [c + 0] = Memory.Map [c + 0x800] = Memory.Map [c + 0x400] = Memory.Map [c + 0xc00] = Memory.RAM;
      Memory.Map [c + 1] = Memory.Map [c + 0x801] = Memory.Map [c + 0x401] = Memory.Map [c + 0xc01] = Memory.RAM;
      Memory.MapInfo[c + 0].Type = Memory.MapInfo[c + 0x800].Type = Memory.MapInfo[c + 0x400].Type = Memory.MapInfo[c + 0xc00].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 1].Type = Memory.MapInfo[c + 0x801].Type = Memory.MapInfo[c + 0x401].Type = Memory.MapInfo[c + 0xc01].Type = MAP_TYPE_RAM;

      Memory.Map [c + 2] = Memory.Map [c + 0x802] = Memory.Map [c + 0x402] = Memory.Map [c + 0xc02] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 3] = Memory.Map [c + 0x803] = Memory.Map [c + 0x403] = Memory.Map [c + 0xc03] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 4] = Memory.Map [c + 0x804] = Memory.Map [c + 0x404] = Memory.Map [c + 0xc04] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 5] = Memory.Map [c + 0x805] = Memory.Map [c + 0x405] = Memory.Map [c + 0xc05] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 6] = Memory.Map [c + 0x806] = Memory.Map [c + 0x406] = Memory.Map [c + 0xc06] = (uint8_t*) MAP_NONE;
      Memory.Map [c + 7] = Memory.Map [c + 0x807] = Memory.Map [c + 0x407] = Memory.Map [c + 0xc07] = (uint8_t*) MAP_NONE;
      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = Memory.Map [i + 0x800] = Memory.Map [i + 0x400] = Memory.Map [i + 0xc00] = &Memory.ROM [(c << 11) % Memory.CalculatedSize] - 0x8000;
         Memory.MapInfo[i].Type = Memory.MapInfo[i + 0x800].Type = Memory.MapInfo[i + 0x400].Type = Memory.MapInfo[i + 0xC00].Type = MAP_TYPE_ROM;
      }
   }

   MapRAM();
   WriteProtectROM();
}

void JumboLoROMMap(bool Interleaved)
{
   int32_t c;
   int32_t i;
   int32_t sum = 0, k, l;

   uint32_t OFFSET0 = 0x400000;
   uint32_t OFFSET2 = 0x000000;

   if (Interleaved)
   {
      OFFSET0 = 0x000000;
      OFFSET2 = Memory.CalculatedSize - 0x400000; /* changed to work with interleaved DKJM2. */
   }
   /* Banks 00->3f and 80->bf */
   for (c = 0; c < 0x400; c += 16)
   {
      Memory.Map [c + 0] = Memory.Map [c + 0x800] = Memory.RAM;
      Memory.Map [c + 1] = Memory.Map [c + 0x801] = Memory.RAM;
      Memory.MapInfo[c + 0].Type = Memory.MapInfo[c + 0x800].Type = MAP_TYPE_RAM;
      Memory.MapInfo[c + 1].Type = Memory.MapInfo[c + 0x801].Type = MAP_TYPE_RAM;

      Memory.Map [c + 2] = Memory.Map [c + 0x802] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 3] = Memory.Map [c + 0x803] = (uint8_t*) MAP_PPU;
      Memory.Map [c + 4] = Memory.Map [c + 0x804] = (uint8_t*) MAP_CPU;
      Memory.Map [c + 5] = Memory.Map [c + 0x805] = (uint8_t*) MAP_CPU;
      if (Settings.DSP1Master)
      {
         Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) MAP_DSP;
         Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) MAP_DSP;
      }
      else if (Settings.C4)
      {
         Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) MAP_C4;
         Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) MAP_C4;
      }
      else
      {
         Memory.Map [c + 6] = Memory.Map [c + 0x806] = (uint8_t*) bytes0x2000 - 0x6000;
         Memory.Map [c + 7] = Memory.Map [c + 0x807] = (uint8_t*) bytes0x2000 - 0x6000;
      }

      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = &Memory.ROM [((c << 11) % (Memory.CalculatedSize - 0x400000)) + OFFSET0] - 0x8000;
         Memory.Map [i + 0x800] = &Memory.ROM [((c << 11) % (0x400000)) + OFFSET2] - 0x8000;
         Memory.MapInfo[i + 0x800].Type = Memory.MapInfo[i].Type = MAP_TYPE_ROM;
      }
   }

   if (Settings.DSP1Master)
   {
      /* Banks 30->3f and b0->bf */
      for (c = 0x300; c < 0x400; c += 16)
      {
         for (i = c + 8; i < c + 16; i++)
         {
            Memory.Map [i + 0x800] = (uint8_t*) MAP_DSP;
            Memory.MapInfo[i].Type = Memory.MapInfo[i + 0x800].Type = MAP_TYPE_ROM;
         }
      }
   }

   /* Banks 40->7f and c0->ff */
   for (c = 0x400; c < 0x800; c += 16)
   {
      /* updated mappings to correct A15 mirroring */
      for (i = c; i < c + 8; i++)
      {
         Memory.Map [i] = &Memory.ROM [((c << 11) % (Memory.CalculatedSize - 0x400000)) + OFFSET0];
         Memory.Map [i + 0x800] = &Memory.ROM [((c << 11) % 0x400000) + OFFSET2];
      }

      for (i = c + 8; i < c + 16; i++)
      {
         Memory.Map [i] = &Memory.ROM [((c << 11) % (Memory.CalculatedSize - 0x400000)) + OFFSET0] - 0x8000;
         Memory.Map [i + 0x800] = &Memory.ROM [((c << 11) % 0x400000) + OFFSET2 ] - 0x8000;
      }

      for (i = c; i < c + 16; i++)
         Memory.MapInfo[i].Type = Memory.MapInfo[i + 0x800].Type = MAP_TYPE_ROM;
   }

   /* ROM type has to be 64 Mbit header! */
   for (k = 0; k < 256; k++)
   {
      uint8_t* bank = 0x8000 + Memory.Map[8 + (k << 4)]; /* use upper half of the banks, and adjust for LoROM. */
      for (l = 0; l < 0x8000; l++)
         sum += bank[l];
   }
   Memory.CalculatedChecksum = sum & 0xFFFF;
   MapRAM();
   WriteProtectROM();
}

const char* TVStandard(void)
{
   return Settings.PAL ? "PAL" : "NTSC";
}

const char* Speed(void)
{
   return Memory.ROMSpeed & 0x10 ? "120ns" : "200ns";
}

const char* MapType(void)
{
   return Memory.HiROM ? "HiROM" : "LoROM";
}

const char* ROMID(void)
{
   return Memory.ROMId;
}

bool match_na(const char* str)
{
   return strcmp(Memory.ROMName, str) == 0;
}

bool match_id(const char* str)
{
   return strncmp(Memory.ROMId, str, strlen(str)) == 0;
}

void ApplyROMPatches(void)
{
   /* Mario Early Years: Fun with Numbers */
   if ((strncmp ((char *) &Memory.ROM [0x7fc0], "MEY Fun with Numbers", 20) == 0))
   {
      int32_t i;
      for (i = 0x87fc0; i < 0x87fe0; i++)
         Memory.ROM [i] = 0;
   }
   else if(Memory.CalculatedSize == 0x100000 && strncmp ((char *) &Memory.ROM [0xffc0], "WWF SUPER WRESTLEMANIA", 22) == 0)
   {
      int32_t cvcount;
      memcpy(&Memory.ROM[0x100000], Memory.ROM, 0x100000);
      for(cvcount = 0; cvcount < 16; cvcount++)
      {
         memcpy(&Memory.ROM[0x8000 * cvcount], &Memory.ROM[0x10000 * cvcount + 0x100000 + 0x8000], 0x8000);
         memcpy(&Memory.ROM[0x8000 * cvcount + 0x80000], &Memory.ROM[0x10000 * cvcount + 0x100000], 0x8000);
      }
   }
   // CheckForIPSPatch(filename, Memory.HeaderCount != 0, &TotalFileSize);
   /* fix hacked games here. */
   if ((strncmp("HONKAKUHA IGO GOSEI", (char*)&Memory.ROM[0x7FC0], 19) == 0) && (Memory.ROM[0x7FD5] != 0x31))
   {
      Memory.ROM[0x7FD5] = 0x31;
      Memory.ROM[0x7FD6] = 0x02;
   }
#ifndef NO_SPEEDHACKS
   /* SNESAdvance speed hacks (from the speed-hacks branch of CatSFC) */
   if (strncmp("YOSHI'S ISLAND", (char *) &Memory.ROM[0x7FC0], 14) == 0)
   {
      Memory.ROM[0x0000F4] = 0x42;
      Memory.ROM[0x0000F5] = 0x3B;
   }
   else if (strncmp("SUPER MARIOWORLD", (char *) &Memory.ROM[0x7FC0], 16) == 0)
   {
      Memory.ROM[0x00006D] = 0x42;
   }
   else if (strncmp("ALL_STARS + WORLD", (char *) &Memory.ROM[0x7FC0], 17) == 0)
   {
      Memory.ROM[0x0003D0] = 0x42;
      Memory.ROM[0x0003D1] = 0x5B;
      Memory.ROM[0x018522] = 0x42;
      Memory.ROM[0x018523] = 0x5B;
      Memory.ROM[0x02C804] = 0x42;
      Memory.ROM[0x02C805] = 0xBA;
      Memory.ROM[0x0683B5] = 0x42;
      Memory.ROM[0x0683B6] = 0x5B;
      Memory.ROM[0x0696AC] = 0x42;
      Memory.ROM[0x0696AD] = 0xBA;
      Memory.ROM[0x089233] = 0xDB;
      Memory.ROM[0x089234] = 0x61;
      Memory.ROM[0x0895DF] = 0x42;
      Memory.ROM[0x0895E0] = 0x5B;
      Memory.ROM[0x0A7A9D] = 0x42;
      Memory.ROM[0x0A7A9E] = 0xBA;
      Memory.ROM[0x1072E7] = 0x42;
      Memory.ROM[0x1072E8] = 0xD9;
      Memory.ROM[0x107355] = 0x42;
      Memory.ROM[0x107356] = 0x5B;
      Memory.ROM[0x1073CF] = 0x42;
      Memory.ROM[0x1073D0] = 0x5B;
      Memory.ROM[0x107443] = 0x42;
      Memory.ROM[0x107444] = 0x5B;
      Memory.ROM[0x107498] = 0x42;
      Memory.ROM[0x107499] = 0x5B;
      Memory.ROM[0x107505] = 0x42;
      Memory.ROM[0x107506] = 0x5B;
      Memory.ROM[0x107539] = 0x42;
      Memory.ROM[0x10753A] = 0x5B;
      Memory.ROM[0x107563] = 0x42;
      Memory.ROM[0x107564] = 0x5B;
      Memory.ROM[0x18041D] = 0x42;
      Memory.ROM[0x18041E] = 0x79;
   }
#endif
}

void ApplyROMFixes(void)
{
   /*
   HACKS NSRT can fix that we hadn't detected before.
   [14:25:13] <@Nach>     case 0x0c572ef0: So called Hook (US)(2648)
   [14:25:13] <@Nach>     case 0x6810aa95: Bazooka Blitzkreig swapped sizes hack -handled
   [14:25:17] <@Nach>     case 0x61E29C06: The Tick region hack
   [14:25:19] <@Nach>     case 0x1EF90F74: Jikkyou Keiba Simulation Stable Star PAL hack
   [14:25:23] <@Nach>     case 0x4ab225b5: So called Krusty's Super Fun House (E)
   [14:25:25] <@Nach>     case 0x77fd806a: Donkey Kong Country 2 (E) v1.1 bad dump -handled
   [14:25:27] <@Nach>     case 0x340f23e5: Donkey Kong Country 3 (U) copier hack - handled
   */

   /* MESHPUNK: SA-1 speedup settings from snes9x2005 — per-game addresses of
    * the SA-1's idle poll loop (WaitAddress: the SA-1 core's CPUShutdown
    * parks the chip there) and of the mailbox bytes whose S-CPU writes wake
    * it (WaitByteAddress1/2, compared in getset.c's store fast path). */
   SA1.WaitAddress = NULL;
   SA1.WaitByteAddress1 = NULL;
   SA1.WaitByteAddress2 = NULL;

   if (Settings.SA1)
   {
      /* Itoi Shigesato no Bass Tsuri No.1 (J) */
      if (match_id("ZBPJ"))
      {
         SA1.WaitAddress = SA1.Map [0x0093f1 >> MEMMAP_SHIFT] + 0x93f1;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x304a;
      }
      /* Daisenryaku Expert WWII (J) */
      else if (match_id("AEVJ"))
      {
         SA1.WaitAddress = SA1.Map [0x0ed18d >> MEMMAP_SHIFT] + 0xd18d;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x3000;
      }
      /* Derby Jockey 2 (J) */
      else if (match_id("A2DJ"))
         SA1.WaitAddress = SA1.Map [0x008b62 >> MEMMAP_SHIFT] + 0x8b62;
      /* Dragon Ball Z - Hyper Dimension (J) */
      else if (match_id("AZIJ"))
      {
         SA1.WaitAddress = SA1.Map [0x008083 >> MEMMAP_SHIFT] + 0x8083;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x3020;
      }
      /* SD Gundam G NEXT (J) */
      else if (match_id("ZX3J"))
      {
         SA1.WaitAddress = SA1.Map [0x0087f2 >> MEMMAP_SHIFT] + 0x87f2;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x30c4;
      }
      /* Shougi no Hanamichi (J) */
      else if (match_id("AARJ"))
      {
         SA1.WaitAddress = SA1.Map [0xc1f85a >> MEMMAP_SHIFT] + 0xf85a;
         SA1.WaitByteAddress1 = Memory.SRAM + 0x0c64;
         SA1.WaitByteAddress2 = Memory.SRAM + 0x0c66;
      }
      /* Asahi Shinbun Rensai Katou Hifumi Kudan Shougi Shingiryu (J) */
      if (match_id("A23J"))
      {
         SA1.WaitAddress = SA1.Map [0xc25037 >> MEMMAP_SHIFT] + 0x5037;
         SA1.WaitByteAddress1 = Memory.SRAM + 0x0c06;
         SA1.WaitByteAddress2 = Memory.SRAM + 0x0c08;
      }
      /* Taikyoku Igo - Idaten (J) */
      else if (match_id("AIIJ"))
      {
         SA1.WaitAddress = SA1.Map [0xc100be >> MEMMAP_SHIFT] + 0x00be;
         SA1.WaitByteAddress1 = Memory.SRAM + 0x1002;
         SA1.WaitByteAddress2 = Memory.SRAM + 0x1004;
      }
      /* Takemiya Masaki Kudan no Igo Taishou (J) */
      else if (match_id("AITJ"))
         SA1.WaitAddress = SA1.Map [0x0080b7 >> MEMMAP_SHIFT] + 0x80b7;
      /* J. League '96 Dream Stadium (J) */
      else if (match_id("AJ6J"))
         SA1.WaitAddress = SA1.Map [0xc0f74a >> MEMMAP_SHIFT] + 0xf74a;
      /* Jumpin' Derby (J) */
      else if (match_id("AJUJ"))
         SA1.WaitAddress = SA1.Map [0x00d926 >> MEMMAP_SHIFT] + 0xd926;
      /* Kakinoki Shougi (J) */
      else if (match_id("AKAJ"))
         SA1.WaitAddress = SA1.Map [0x00f070 >> MEMMAP_SHIFT] + 0xf070;
      /* Hoshi no Kirby 3 (J), Kirby's Dream Land 3 (U) */
      else if (match_id("AFJJ") || match_id("AFJE"))
      {
         SA1.WaitAddress = SA1.Map [0x0082d4 >> MEMMAP_SHIFT] + 0x82d4;
         SA1.WaitByteAddress1 = Memory.SRAM + 0x72a4;
      }
      /* Hoshi no Kirby - Super Deluxe (J) */
      else if (match_id("AKFJ"))
      {
         SA1.WaitAddress = SA1.Map [0x008c93 >> MEMMAP_SHIFT] + 0x8c93;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x300a;
         SA1.WaitByteAddress2 = Memory.FillRAM + 0x300e;
      }
      /* Kirby Super Star (U) */
      else if (match_id("AKFE"))
      {
         SA1.WaitAddress = SA1.Map [0x008cb8 >> MEMMAP_SHIFT] + 0x8cb8;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x300a;
         SA1.WaitByteAddress2 = Memory.FillRAM + 0x300e;
      }
      /* Super Mario RPG (J), (U) */
      else if (match_id("ARWJ") || match_id("ARWE"))
      {
         SA1.WaitAddress = SA1.Map [0xc0816f >> MEMMAP_SHIFT] + 0x816f;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x3000;
      }
      /* Marvelous (J) */
      else if (match_id("AVRJ"))
      {
         SA1.WaitAddress = SA1.Map [0x0085f2 >> MEMMAP_SHIFT] + 0x85f2;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x3024;
      }
      /* Harukanaru Augusta 3 - Masters New (J) */
      else if (match_id("AO3J"))
      {
         SA1.WaitAddress = SA1.Map [0x00dddb >> MEMMAP_SHIFT] + 0xdddb;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x37b4;
      }
      /* Jikkyou Oshaberi Parodius (J) */
      else if (match_id("AJOJ"))
         SA1.WaitAddress = SA1.Map [0x8084e5 >> MEMMAP_SHIFT] + 0x84e5;
      /* Super Bomberman - Panic Bomber W (J) */
      else if (match_id("APBJ"))
         SA1.WaitAddress = SA1.Map [0x00857a >> MEMMAP_SHIFT] + 0x857a;
      /* Pebble Beach no Hatou New - Tournament Edition (J) */
      else if (match_id("AONJ"))
      {
         SA1.WaitAddress = SA1.Map [0x00df33 >> MEMMAP_SHIFT] + 0xdf33;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x37b4;
      }
      /* PGA European Tour (U) */
      else if (match_id("AEPE"))
      {
         SA1.WaitAddress = SA1.Map [0x003700 >> MEMMAP_SHIFT] + 0x3700;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x3102;
      }
      /* PGA Tour 96 (U) */
      else if (match_id("A3GE"))
      {
         SA1.WaitAddress = SA1.Map [0x003700 >> MEMMAP_SHIFT] + 0x3700;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x3102;
      }
      /* Power Rangers Zeo - Battle Racers (U) */
      else if (match_id("A4RE"))
      {
         SA1.WaitAddress = SA1.Map [0x009899 >> MEMMAP_SHIFT] + 0x9899;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x3000;
      }
      /* SD F-1 Grand Prix (J) */
      else if (match_id("AGFJ"))
         SA1.WaitAddress = SA1.Map [0x0181bc >> MEMMAP_SHIFT] + 0x81bc;
      /* Saikousoku Shikou Shougi Mahjong (J) */
      else if (match_id("ASYJ"))
      {
         SA1.WaitAddress = SA1.Map [0x00f2cc >> MEMMAP_SHIFT] + 0xf2cc;
         SA1.WaitByteAddress1 = Memory.SRAM + 0x7ffe;
         SA1.WaitByteAddress2 = Memory.SRAM + 0x7ffc;
      }
      /* Shougi Saikyou II (J) */
      else if (match_id("AX2J"))
         SA1.WaitAddress = SA1.Map [0x00d675 >> MEMMAP_SHIFT] + 0xd675;
      /* Mini Yonku Shining Scorpion - Let's & Go!! (J) */
      else if (match_id("A4WJ"))
         SA1.WaitAddress = SA1.Map [0xc048be >> MEMMAP_SHIFT] + 0x48be;
      /* Shin Shougi Club (J) */
      else if (match_id("AHJJ"))
      {
         SA1.WaitAddress = SA1.Map [0xc1002a >> MEMMAP_SHIFT] + 0x002a;
         SA1.WaitByteAddress1 = Memory.SRAM + 0x0806;
         SA1.WaitByteAddress2 = Memory.SRAM + 0x0808;
      }
      /* Shougi Saikyou (J) */
      else if (match_id("AMSJ"))
         SA1.WaitAddress = SA1.Map [0x00CD6A >> MEMMAP_SHIFT] + 0xCD6A;
      /* Habu Meijin no Omoshiro Shougi (J) */
      else if (match_id("IL"))
         SA1.WaitAddress = SA1.Map [0x008549 >> MEMMAP_SHIFT] + 0x8549;
      /* Masoukishin (J) */
      else if (match_id("ALXJ"))
      {
         SA1.WaitAddress = SA1.Map [0x00EC9C >> MEMMAP_SHIFT] + 0xEC9C;
         SA1.WaitByteAddress1 = Memory.FillRAM + 0x3072;
      }
      /* Super Shogi 3 (J) */
      else if (match_id("A3IJ"))
         SA1.WaitAddress = SA1.Map [0x00F669 >> MEMMAP_SHIFT] + 0xF669;
   }

   /* not MAD-1 compliant */
   if (match_na("WANDERERS FROM YS"))
   {
      int32_t c;
      for (c = 0; c < 0xE0; c++)
      {
         Memory.Map[c + 0x700] = MAP_LOROM_SRAM_OR_NONE;
         Memory.MapInfo[c + 0x700].Type = MAP_TYPE_RAM;
      }
      WriteProtectROM();
   }

   if (strncmp(Memory.ROMName, "WAR 2410", 8) == 0)
   {
      Memory.Map [0x005] = (uint8_t*) Memory.RAM;
      Memory.MapInfo[0x005].Type = MAP_TYPE_RAM;
   }

   /* NMI hacks */
   CPU.NMITriggerPoint = 4;
   if (match_na("CACOMA KNIGHT"))
      CPU.NMITriggerPoint = 25;

   /* Disabling a speed-up:
      Games which spool sound samples between the SNES and sound CPU using
      H-DMA as the sample is playing. */
   if (match_na("EARTHWORM JIM 2") ||
         match_na("PRIMAL RAGE") ||
         match_na("CLAY FIGHTER") ||
         match_na("ClayFighter 2") ||
         strncasecmp(Memory.ROMName, "MADDEN", 6) == 0 ||
         strncmp(Memory.ROMName, "NHL", 3) == 0 ||
         match_na("WeaponLord") ||
         strncmp(Memory.ROMName, "WAR 2410", 8) == 0)
      Settings.Shutdown = false;

   /* APU timing hacks */

#ifndef USE_BLARGG_APU
   /* Stunt Racer FX */
   if (match_id("CQ  ") ||
         /* Illusion of Gaia */
         strncmp(Memory.ROMId, "JG", 2) == 0 ||
         match_na("GAIA GENSOUKI 1 JPN"))
      IAPU.OneCycle = 13;
   else if (strcmp (Memory.ROMName, "UMIHARAKAWASE") == 0)
      IAPU.OneCycle = 20;
   /* RENDERING RANGER R2 */
   else if (match_id("AVCJ") ||
         /* Mark Davis */
         strncmp(Memory.ROMName, "THE FISHING MASTER", 18) == 0 || /* needs >= actual APU timing. (21 is .002 Mhz slower) */
         /* Star Ocean */
         strncmp(Memory.ROMId, "ARF", 3) == 0 ||
         /* Tales of Phantasia */
         strncmp(Memory.ROMId, "ATV", 3) == 0 ||
         /* Act Raiser 1 & 2 */
         strncasecmp(Memory.ROMName, "ActRaiser", 9) == 0 ||
         /* Soulblazer */
         match_na("SOULBLAZER - 1 USA") ||
         match_na("SOULBLADER - 1") ||
         /* Terranigma */
         strncmp(Memory.ROMId, "AQT", 3) == 0 ||
         /* Robotrek */
         strncmp(Memory.ROMId, "E9 ", 3) == 0 ||
         match_na("SLAP STICK 1 JPN") ||
         /* ZENNIHON PURORESU2 */
         strncmp(Memory.ROMId, "APR", 3) == 0 ||
         /* Bomberman 4 */
         strncmp(Memory.ROMId, "A4B", 3) == 0 ||
         /* UFO KAMEN YAKISOBAN */
         strncmp(Memory.ROMId, "Y7 ", 3) == 0 ||
         strncmp(Memory.ROMId, "Y9 ", 3) == 0 ||
         /* Panic Bomber World */
         strncmp(Memory.ROMId, "APB", 3) == 0 ||
         ((strncmp(Memory.ROMName, "Parlor", 6) == 0 ||
         match_na("HEIWA Parlor!Mini8") ||
         strncmp(Memory.ROMName, "SANKYO Fever! \xCC\xA8\xB0\xCA\xDE\xB0!", 21) == 0) &&
         strcmp(Memory.CompanyId, "A0") == 0) ||
         match_na("DARK KINGDOM") ||
         match_na("ZAN3 SFC") ||
         match_na("HIOUDEN") ||
         match_na("\xC3\xDD\xBC\xC9\xB3\xC0") || /* Tenshi no Uta */
         match_na("FORTUNE QUEST") ||
         match_na("FISHING TO BASSING") ||
         strncmp(Memory.ROMName, "TokyoDome '95Battle 7", 21) == 0 ||
         match_na("OHMONO BLACKBASS") ||
         strncmp(Memory.ROMName, "SWORD WORLD SFC", 15) == 0 ||
         match_na("MASTERS") || /* Augusta 2 J */
         match_na("SFC \xB6\xD2\xDD\xD7\xB2\xC0\xDE\xB0") || /* Kamen Rider */
         strncmp(Memory.ROMName, "LETs PACHINKO(", 14) == 0)  /* A set of BS games */
      IAPU.OneCycle = 15;
#endif

   /* Specific game fixes */
   Settings.StarfoxHack = match_na("STAR FOX") || match_na("STAR WING");
   Settings.WinterGold = match_na("FX SKIING NINTENDO 96") || match_na("DIRT RACER") || Settings.StarfoxHack;
   Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;

   /* CPU timing hacks */
   Settings.H_Max = (SNES_CYCLES_PER_SCANLINE * Settings.CyclesPercentage) / 100;

   /* A Couple of HDMA related hacks - Lantus */
   if ((match_na("SFX SUPERBUTOUDEN2")) || (match_na("ALIEN vs. PREDATOR")) || (match_na("STONE PROTECTORS")) || (match_na("SUPER BATTLETANK 2")))
      Settings.H_Max = (SNES_CYCLES_PER_SCANLINE * 130) / 100;
   else if (match_na("HOME IMPROVEMENT"))
      Settings.H_Max = (SNES_CYCLES_PER_SCANLINE * 200) / 100;
   else if (match_id("ASRJ") && Settings.CyclesPercentage == 100)
      /* Street Racer */
      Settings.H_Max = (SNES_CYCLES_PER_SCANLINE * 95) / 100;
   /* Power Rangers Fight */
   else if (strncmp(Memory.ROMId, "A3R", 3) == 0 ||
         /* Clock Tower */
         strncmp(Memory.ROMId, "AJE", 3) == 0)
      Settings.H_Max = (SNES_CYCLES_PER_SCANLINE * 103) / 100;
   else if (strncmp(Memory.ROMId, "A3M", 3) == 0 && Settings.CyclesPercentage == 100)
      /* Mortal Kombat 3. Fixes cut off speech sample */
      Settings.H_Max = (SNES_CYCLES_PER_SCANLINE * 110) / 100;
   else if (match_na("\x0bd\x0da\x0b2\x0d4\x0b0\x0bd\x0de") &&
         Settings.CyclesPercentage == 100)
      Settings.H_Max = (SNES_CYCLES_PER_SCANLINE * 101) / 100;
   else if (match_na("WILD TRAX") || match_na("STAR FOX 2") || match_na("YOSSY'S ISLAND") || match_na("YOSHI'S ISLAND"))
      CPU.TriedInterleavedMode2 = true;
   /* Start Trek: Deep Sleep 9 */
   else if (strncmp(Memory.ROMId, "A9D", 3) == 0 && Settings.CyclesPercentage == 100)
      Settings.H_Max = (SNES_CYCLES_PER_SCANLINE * 110) / 100;

   /* Other */

   /* Additional game fixes by sanmaiwashi ... */
   /* Gundam Knight Story */
   if (match_na("SFX \xC5\xB2\xC4\xB6\xDE\xDD\xC0\xDE\xD1\xD3\xC9\xB6\xDE\xC0\xD8 1"))
   {
      bytes0x2000 [0xb18] = 0x4c;
      bytes0x2000 [0xb19] = 0x4b;
      bytes0x2000 [0xb1a] = 0xea;
   }
}
