/* Video backend for the libretro core: implements plat_video.h without a
 * window. Mirrors the clipping of video_sdl2.c and headless_video.c, but
 * instead of composing into a 2048x2048 texture and reading it back, the
 * visible rectangle is taken straight from the VIDC's bitmap at frame end:
 * converted to RGB565, or copied as XRGB8888. Every caller in vidc.c passes
 * dest (0,0) and then presents from (0,0), so the composition step is
 * redundant here.
 *
 * The copy happens here rather than in retro_run() because the VIDC draws
 * each scanline into its bitmap as the beam goes; by the time retro_run()
 * ends, the top of the bitmap already holds the next frame. Taking the frame
 * at video_renderer_update() time (VIDC frame end) keeps it whole, and the
 * frontend never sees a pointer into the emulator's buffer.
 *
 * In RGB565 the conversion is done a scanline at a time, from
 * vidc_line_callback, while the line the VIDC has just drawn is still in the
 * cache, rather than by reading the 32 bpp bitmap back at frame end. Each
 * scanline is converted for the window the previous frame presented; frame
 * end then only copies 16 bpp rows. A row that was not converted for the
 * window now asked for (the first frame, a mode change) is converted from the
 * bitmap as before, so the output is the same either way.
 *
 * Both buffers here are packed to the frame's width. A power-of-two row
 * pitch maps every row to the same cache sets, and on the RG40XXV's
 * Cortex-A53 that alone made copying a 320x256 frame take 1.4 ms.
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

extern BITMAP* buffer;          /* the VIDC's bitmap (vidc.c) */

static void* s_fb;              /* FB_W * FB_H pixels at the current depth */
static size_t s_fb_pitch;       /* bytes per row of s_fb, set by each update */
static int s_rgb565;
static int s_native_lowres;
static int s_halved;            /* the last update was written at half width */
libretro_frame_t libretro_frame;

/* Line-time conversion (RGB565 only). Row r of s_shadow holds bitmap row r,
 * pixels s_win_x0, s_win_x0 + s_win_step, ... below s_win_x1, packed from
 * column 0 at s_win_pitch pixels per row. A row is current when
 * s_row_gen[r] == s_win_gen. */
static uint16_t* s_shadow;      /* FB_W * FB_H */
static uint32_t* s_row_gen;     /* FB_H */
static uint32_t s_win_gen = 1;
static int s_win_valid;
static int s_win_x0, s_win_x1, s_win_step;
static size_t s_win_pitch;

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

/* Row pitch in bytes for n pixels of bpp bytes: whole cache lines, and never
 * a multiple of 4 KB. */
static size_t
packed_pitch(int n, int bpp) {
  size_t p = ((size_t) n * bpp + 63) & ~(size_t) 63;
  if ((p & 4095) == 0) p += 64;
  return p;
}

static inline uint16_t
to565(uint32_t p) {
  return (uint16_t) (((p >> 8) & 0xf800) | ((p >> 5) & 0x07e0) | ((p >> 3) & 0x001f));
}

static inline void
convert565(uint16_t* d, const uint32_t* s, int n, int step) {
  int x;
  if (step == 1) {
    for (x = 0; x < n; x++) d[x] = to565(s[x]);
  } else {
    for (x = 0; x < n; x++) d[x] = to565(s[x * 2]);
  }
}

static void
line_cb(BITMAP* b, int row) {
  if (b != buffer || !s_shadow) return;
  if (row < 0) {
    /* Bitmap cleared: every converted row is stale. */
    s_win_gen++;
    return;
  }
  if (row >= FB_H || row >= b->h || !s_win_valid) return;
  convert565(s_shadow + (size_t) row * s_win_pitch, (const uint32_t*) b->line[row] + s_win_x0,
             (s_win_x1 - s_win_x0) / s_win_step, s_win_step);
  s_row_gen[row] = s_win_gen;
}

int video_renderer_init(void* main_window) {
  (void) main_window;
  if (!s_fb) {
    /* Always sized for 32 bpp; the 16 bpp case uses half of it. */
    s_fb = calloc((size_t) FB_W * FB_H, sizeof(uint32_t));
    if (!s_fb) return 0;
  }
  if (s_rgb565 && !s_shadow) {
    /* Room for FB_H rows of the widest window at its padded pitch. */
    s_shadow = malloc((size_t) (FB_W + 64) * FB_H * sizeof(uint16_t));
    s_row_gen = calloc(FB_H, sizeof(uint32_t));
    if (!s_shadow || !s_row_gen) {
      free(s_shadow); free(s_row_gen);
      s_shadow = NULL; s_row_gen = NULL;
    }
  }
  s_win_valid = 0;
  s_win_gen++;
  vidc_line_callback = s_shadow ? line_cb : NULL;
  s_fb_pitch = (size_t) FB_W * (s_rgb565 ? 2 : 4);
  memset(&libretro_frame, 0, sizeof(libretro_frame));
  libretro_frame.p_pixels = s_fb;
  libretro_frame.bpp = s_rgb565 ? 16 : 32;
  libretro_frame.pitch = s_fb_pitch;
  return 1;
}

int video_renderer_reinit(void* main_window) { return video_renderer_init(main_window); }

void video_renderer_close(void) {
  vidc_line_callback = NULL;
  free(s_fb);
  free(s_shadow);
  free(s_row_gen);
  s_fb = NULL;
  s_shadow = NULL;
  s_row_gen = NULL;
  s_win_valid = 0;
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

void video_renderer_update(BITMAP* src, int src_x, int src_y, int dest_x, int dest_y, int w, int h) {
  int row;
  int step;
  int n;
  int bpp = s_rgb565 ? 2 : 4;
  int use_shadow;
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
  n = w / step;

  /* Pack the output rows to the width presented (every caller in vidc.c
   * writes at dest (0,0) and presents from there). */
  s_fb_pitch = packed_pitch((dest_x + w) / step, bpp);
  if (s_fb_pitch * (size_t) (dest_y + h) > (size_t) FB_W * FB_H * 4)
    s_fb_pitch = (size_t) FB_W * bpp;

  /* Rows converted at line time can be used if they were converted for a
   * window that covers this rect with the same sampling. */
  use_shadow = s_rgb565 && s_shadow && src == buffer && s_win_valid && step == s_win_step &&
               src_x >= s_win_x0 && src_x + w <= s_win_x1 && (src_x - s_win_x0) % step == 0;

  for (row = 0; row < h; row++) {
    const uint32_t* s = (const uint32_t*) src->line[src_y + row] + src_x;
    uint8_t* d = (uint8_t*) s_fb + (size_t) (dest_y + row) * s_fb_pitch + (size_t) (dest_x / step) * bpp;
    if (s_rgb565) {
      if (use_shadow && s_row_gen[src_y + row] == s_win_gen)
        memcpy(d, s_shadow + (size_t) (src_y + row) * s_win_pitch + (src_x - s_win_x0) / step,
               (size_t) n * sizeof(uint16_t));
      else
        convert565((uint16_t*) d, s, n, step);
    } else {
      if (step == 1) {
        memcpy(d, s, (size_t) w * sizeof(uint32_t));
      } else {
        int x;
        for (x = 0; x < n; x++) ((uint32_t*) d)[x] = s[x * 2];
      }
    }
  }

  /* Convert the next frame's scanlines for the window just presented. */
  if (s_shadow && src == buffer &&
      !(s_win_valid && s_win_x0 == src_x && s_win_x1 == src_x + w && s_win_step == step)) {
    s_win_x0 = src_x;
    s_win_x1 = src_x + w;
    s_win_step = step;
    s_win_pitch = packed_pitch(n, 2) / 2;
    s_win_valid = 1;
    s_win_gen++;
  }
}

void video_renderer_present(int src_x, int src_y, int src_w, int src_h, int dblscan) {
  int bpp = s_rgb565 ? 2 : 4;
  if (!s_fb) return;
  if (src_x < 0) src_x = 0;
  if (src_y < 0) src_y = 0;
  if (src_w < 0) src_w = 0;
  if (src_h < 0) src_h = 0;
  if (src_x + src_w > FB_W) src_w = FB_W - src_x;
  if (src_y + src_h > FB_H) src_h = FB_H - src_y;
  libretro_frame.full_w = src_w;
  if (s_halved) { src_x /= 2; src_w /= 2; }
  libretro_frame.bpp = bpp * 8;
  libretro_frame.pitch = s_fb_pitch;
  libretro_frame.p_pixels = (const uint8_t*) s_fb + (size_t) src_y * s_fb_pitch + (size_t) src_x * bpp;
  libretro_frame.w = src_w;
  libretro_frame.h = src_h;
  libretro_frame.dblscan = dblscan;
  libretro_frame.frame_count++;
  libretro_frame.valid = (src_w > 0 && src_h > 0);
}
