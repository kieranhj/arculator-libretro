#ifndef LIBRETRO_SOUND_H
#define LIBRETRO_SOUND_H
#include <stddef.h>
#include <stdint.h>
/* Copy up to max_frames stereo frames out of the ring; returns frames copied. */
size_t libretro_sound_read(int16_t* out, size_t max_frames);
void libretro_sound_reset(void);
#endif
