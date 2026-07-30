#ifndef NEOPOP_BLIP_H
#define NEOPOP_BLIP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void neopop_blip_init(int sample_rate);
void neopop_blip_reset(void);
void neopop_blip_run(int chip_cycles);
void neopop_blip_sync_tone(int chan, int period_div, int volume_idx);
void neopop_blip_sync_noise(int period_div, int volume_idx, int feedback_periodic);
void neopop_blip_dac(int dac_value);
int  neopop_blip_flush(int16_t *stereo_out, int frames);

/* Runtime selector: 0 = fast (per-sample), 1 = accurate (blip). */
extern int neopop_audio_accurate;

#ifdef __cplusplus
}
#endif

#endif
