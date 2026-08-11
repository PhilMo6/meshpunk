/* MESHPUNK: shim for gwenesis's Game & Watch build.
 *
 * gwenesis_vdp_gfx.c includes the STM32 CMSIS header on the embedded path.
 * Nothing from it is used by the code we compile -- it comes in with the
 * G&W platform -- so an empty header keeps that path available on Xtensa.
 */
#ifndef _MESHPUNK_STM32_SHIM_H_
#define _MESHPUNK_STM32_SHIM_H_
#endif
