/*---------------------------------------------------------------------------
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *	(at your option) any later version. See also the license.txt file for
 *	additional informations.
 *---------------------------------------------------------------------------
 */

/* graphics.h: interface for the graphics class.
 *
 */

#ifndef AFX_GRAPHICS_H
#define AFX_GRAPHICS_H

/* actual NGPC */
#define NGPC_SIZEX 160
#define NGPC_SIZEY 152

/* render screen 260x152 is good for NGPC */
#define SIZEX	260
#define SIZEY	152

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned char *sprite_table;
extern unsigned char *pattern_table;
extern unsigned short*patterns;
extern unsigned short *tile_table_front;
extern unsigned short *tile_table_back;
extern unsigned short *palette_table;
extern unsigned char *bw_palette_table;
extern unsigned char *sprite_palette_numbers;

extern unsigned char *scanlineY;
/* frame 0/1 priority registers */
extern unsigned char *frame0Pri;
extern unsigned char *frame1Pri;
/* windowing registers */
extern unsigned char *wndTopLeftX;
extern unsigned char *wndTopLeftY;
extern unsigned char *wndSizeX;
extern unsigned char *wndSizeY;
/* scrolling registers */
extern unsigned char *scrollSpriteX;
extern unsigned char *scrollSpriteY;
extern unsigned char *scrollFrontX;
extern unsigned char *scrollFrontY;
extern unsigned char *scrollBackX;
extern unsigned char *scrollBackY;
/* background color selection register and table */
extern unsigned char *bgSelect;
extern unsigned short *bgTable;
extern unsigned char *oowSelect;
extern unsigned short *oowTable;
/* machine constants */
extern unsigned char *color_switch;

void graphics_init(void);
void graphics_paint(unsigned char render);
void graphicsSetDarkFilterLevel(unsigned filterLevel);
/* new renderer (NeoGeo Pocket (Color)) */
void myGraphicsBlitLine(unsigned char render);
/* MESHPUNK: pure per-line pixel body (no raster/VBlank side effects). */
void myGraphicsRenderLine(int line);

/* MESHPUNK: per-line register journal for the Core-1 render worker. The
   raster wrapper records the 13 render-relevant byte registers as each
   emulated line executes; the worker replays them line-by-line against its
   frame snapshot, reproducing mid-frame (raster) effects exactly. */
typedef struct {
    unsigned char wtlx, wtly, wszx, wszy;
    unsigned char oow, bgsel, f1pri;
    unsigned char sfx, sfy, sbx, sby;
    unsigned char ssx, ssy;
} race_line_regs;

extern race_line_regs race_journal_live[152];
extern race_line_regs race_journal_snap[152];
extern int race_journal_active;
void race_journal_init(void);        /* capture live reg pointers + activate */

/*
 * adventure vision stuff
 */

extern unsigned short palettes[16*4+16*4+16*4]; /* placeholder for the converted palette */
extern int    totalpalette[32*32*32];
#define NGPC_TO_RGB565(col) totalpalette[col & 0x0FFF]

#define setColPaletteEntry(addr, data) palettes[(addr)] = NGPC_TO_RGB565(data)
#define setBWPaletteEntry(addr, data) palettes[(addr)] = NGPC_TO_RGB565(data)

extern unsigned char *scanlineY;

struct ngp_screen
{
   int w, h;                 
   void *pixels;
};

#ifdef __cplusplus
}
#endif

#endif /* !defined(AFX_GRAPHICS_H) */
