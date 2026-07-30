#ifndef MAIN_H
#define MAIN_H

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <ctype.h>
#include <math.h>

#define KEY_UP			0
#define KEY_DOWN		1
#define KEY_LEFT		2
#define KEY_RIGHT		3
#define KEY_START		4
#define KEY_BUTTON_A	5
#define KEY_BUTTON_B	6
#define KEY_SELECT		7
#define KEY_UP_2		8
#define KEY_DOWN_2		9
#define KEY_LEFT_2		10
#define KEY_RIGHT_2		11

// Possible Neogeo Pocket versions
#define NGP				0x00
#define NGPC			0x01

#define NR_OF_SYSTEMS	2

#ifdef __cplusplus
extern "C" {
#endif

typedef struct EMUINFO
{
	char 	RomFileName[_MAX_PATH];

	int		machine;		// what kind of machine should we emulate
	int		romSize;		// what is the size of the currently loaded file
	int		samples;
} EMUINFO;

extern int		m_bIsActive;
extern EMUINFO		m_emuInfo;
extern int romSize;

int handleInputFile(const char *romName,
		const unsigned char *romData, int romSize);
void mainemuinit(void);

#ifdef __cplusplus
}
#endif

#endif
