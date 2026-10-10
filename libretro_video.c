/* Video backend for the libretro core: implements plat_video.h without a
 * window. Mirrors the clipping of video_sdl2.c and headless_video.c, but
 * instead of composing into a 2048x2048 texture and reading it back, the
 * visible rectangle is taken straight from the VIDC's bitmap at frame end:
 * converted to RGB565 (one pass, half the bytes written), or copied as
 * XRGB8888. Every caller in vidc.c passes dest (0,0) and then presents from
 * (0,0), so the composition step is redundant here.
 *
 * The copy happens here rather than in retro_run() because the VIDC draws
 * each scanline into its bitmap as the beam goes; by the time retro_run()
 * ends, the top of the bitmap already holds the next frame. Taking the frame
 * at video_renderer_update() time (VIDC frame end) keeps it whole, and the
 * frontend never sees a pointer into the emulator's buffer.
 *
 * Profiling on the RG40XXV showed the earlier XRGB memcpy plus a separate
 * RGB565 conversion costing 7% to 15% of a frame; this is the fix.
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

static void* s_fb;              /* FB_W * FB_H pixels at the current depth */
static int s_rgb565;
static int s_native_lowres;
static int s_halved;            /* the last update was written at half width */
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

void
libretro_video_set_format(int rgb565, int native_lowres) {
  s_rgb565 = rgb565;
  s_native_lowres = native_lowres;
}

int video_renderer_init(void* main_window) {
  (void) main_window;
  if (!s_fb) {
    /* Always sized for 32 bpp; the 16 bpp case uses half of it. */
    s_fb = calloc((size_t) FB_W * FB_H, sizeof(uint32_t));
    if (!s_fb) return 0;
  }
  memset(&libretro_frame, 0, sizeof(libretro_frame));
  libretro_frame.p_pixels = s_fb;
  libretro_frame.bpp = s_rgb565 ? 16 : 32;
  libretro_frame.pitch = FB_W * (s_rgb565 ? 2 : 4);
  return 1;
}

int video_renderer_reinit(void* main_window) { return video_renderer_init(main_window); }

void video_renderer_close(void) {
  free(s_fb);
  s_fb = NULL;
  memset(&libretro_frame, 0, sizeof(libretro_frame));
}

/* The VIDC register file is static to vidc.c; the pixel rate is read back
 * through the fork's JSON state report instead. Once per VIDC frame, so the
 * formatting cost is noise. */
static int
vidc_is_low_pixel_rate(void) {
  char buf[2048];
  const char* p;
  vidc_get_state_json(buf, sizeof(buf));
  p = strstr(buf, "\"pixel_clock_mhz\":");
  return p && atof(p + 18) < 15.0;
}

static inline uint16_t
to565(uint32_t p) {
  return (uint16_t) (((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
}

void video_renderer_update(BITMAP* src, int src_x, int src_y, int dest_x, int dest_y, int w, int h) {
  int row;
  int step;
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

  /* At the 8 and 12 MHz pixel rates (VIDC control register bit 1 clear) the
   * renderer draws every guest pixel twice horizontally; emit one of each. */
  s_halved = s_native_lowres && vidc_is_low_pixel_rate() && w >= 2;
  step = s_halved ? 2 : 1;

  for (row = 0; row < h; row++) {
    const uint32_t* s = &((const uint32_t*) src->dat)[(src_y + row) * src->w + src_x];
    if (s_rgb565) {
      uint16_t* d = (uint16_t*) s_fb + (size_t) (dest_y + row) * FB_W + dest_x;
      int x, n = w / step;
      if (step == 1) {
        for (x = 0; x < n; x++) d[x] = to565(s[x]);
      } else {
        for (x = 0; x < n; x++) d[x] = to565(s[x * 2]);
      }
    } else {
      uint32_t* d = (uint32_t*) s_fb + (size_t) (dest_y + row) * FB_W + dest_x;
      if (step == 1) {
        memcpy(d, s, (size_t) w * sizeof(uint32_t));
      } else {
        int x, n = w / 2;
        for (x = 0; x < n; x++) d[x] = s[x * 2];
      }
    }
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
  if (s_halved) { src_x /= 2; src_w /= 2; }
  libretro_frame.bpp = s_rgb565 ? 16 : 32;
  libretro_frame.pitch = FB_W * (s_rgb565 ? 2 : 4);
  libretro_frame.p_pixels = (const uint8_t*) s_fb + (size_t) src_y * libretro_frame.pitch + (size_t) src_x * (s_rgb565 ? 2 : 4);
  libretro_frame.w = src_w;
  libretro_frame.h = src_h;
  libretro_frame.dblscan = dblscan;
  libretro_frame.frame_count++;
  libretro_frame.valid = (src_w > 0 && src_h > 0);
}
