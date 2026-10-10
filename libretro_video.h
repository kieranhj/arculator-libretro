#ifndef LIBRETRO_VIDEO_H
#define LIBRETRO_VIDEO_H
#include <stdint.h>
#include <stddef.h>

/* The last rectangle the VIDC presented, in a buffer owned by the core. */
typedef struct {
  int valid;
  const uint32_t* p_pixels;   /* XRGB8888, top-left of the visible rect */
  size_t pitch;               /* bytes per row */
  int w, h;
  int dblscan;                /* guest asked for each line to be shown twice */
  uint64_t frame_count;       /* incremented on every present */
} libretro_frame_t;

extern libretro_frame_t libretro_frame;

#endif
