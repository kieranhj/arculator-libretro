/* Minimal libretro host for headless testing on the device.
 *
 *   rt <core.so> <content> <frames> <out.bgra> [key=value ...]
 *
 * Runs the core for <frames> frames with stub callbacks, answering core option
 * queries from the key=value arguments, and writes the last frame as raw
 * XRGB8888 (with a 16-byte header: width, height, pitch, 0). Build:
 *   zig cc -target aarch64-linux-gnu.2.31 -O2 -o rt scripts/rt.c -ldl
 * RT_SYSTEM_DIR names the system directory handed to the core (default /tmp).
 */
#define _GNU_SOURCE
#include "../libretro.h"

#include <dlfcn.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RT_PROFILE=<path>: sample the PC at 1 kHz of CPU time (SIGPROF) and write
 * "offset count" lines for samples inside the core, as offsets from its load
 * address, plus "other count" for everything else. scripts/profile.py
 * symbolises them against the unstripped .so. */
#define PROF_SLOTS (8u << 20)   /* 32 MB of core text, in 4-byte words */
static unsigned* s_prof_hist;
static unsigned long s_prof_other, s_prof_total;
static unsigned long s_core_base, s_core_end;
/* Samples outside the core, by page, so libc hot spots can be named. */
#define PROF_EXT 4096
static unsigned long s_ext_page[PROF_EXT];
static unsigned s_ext_count[PROF_EXT];
static unsigned* s_lr_hist;

static void prof_handler(int sig, siginfo_t* si, void* ctx) {
  ucontext_t* uc = (ucontext_t*) ctx;
  unsigned long pc;
  (void) sig; (void) si;
#if defined(__aarch64__)
  pc = uc->uc_mcontext.pc;
#else
  pc = 0;
#endif
  s_prof_total++;
  if (pc >= s_core_base && pc < s_core_end) s_prof_hist[(pc - s_core_base) >> 2]++;
  else {
    unsigned long page = pc & ~0xffful;
    unsigned i = (unsigned) ((pc >> 12) * 2654435761u) & (PROF_EXT - 1);
    s_prof_other++;
    while (s_ext_count[i] && s_ext_page[i] != page) i = (i + 1) & (PROF_EXT - 1);
    s_ext_page[i] = page; s_ext_count[i]++;
    /* Attribute libc time (memset, memcpy) to the caller in the core: the
     * link register still holds the return address for leaf routines. */
#if defined(__aarch64__)
    {
      unsigned long lr = uc->uc_mcontext.regs[30];
      if (lr >= s_core_base && lr < s_core_end) s_lr_hist[(lr - s_core_base) >> 2]++;
    }
#endif
  }
}

static void prof_start(void* h) {
  struct sigaction sa;
  struct itimerval it;
  Dl_info info;
  FILE* f;
  char line[512];
  unsigned long lo = 0, hi = 0;
  void* sym = dlsym(h, "retro_run");
  if (!dladdr(sym, &info)) { fprintf(stderr, "profile: dladdr failed\n"); return; }
  s_core_base = (unsigned long) info.dli_fbase;
  /* Find the extent of the core's mapping from /proc/self/maps. */
  f = fopen("/proc/self/maps", "r");
  while (f && fgets(line, sizeof(line), f)) {
    unsigned long a, b;
    const char* p_base = strrchr(info.dli_fname, '/');
    p_base = p_base ? p_base + 1 : info.dli_fname;
    if (sscanf(line, "%lx-%lx", &a, &b) == 2 && strstr(line, p_base)) {
      if (!lo || a < lo) lo = a;
      if (b > hi) hi = b;
    }
  }
  if (f) fclose(f);
  s_core_end = hi ? hi : s_core_base + (PROF_SLOTS << 2);
  if (s_core_end - s_core_base > (PROF_SLOTS << 2)) s_core_end = s_core_base + (PROF_SLOTS << 2);
  s_prof_hist = calloc(PROF_SLOTS, sizeof(unsigned));
  s_lr_hist = calloc(PROF_SLOTS, sizeof(unsigned));
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = prof_handler;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGPROF, &sa, NULL);
  it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 1000;
  it.it_value = it.it_interval;
  setitimer(ITIMER_PROF, &it, NULL);
  fprintf(stderr, "profile: core at %lx-%lx\n", s_core_base, s_core_end);
}

static void prof_stop(const char* path) {
  struct itimerval it;
  FILE* f;
  unsigned i;
  memset(&it, 0, sizeof(it));
  setitimer(ITIMER_PROF, &it, NULL);
  f = fopen(path, "w");
  if (!f) return;
  fprintf(f, "total %lu\nother %lu\n", s_prof_total, s_prof_other);
  for (i = 0; i < PROF_SLOTS; ++i) if (s_prof_hist[i]) fprintf(f, "%x %u\n", i << 2, s_prof_hist[i]);
  for (i = 0; i < PROF_EXT; ++i) if (s_ext_count[i]) fprintf(f, "ext %lx %u\n", s_ext_page[i], s_ext_count[i]);
  for (i = 0; i < PROF_SLOTS; ++i) if (s_lr_hist[i]) fprintf(f, "lr %x %u\n", i << 2, s_lr_hist[i]);
  fclose(f);
  {
    /* Keep the address map alongside, for symbolising the ext lines. */
    char mp[1024];
    FILE* m = fopen("/proc/self/maps", "r");
    FILE* o;
    snprintf(mp, sizeof(mp), "%s.maps", path);
    o = fopen(mp, "w");
    if (m && o) { char l[512]; while (fgets(l, sizeof(l), m)) fputs(l, o); }
    if (m) fclose(m);
    if (o) fclose(o);
  }
  fprintf(stderr, "profile: %lu samples, %lu outside the core, written to %s\n", s_prof_total, s_prof_other, path);
}

static int s_argc;
static char** s_argv;
static const void* s_frame;
static unsigned s_w, s_h;
static size_t s_pitch;
static unsigned long s_audio_frames;
static retro_keyboard_event_t s_key_cb;

/* RT_KEYS="frame:retrok[:hold],..." presses host key <retrok> (a RETROK_*
 * number) at <frame> and releases it <hold> frames later (default 3). */
static void
inject_keys(int frame) {
  const char* p = getenv("RT_KEYS");
  if (!p || !s_key_cb) return;
  while (*p) {
    int at = -1, key = 0, hold = 3;
    if (sscanf(p, "%d:%d:%d", &at, &key, &hold) >= 2) {
      if (frame == at) s_key_cb(true, key, 0, 0);
      if (frame == at + hold) s_key_cb(false, key, 0, 0);
    }
    p = strchr(p, ',');
    if (!p) break;
    p++;
  }
}

static void log_cb(enum retro_log_level level, const char* fmt, ...) {
  va_list va; (void) level;
  va_start(va, fmt); vfprintf(stderr, fmt, va); va_end(va);
}

static bool env_cb(unsigned cmd, void* data) {
  switch (cmd) {
  case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
    ((struct retro_log_callback*) data)->log = log_cb; return true;
  case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
    return *(enum retro_pixel_format*) data == RETRO_PIXEL_FORMAT_XRGB8888;
  case RETRO_ENVIRONMENT_GET_VARIABLE: {
    struct retro_variable* v = (struct retro_variable*) data;
    int i;
    size_t kl = strlen(v->key);
    v->value = NULL;
    for (i = 5; i < s_argc; ++i) {
      if (!strncmp(s_argv[i], v->key, kl) && s_argv[i][kl] == '=') {
        v->value = s_argv[i] + kl + 1;
        return true;
      }
    }
    return false;
  }
  case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
    *(bool*) data = false; return true;
  case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
    const char* p = getenv("RT_SYSTEM_DIR");
    *(const char**) data = p ? p : "/tmp";
    return true;
  }
  case RETRO_ENVIRONMENT_SET_GEOMETRY: {
    const struct retro_game_geometry* g = (const struct retro_game_geometry*) data;
    fprintf(stderr, "geometry change %ux%u\n", g->base_width, g->base_height);
    return true;
  }
  case RETRO_ENVIRONMENT_SET_MESSAGE:
    fprintf(stderr, "message: %s\n", ((const struct retro_message*) data)->msg);
    return true;
  case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
    s_key_cb = ((const struct retro_keyboard_callback*) data)->callback;
    return true;
  case RETRO_ENVIRONMENT_SET_VARIABLES:
  case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    return true;
  default:
    return false;
  }
}

static void video_cb(const void* data, unsigned w, unsigned h, size_t pitch) {
  if (data) { s_frame = data; s_w = w; s_h = h; s_pitch = pitch; }
}
static int s_audio_peak;
static size_t audio_batch_cb(const int16_t* d, size_t frames) {
  size_t i;
  for (i = 0; i < frames * 2; ++i) { int v = d[i] < 0 ? -d[i] : d[i]; if (v > s_audio_peak) s_audio_peak = v; }
  s_audio_frames += frames; return frames;
}
static void audio_cb(int16_t l, int16_t r) { (void) l; (void) r; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
  (void) port; (void) dev; (void) idx; (void) id; return 0;
}

#define SYM(name) name = dlsym(h, #name); if (!name) { fprintf(stderr, "missing %s\n", #name); return 1; }

int main(int argc, char** argv) {
  void* h;
  int frames, i;
  struct retro_game_info info;
  struct retro_system_av_info av;
  void (*retro_set_environment)(retro_environment_t);
  void (*retro_set_video_refresh)(retro_video_refresh_t);
  void (*retro_set_audio_sample)(retro_audio_sample_t);
  void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
  void (*retro_set_input_poll)(retro_input_poll_t);
  void (*retro_set_input_state)(retro_input_state_t);
  void (*retro_init)(void);
  void (*retro_deinit)(void);
  bool (*retro_load_game)(const struct retro_game_info*);
  void (*retro_unload_game)(void);
  void (*retro_run)(void);
  void (*retro_get_system_av_info)(struct retro_system_av_info*);

  if (argc < 5) { fprintf(stderr, "usage: rt core content frames out.bgra [key=value...]\n"); return 2; }
  s_argc = argc; s_argv = argv;
  frames = atoi(argv[3]);

  h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
  SYM(retro_set_environment) SYM(retro_set_video_refresh) SYM(retro_set_audio_sample)
  SYM(retro_set_audio_sample_batch) SYM(retro_set_input_poll) SYM(retro_set_input_state)
  SYM(retro_init) SYM(retro_deinit) SYM(retro_load_game) SYM(retro_unload_game)
  SYM(retro_run) SYM(retro_get_system_av_info)

  retro_set_environment(env_cb);
  retro_set_video_refresh(video_cb);
  retro_set_audio_sample(audio_cb);
  retro_set_audio_sample_batch(audio_batch_cb);
  retro_set_input_poll(input_poll_cb);
  retro_set_input_state(input_state_cb);
  retro_init();
  memset(&info, 0, sizeof(info));
  info.path = argv[2];
  /* Pass "-" as the content path to boot with no disc. */
  if (!retro_load_game(strcmp(argv[2], "-") ? &info : NULL)) { fprintf(stderr, "load_game failed\n"); return 1; }
  retro_get_system_av_info(&av);
  fprintf(stderr, "geometry %ux%u fps %.2f rate %.0f\n", av.geometry.base_width,
          av.geometry.base_height, av.timing.fps, av.timing.sample_rate);
  if (getenv("RT_PROFILE")) prof_start(h);
  {
    /* RT_SLEEP_US=20000 paces frames like a real frontend would. */
    const char* p_sleep = getenv("RT_SLEEP_US");
    const char* p_start = getenv("RT_START_SLEEP_US");
    long sleep_us = p_sleep ? atol(p_sleep) : 0;
    if (p_start) usleep(atol(p_start));
    for (i = 0; i < frames; ++i) {
      inject_keys(i);
      retro_run();
      if (sleep_us > 0) usleep(sleep_us);
    }
  }
  if (getenv("RT_PROFILE")) prof_stop(getenv("RT_PROFILE"));
  fprintf(stderr, "ran %d frames, %lu audio frames (peak %d), last frame %ux%u pitch %zu\n",
          frames, s_audio_frames, s_audio_peak, s_w, s_h, s_pitch);
  if (s_frame) {
    FILE* f = fopen(argv[4], "wb");
    unsigned hdr[4] = { s_w, s_h, (unsigned) s_pitch, 0 };
    unsigned y;
    fwrite(hdr, sizeof(hdr), 1, f);
    for (y = 0; y < s_h; ++y) fwrite((const char*) s_frame + y * s_pitch, 4, s_w, f);
    fclose(f);
  }
  retro_unload_game();
  retro_deinit();
  return 0;
}
