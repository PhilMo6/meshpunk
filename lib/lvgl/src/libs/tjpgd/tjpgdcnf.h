/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/*----------------------------------------------*/

#define JD_SZBUF        512
/* Specifies size of stream input buffer */

#define JD_FORMAT       0
/* Specifies output pixel format.
/  0: RGB888 (24-bit/pix)
/  1: RGB565 (16-bit/pix)
/  2: Grayscale (8-bit/pix)
*/

/* MESHPUNK: enabled so jd_decomp() can descale by 1/2, 1/4 or 1/8 while it
   decodes — that is what lets src/img_bridge.cpp open a multi-megapixel JPEG
   into a screen-sized buffer without ever holding the full-resolution image.
   Additive for LVGL's own decoder: lv_tjpgd.c pins jd->scale = 0. */
#define JD_USE_SCALE    1
/* Switches output descaling feature.
/  0: Disable
/  1: Enable
*/

#define JD_TBLCLIP      1
/* Use table conversion for saturation arithmetic. A bit faster, but increases 1 KB of code size.
/  0: Disable
/  1: Enable
*/

#define JD_FASTDECODE   1
/* Optimization level
/  0: Basic optimization. Suitable for 8/16-bit MCUs.
/  1: + 32-bit barrel shifter. Suitable for 32-bit MCUs.
/  2: + Table conversion for huffman decoding (wants 6 << HUFF_BIT bytes of RAM)
*/

