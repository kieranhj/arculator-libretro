/* Sound backend for the libretro core: implements plat_sound.h with a ring
 * buffer instead of an audio device. sound.c hands over 2400 stereo frames at
 * 48 kHz every 50 ms of emulated time; core.c drains the ring into the
 * frontend after each retro_run(). Drive noise (sound_givebufferdd) is mixed
 * by ddnoise_mix(), which the core never calls, so it is a no-op here.
 */
#include <string.h>
#include "arc.h"
#include "plat_sound.h"
#include "libretro_sound.h"

#define FRAMES_PER_BUFFER 2400
#define RING_FRAMES 16384   /* power of two, about 340 ms */

static int16_t s_ring[RING_FRAMES * 2];
static unsigned s_wp, s_rp;   /* in frames, free-running */

void sound_dev_init(void) { s_wp = s_rp = 0; }
void sound_dev_close(void) {}

void sound_givebuffer(int16_t* buf) {
  unsigned i;
  if ((s_wp - s_rp) + FRAMES_PER_BUFFER > RING_FRAMES) {
    /* Frontend is not draining (paused, or fast-forward dropped audio): keep
     * the newest audio by discarding the oldest. */
    s_rp = s_wp + FRAMES_PER_BUFFER - RING_FRAMES;
  }
  for (i = 0; i < FRAMES_PER_BUFFER; i++) {
    unsigned slot = (s_wp + i) & (RING_FRAMES - 1);
    s_ring[slot * 2] = buf[i * 2];
    s_ring[slot * 2 + 1] = buf[i * 2 + 1];
  }
  s_wp += FRAMES_PER_BUFFER;
}

void sound_givebufferdd(int16_t* buf) { (void) buf; }

size_t libretro_sound_read(int16_t* out, size_t max_frames) {
  size_t avail = s_wp - s_rp;
  size_t n = avail < max_frames ? avail : max_frames;
  size_t i;
  for (i = 0; i < n; i++) {
    unsigned slot = (s_rp + i) & (RING_FRAMES - 1);
    out[i * 2] = s_ring[slot * 2];
    out[i * 2 + 1] = s_ring[slot * 2 + 1];
  }
  s_rp += n;
  return n;
}

void libretro_sound_reset(void) { s_wp = s_rp = 0; }
