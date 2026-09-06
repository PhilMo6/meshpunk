/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
**
** This program is free software; you can redistribute it and/or
** modify it under the terms of version 2 of the GNU Library General
** Public License as published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
** Library General Public License for more details.  To obtain a
** copy of the GNU Library General Public License, write to the Free
** Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** Any permitted reproduction of these routines, in whole or in part,
** must bear this legend.
**
**
** map163.c
**
** mappers 162 + 163 (Nanjing) interface
** MESHPUNK: ported from retro-go's map162.c (ducalex) to this
** nofrendo mapper API; one implementation serves both numbers.
*/

#include "../noftypes.h"
#include "../nes/nes_mmc.h"
#include "../nes/nes_ppu.h"
#include "../nes/nes.h"
#include "../log.h"

static uint8 reg5000;
static uint8 reg5100;
static uint8 reg5101;
static uint8 reg5200;
static uint8 reg5300;
static uint8 trigger;

/* Nanjing boards carry 8KB CHR RAM used as two 4KB banks. mmc_bankvrom
** is a no-op when vrom_banks == 0, so CHR RAM banks go through
** ppu_setpage directly (pointer argument = bank base minus address).
*/
static void map163_chr4(uint32 address, int bank)
{
   rominfo_t *cart = mmc_getinfo();

   if (cart->vrom_banks)
      mmc_bankvrom(4, address, bank);
   else
      ppu_setpage(4, address >> 10, cart->vram + ((bank & 1) << 12) - address);
}

static void map163_chr8(void)
{
   rominfo_t *cart = mmc_getinfo();

   if (cart->vrom_banks)
      mmc_bankvrom(8, 0x0000, 0);
   else
      ppu_setpage(8, 0, cart->vram);
}

static void map163_update(void)
{
   uint8 bank = (reg5200 & 0x3) << 4 | (reg5000 & 0xF);

   mmc_bankrom(32, 0x8000, bank);
}

/* With $5000 bit 7 set, CHR bank 1 renders scanlines 128-239 and
** bank 0 the rest of the frame (both pattern tables show one bank).
*/
static void map163_hblank(int vblank)
{
   UNUSED(vblank);

   if (reg5000 & 0x80)
   {
      int scanline = nes_getcontextptr()->scanline;

      if (127 == scanline)
      {
         map163_chr4(0x0000, 1);
         map163_chr4(0x1000, 1);
      }
      else if (239 == scanline)
      {
         map163_chr4(0x0000, 0);
         map163_chr4(0x1000, 0);
      }
   }
}

static uint8 map163_read(uint32 address)
{
   /* $5500 & 0x7300 == 0x5100, so $5500-family reads land in the
   ** 0x5100 case and the 0x5500 case is unreachable; both retained
   ** as retro-go ships them.
   */
   switch (address & 0x7300)
   {
   case 0x5100:
      return reg5300;

   case 0x5500:
      return trigger ? reg5300 : 0;

   default:
      nofrendo_log_printf("map163: unhandled read from $%04X\n", (unsigned)address);
      return 0x04;
   }
}

static void map163_write(uint32 address, uint8 value)
{
   switch (address & 0x7300)
   {
   case 0x5000:
      reg5000 = value;
      if (0 == (value & 0x80) && nes_getcontextptr()->scanline < 128)
         map163_chr8();
      map163_update();
      break;

   case 0x5100:
      if (0x5101 == address)
      {
         /* copy protection: a falling edge on $5101 toggles the trigger */
         if (reg5101 && 0 == value)
            trigger = !trigger;
         reg5101 = value;
      }
      else if (0x6 == value)
      {
         mmc_bankrom(32, 0x8000, 3);
      }
      reg5100 = value;
      break;

   case 0x5200:
      reg5200 = value;
      map163_update();
      break;

   case 0x5300:
      reg5300 = value;
      break;

   default:
      break;
   }
}

static void map163_init(void)
{
   reg5000 = 0;
   reg5100 = 1;
   reg5101 = 1;
   reg5200 = 0;
   reg5300 = 0;
   trigger = 0;

   map163_chr4(0x0000, 0);
   map163_chr4(0x1000, 0);
   map163_update();
}

static map_memread map163_memread[] =
    {
        {0x5000, 0x5FFF, map163_read},
        {-1, -1, NULL}};

static map_memwrite map163_memwrite[] =
    {
        {0x5000, 0x5FFF, map163_write},
        {-1, -1, NULL}};

mapintf_t map162_intf =
    {
        162,             /* mapper number */
        "Nanjing 162",   /* mapper name */
        map163_init,     /* init routine */
        NULL,            /* vblank callback */
        map163_hblank,   /* hblank callback */
        NULL,            /* get state (snss) */
        NULL,            /* set state (snss) */
        map163_memread,  /* memory read structure */
        map163_memwrite, /* memory write structure */
        NULL             /* external sound device */
};

mapintf_t map163_intf =
    {
        163,             /* mapper number */
        "Nanjing 163",   /* mapper name */
        map163_init,     /* init routine */
        NULL,            /* vblank callback */
        map163_hblank,   /* hblank callback */
        NULL,            /* get state (snss) */
        NULL,            /* set state (snss) */
        map163_memread,  /* memory read structure */
        map163_memwrite, /* memory write structure */
        NULL             /* external sound device */
};
