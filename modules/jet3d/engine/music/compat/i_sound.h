// Stand-in for Doom's sound interface: just enough for i_oplmusic.c to
// declare its module table. jet3d drives the player through jet_music.h and
// never goes near this indirection, but the table is what the file ends
// with, so it has to compile.

#ifndef JET_COMPAT_I_SOUND_H
#define JET_COMPAT_I_SOUND_H

#include "doomtype.h"

typedef enum
{
    SNDDEVICE_NONE = 0,
    SNDDEVICE_PCSPEAKER = 1,
    SNDDEVICE_ADLIB = 2,
    SNDDEVICE_SB = 3,
    SNDDEVICE_PAS = 4,
    SNDDEVICE_GUS = 5,
    SNDDEVICE_WAVEBLASTER = 6,
    SNDDEVICE_SOUNDCANVAS = 7,
    SNDDEVICE_GENMIDI = 8,
    SNDDEVICE_AWE32 = 9,
    SNDDEVICE_CD = 10,
} snddevice_t;

// Member order must match the initialiser at the end of i_oplmusic.c.
typedef struct
{
    snddevice_t *sound_devices;
    int num_sound_devices;

    boolean (*Init)(void);
    void (*Shutdown)(void);
    void (*SetMusicVolume)(int volume);
    void (*PauseMusic)(void);
    void (*ResumeMusic)(void);
    void *(*RegisterSong)(void *data, int len);
    void (*UnRegisterSong)(void *handle);
    void (*PlaySong)(void *handle, boolean looping);
    void (*StopSong)(void);
    boolean (*MusicIsPlaying)(void);
    void (*Poll)(void);
} music_module_t;

extern int snd_samplerate;

// Upstream picks this up transitively from i_system.h. The player calls it
// only for a broken instrument bank; jet_music.c logs and returns rather
// than aborting, because a game module has nowhere to abort to.
void I_Error(const char *error, ...);

#endif
