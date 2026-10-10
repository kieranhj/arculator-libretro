/* Video backend for the libretro core: implements plat_video.h without a
 * window. Mirrors the clipping of video_sdl2.c and headless_video.c, but
 * instead of composing into a 2048x2048 texture and reading it back, the
 * visible rectangle is copied straight into a frame buffer owned by the core.
 * Every caller in vidc.c passes dest (0,0) and then presents from (0,0), so
 * the composition step is redundant here.
 *
 * The frame buffer is allocated once and never handed back to the emulator,
 * so what the frontend receives is a private copy (see retro_run in core.c).
 */
#include <stdlib.h>
#include <string.h>
#include "arc.h"
#include "plat_video.h"
#include "vidc.h"
#include "video.h"
#include "libretro_video.h"

int selected_video_renderer;

#define FB_W 2048
#define FB_H 2048

static uint32_t* s_fb;
libretro_frame_t libretro_frame;

static const char* renderer_names[RENDERER_COUNT] = { "Auto", "Direct3D", "OpenGL", "Software" };

int video_renderer_available(int id) { return (id == RENDERER_AUTO || id == RENDERER_SOFTWARE); }
char* video_renderer_get_name(int id) {
  if (id < 0 || id >= RENDERER_COUNT) return (char*) renderer_names[RENDERER_AUTO];
  return (char*) renderer_names[id];
}
int video_renderer_get_id(char* name) {
  int c;
  for (c = 0; c < RENDERER_COUNT; c++) if (!strcmp(name, renderer_names[c])) return c;
  return RENDERER_AUTO;
}

int video_renderer_init(void* main_window) {
  (void) main_window;
  if (!s_fb) {
    s_fb = calloc((size_t) FB_W * FB_H, sizeof(uint32_t));
    if (!s_fb) return 0;
  }
  memset(&libretro_frame, 0, sizeof(libretro_frame));
  libretro_frame.p_pixels = s_fb;
  libretro_frame.pitch = FB_W * sizeof(uint32_t);
  return 1;
}

int video_renderer_reinit(void* main_window) { return video_renderer_init(main_window); }

void video_renderer_close(void) {
  free(s_fb);
  s_fb = NULL;
  memset(&libretro_frame, 0, sizeof(libretro_frame));
}

void video_renderer_update(BITMAP* src, int src_x, int src_y, int dest_x, int dest_y, int w, int h) {
  int row;
  if (!s_fb || !src || !src->dat) return;

  /* Clip against the source bitmap: the guest can ask for a rect that starts
   * off the top or left during mode changes. */
  if (src_x < 0) { w += src_x; dest_x -= src_x; src_x = 0; }
  if (src_y < 0) { h += src_y; dest_y -= src_y; src_y = 0; }
  if (dest_x < 0) { w += dest_x; src_x -= dest_x; dest_x = 0; }
  if (dest_y < 0) { h += dest_y; src_y -= dest_y; dest_y = 0; }
  if (src_x >= src->w || src_y >= src->h || dest_x >= FB_W || dest_y >= FB_H) return;
  if (src_x + w > src->w) w = src->w - src_x;
  if (src_y + h > src->h) h = src->h - src_y;
  if (dest_x + w > FB_W) w = FB_W - dest_x;
  if (dest_y + h > FB_H) h = FB_H - dest_y;
  if (w <= 0 || h <= 0) return;

  for (row = 0; row < h; row++) {
    const uint32_t* s = &((uint32_t*) src->dat)[(src_y + row) * src->w + src_x];
    uint32_t* d = &s_fb[(dest_y + row) * FB_W + dest_x];
    memcpy(d, s, (size_t) w * sizeof(uint32_t));
  }
}

void video_renderer_present(int src_x, int src_y, int src_w, int src_h, int dblscan) {
  if (!s_fb) return;
  if (src_x < 0) src_x = 0;
  if (src_y < 0) src_y = 0;
  if (src_w < 0) src_w = 0;
  if (src_h < 0) src_h = 0;
  if (src_x + src_w > FB_W) src_w = FB_W - src_x;
  if (src_y + src_h > FB_H) src_h = FB_H - src_y;
  libretro_frame.p_pixels = &s_fb[src_y * FB_W + src_x];
  libretro_frame.w = src_w;
  libretro_frame.h = src_h;
  libretro_frame.dblscan = dblscan;
  libretro_frame.frame_count++;
  libretro_frame.valid = (src_w > 0 && src_h > 0);
}
