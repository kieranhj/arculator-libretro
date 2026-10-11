#ifndef LIBRETRO_VIDEO_H
#define LIBRETRO_VIDEO_H
#include <stdint.h>
#include <stddef.h>

/* The last rectangle the VIDC presented, in a buffer owned by the core. */
typedef struct {
  int valid;
  const void* p_pixels;       /* top-left of the visible rect */
  size_t pitch;               /* bytes per row */
  int bpp;                    /* 16 (RGB565) or 32 (XRGB8888) */
  int w, h;                   /* as presented: w is halved for low-res modes when enabled */
  int dblscan;                /* guest asked for each line to be shown twice */
  int full_w;                 /* w before low-res halving, in the VIDC bitmap's pixels */
  uint64_t frame_count;       /* incremented on every present */
} libretro_frame_t;

extern libretro_frame_t libretro_frame;

/* rgb565: convert at frame end instead of copying XRGB8888.
 * native_lowres: present 8 and 12 MHz pixel-rate modes at one bitmap pixel
 * per two, since the VIDC renderer doubles them horizontally. */
void libretro_video_set_format(int rgb565, int native_lowres);

#endif
