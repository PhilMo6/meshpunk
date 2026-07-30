#ifndef RACE_LIBRETRO_LOG_H__
#define RACE_LIBRETRO_LOG_H__

#include "libretro.h"

#ifdef __cplusplus
extern "C" {
#endif

void init_log(retro_environment_t environ_cb);
void handle_error( const char* error );
void handle_info( const char* info );

#ifdef __cplusplus
}
#endif

#endif
