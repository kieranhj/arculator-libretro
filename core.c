/* libretro core wrapping Arculator.
 *
 * This is the fourth implementation of Arculator's arc_* emulator/UI boundary
 * (after wx-sdl2.c, wx-mainwindow.cc and headless_main.c). Like the headless
 * front-end it is single threaded: retro_run() advances the machine by exactly
 * one 50 Hz frame of CPU cycles, presents whatever the VIDC last handed to
 * video_renderer_present(), and drains the sound ring. There is no emulation
 * thread and no main_thread_mutex, so the frontend is the only clock.
 *
 * Files the emulator expects beside its executable (roms/, cmos/, hostfs/) are
 * looked for under <system directory>/arculator/, which is what exname is set
 * to. The CMOS defaults and support ROM are seeded there on first run from
 * copies embedded at build time; ROM images are not embedded.
 */
#include "libretro.h"

#include "arc.h"
#include "arm.h"
#include "cmos.h"
#include "config.h"
#include "ddnoise.h"
#include "debugger.h"
#include "disc.h"
#include "headless.h"
#include "hostfs.h"
#include "ioc.h"
#include "keyboard.h"
#include "plat_input.h"
#include "plat_joystick.h"
#include "plat_sound.h"
#include "plat_video.h"
#include "podules.h"
#include "timer.h"
#include "vidc.h"
#include "video.h"

#include "libretro_sound.h"
#include "libretro_video.h"
#include "data_embedded.h"

#include <dirent.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
  k_sample_rate = 48000,
  k_fps = 50,
  k_audio_chunk = 4096,
  k_max_width = 1280,
  k_max_height = 1024,
};

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_log_printf_t log_cb;

static int s_running;
static char s_system_dir[1024];
static char s_disc_path[1024];
static int16_t s_audio[k_audio_chunk * 2];
static unsigned s_last_w, s_last_h;
static uint32_t s_frame_count;
static int s_log_input;

/* Per-phase timing, logged every k_stats_frames frames. */
enum { k_stats_frames = 250 };
static uint64_t s_t_emu, s_t_video, s_t_audio, s_t_total;
static uint32_t s_stats_count;

static uint64_t
time_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000000ull + (uint64_t) ts.tv_nsec / 1000ull;
}

/* ---- machine presets ---------------------------------------------------- */

struct preset {
  const char* p_name;        /* shown in the core option */
  const char* p_cfg_name;    /* machine_config_name; also the per-machine CMOS file prefix */
  const char* p_machine;     /* Arculator "machine" string */
  int cpu_type;              /* index into arc_cpus[] in main.c */
  int memc_type;             /* index into arc_memcs[] in main.c */
};

/* cpu_type: 0 ARM2, 1 ARM250, 2 ARM3 20 MHz, 3 ARM3 25 MHz, 5 ARM3 30 MHz, 6 ARM3 33 MHz.
 * memc_type: 0 MEMC1, 1 MEMC1A 8 MHz, 2 MEMC1A 12 MHz.
 */
static const struct preset s_presets[] = {
  { "A3000",       "a3000", "a3000", 0, 1 },
  { "A3010/A3020", "a3020", "a3020", 1, 2 },
  { "A5000",       "a5000", "a5000", 3, 2 },
  { "A440/1",      "a440",  "a440",  0, 1 },
  { "A310",        "a310",  "a310",  0, 0 },
};
#define k_num_presets (sizeof(s_presets) / sizeof(s_presets[0]))

/* ---- joypad to Archimedes key mapping ------------------------------------ */

enum {
  k_key_none = 0,
  k_mouse_select = 1000,
  k_mouse_menu = 1001,
  k_mouse_adjust = 1002,
};

struct key_name {
  const char* p_name;
  int key;    /* SDL scancode, or one of the k_mouse_* pseudo keys */
};

static const struct key_name s_key_names[] = {
  { "None", k_key_none },
  { "Mouse Select", k_mouse_select }, { "Mouse Menu", k_mouse_menu }, { "Mouse Adjust", k_mouse_adjust },
  { "Return", SDL_SCANCODE_RETURN }, { "Space", SDL_SCANCODE_SPACE }, { "Escape", SDL_SCANCODE_ESCAPE },
  { "Shift", SDL_SCANCODE_LSHIFT }, { "Ctrl", SDL_SCANCODE_LCTRL }, { "Alt", SDL_SCANCODE_LALT },
  { "Tab", SDL_SCANCODE_TAB }, { "Backspace", SDL_SCANCODE_BACKSPACE }, { "Delete", SDL_SCANCODE_DELETE },
  { "Copy", SDL_SCANCODE_END }, { "Insert", SDL_SCANCODE_INSERT }, { "Home", SDL_SCANCODE_HOME },
  { "Page Up", SDL_SCANCODE_PAGEUP }, { "Page Down", SDL_SCANCODE_PAGEDOWN },
  { "Cursor Up", SDL_SCANCODE_UP }, { "Cursor Down", SDL_SCANCODE_DOWN },
  { "Cursor Left", SDL_SCANCODE_LEFT }, { "Cursor Right", SDL_SCANCODE_RIGHT },
  { "F1", SDL_SCANCODE_F1 }, { "F2", SDL_SCANCODE_F2 }, { "F3", SDL_SCANCODE_F3 }, { "F4", SDL_SCANCODE_F4 },
  { "F5", SDL_SCANCODE_F5 }, { "F6", SDL_SCANCODE_F6 }, { "F7", SDL_SCANCODE_F7 }, { "F8", SDL_SCANCODE_F8 },
  { "F9", SDL_SCANCODE_F9 }, { "F10", SDL_SCANCODE_F10 }, { "F11", SDL_SCANCODE_F11 }, { "F12", SDL_SCANCODE_F12 },
  { "A", SDL_SCANCODE_A }, { "B", SDL_SCANCODE_B }, { "C", SDL_SCANCODE_C }, { "D", SDL_SCANCODE_D },
  { "E", SDL_SCANCODE_E }, { "F", SDL_SCANCODE_F }, { "G", SDL_SCANCODE_G }, { "H", SDL_SCANCODE_H },
  { "I", SDL_SCANCODE_I }, { "J", SDL_SCANCODE_J }, { "K", SDL_SCANCODE_K }, { "L", SDL_SCANCODE_L },
  { "M", SDL_SCANCODE_M }, { "N", SDL_SCANCODE_N }, { "O", SDL_SCANCODE_O }, { "P", SDL_SCANCODE_P },
  { "Q", SDL_SCANCODE_Q }, { "R", SDL_SCANCODE_R }, { "S", SDL_SCANCODE_S }, { "T", SDL_SCANCODE_T },
  { "U", SDL_SCANCODE_U }, { "V", SDL_SCANCODE_V }, { "W", SDL_SCANCODE_W }, { "X", SDL_SCANCODE_X },
  { "Y", SDL_SCANCODE_Y }, { "Z", SDL_SCANCODE_Z },
  { "1", SDL_SCANCODE_1 }, { "2", SDL_SCANCODE_2 }, { "3", SDL_SCANCODE_3 }, { "4", SDL_SCANCODE_4 },
  { "5", SDL_SCANCODE_5 }, { "6", SDL_SCANCODE_6 }, { "7", SDL_SCANCODE_7 }, { "8", SDL_SCANCODE_8 },
  { "9", SDL_SCANCODE_9 }, { "0", SDL_SCANCODE_0 },
  { "Minus", SDL_SCANCODE_MINUS }, { "Equals", SDL_SCANCODE_EQUALS },
  { "Comma", SDL_SCANCODE_COMMA }, { "Full stop", SDL_SCANCODE_PERIOD }, { "Slash", SDL_SCANCODE_SLASH },
  { "Semicolon", SDL_SCANCODE_SEMICOLON }, { "Quote", SDL_SCANCODE_APOSTROPHE },
  { "Keypad Enter", SDL_SCANCODE_KP_ENTER },
};
#define k_num_key_names (sizeof(s_key_names) / sizeof(s_key_names[0]))

struct pad_button {
  const char* p_option;
  const char* p_label;
  unsigned id;
  const char* p_default;
};

static const struct pad_button s_pad_buttons[] = {
  { "a",      "A",      RETRO_DEVICE_ID_JOYPAD_A,      "Mouse Select" },
  { "b",      "B",      RETRO_DEVICE_ID_JOYPAD_B,      "Mouse Adjust" },
  { "x",      "X",      RETRO_DEVICE_ID_JOYPAD_X,      "Mouse Menu" },
  { "y",      "Y",      RETRO_DEVICE_ID_JOYPAD_Y,      "Return" },
  { "l",      "L",      RETRO_DEVICE_ID_JOYPAD_L,      "Shift" },
  { "r",      "R",      RETRO_DEVICE_ID_JOYPAD_R,      "Ctrl" },
  { "l2",     "L2",     RETRO_DEVICE_ID_JOYPAD_L2,     "Escape" },
  { "r2",     "R2",     RETRO_DEVICE_ID_JOYPAD_R2,     "Space" },
  { "l3",     "L3",     RETRO_DEVICE_ID_JOYPAD_L3,     "None" },
  { "r3",     "R3",     RETRO_DEVICE_ID_JOYPAD_R3,     "None" },
  { "start",  "Start",  RETRO_DEVICE_ID_JOYPAD_START,  "Return" },
  { "select", "Select", RETRO_DEVICE_ID_JOYPAD_SELECT, "F12" },
};
#define k_num_pad_buttons (sizeof(s_pad_buttons) / sizeof(s_pad_buttons[0]))

enum { k_dpad_cursor = 0, k_dpad_mouse = 1 };

static int s_pad_key[k_num_pad_buttons];
static int s_pad_down[k_num_pad_buttons];
static int s_dpad_mode;
static int s_dpad_down[4];
static int s_mouse_speed;          /* 16.16 pixels per frame at full deflection */
static int32_t s_mouse_acc_x, s_mouse_acc_y;
static int s_display_mode;
static char s_option_keys[k_num_pad_buttons][32];
static char s_option_desc[k_num_pad_buttons][1024];
static char s_machine_desc[256];
static struct retro_variable s_variables[k_num_pad_buttons + 12];
static int s_rgb565;
static int s_is_archive;       /* content is a Spark archive unpacked to HostFS */

static int
key_from_name(const char* p_name) {
  size_t i;
  for (i = 0; i < k_num_key_names; ++i) {
    if (!strcmp(s_key_names[i].p_name, p_name)) return s_key_names[i].key;
  }
  return k_key_none;
}

static void
build_variables(void) {
  size_t i, j;
  char* p;
  size_t left;
  int n;
  for (i = 0; i < k_num_pad_buttons; ++i) {
    p = s_option_desc[i];
    left = sizeof(s_option_desc[i]);
    snprintf(s_option_keys[i], sizeof(s_option_keys[i]), "arculator_pad_%s", s_pad_buttons[i].p_option);
    /* libretro v0 option format: "Description; default|other|other" */
    n = snprintf(p, left, "Pad %s; %s", s_pad_buttons[i].p_label, s_pad_buttons[i].p_default);
    p += n; left -= n;
    for (j = 0; j < k_num_key_names; ++j) {
      if (!strcmp(s_key_names[j].p_name, s_pad_buttons[i].p_default)) continue;
      n = snprintf(p, left, "|%s", s_key_names[j].p_name);
      p += n; left -= n;
    }
    s_variables[i].key = s_option_keys[i];
    s_variables[i].value = s_option_desc[i];
  }
  p = s_machine_desc;
  left = sizeof(s_machine_desc);
  n = snprintf(p, left, "Machine (restart); ");
  p += n; left -= n;
  for (j = 0; j < k_num_presets; ++j) {
    n = snprintf(p, left, "%s%s", j ? "|" : "", s_presets[j].p_name);
    p += n; left -= n;
  }
  s_variables[i].key = "arculator_machine";
  s_variables[i].value = s_machine_desc;
  i++;
  s_variables[i].key = "arculator_cpu";
  s_variables[i].value = "CPU (restart); Preset|ARM2|ARM250|ARM3 (20 MHz)|ARM3 (25 MHz)|ARM3 (30 MHz)|ARM3 (33 MHz)";
  i++;
  s_variables[i].key = "arculator_memory";
  s_variables[i].value = "Memory (restart); 4 MB|1 MB|2 MB|8 MB|16 MB";
  i++;
  s_variables[i].key = "arculator_fdc";
  s_variables[i].value = "Floppy controller (restart); WD1772|82C711";
  i++;
  s_variables[i].key = "arculator_monitor";
  s_variables[i].value = "Monitor (restart); Multisync|Standard (TV)|VGA|Mono";
  i++;
  s_variables[i].key = "arculator_autoboot";
  s_variables[i].value = "Auto-run disc (restart); enabled|disabled";
  i++;
  s_variables[i].key = "arculator_pixel_format";
  s_variables[i].value = "Frame format (restart); RGB565|XRGB8888";
  i++;
  s_variables[i].key = "arculator_lowres_width";
  s_variables[i].value = "Low-res mode width (restart); Native|Doubled";
  i++;
  s_variables[i].key = "arculator_display";
  s_variables[i].value = "Display area; No borders|Native borders|TV";
  i++;
  s_variables[i].key = "arculator_dpad";
  s_variables[i].value = "D-pad; Cursor keys|Mouse";
  i++;
  s_variables[i].key = "arculator_mouse_speed";
  s_variables[i].value = "Mouse speed; Normal|Slow|Fast|Very fast";
  i++;
  s_variables[i].key = NULL;
  s_variables[i].value = NULL;
}

static const char*
get_option(const char* p_key, const char* p_default) {
  struct retro_variable var = { p_key, NULL };
  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) return var.value;
  return p_default;
}

static int
display_mode_from_option(void) {
  const char* p = get_option("arculator_display", "No borders");
  if (!strcmp(p, "Native borders")) return DISPLAY_MODE_NATIVE_BORDERS;
  if (!strcmp(p, "TV")) return DISPLAY_MODE_TV;
  return DISPLAY_MODE_NO_BORDERS;
}

/* Options that can change while running. */
static void
read_live_options(void) {
  size_t i;
  const char* p;
  for (i = 0; i < k_num_pad_buttons; ++i) {
    s_pad_key[i] = key_from_name(get_option(s_option_keys[i], s_pad_buttons[i].p_default));
  }
  s_dpad_mode = !strcmp(get_option("arculator_dpad", "Cursor keys"), "Mouse") ? k_dpad_mouse : k_dpad_cursor;
  p = get_option("arculator_mouse_speed", "Normal");
  if (!strcmp(p, "Slow")) s_mouse_speed = 3 << 16;
  else if (!strcmp(p, "Fast")) s_mouse_speed = 12 << 16;
  else if (!strcmp(p, "Very fast")) s_mouse_speed = 20 << 16;
  else s_mouse_speed = 7 << 16;
  if (s_running) {
    int mode = display_mode_from_option();
    if (mode != s_display_mode) {
      s_display_mode = mode;
      arc_set_display_mode(mode);
    }
  }
}

/* ---- host keyboard ------------------------------------------------------- */

static int
retrok_to_scancode(unsigned k) {
  if (k >= RETROK_a && k <= RETROK_z) return SDL_SCANCODE_A + (k - RETROK_a);
  if (k >= RETROK_1 && k <= RETROK_9) return SDL_SCANCODE_1 + (k - RETROK_1);
  if (k >= RETROK_F1 && k <= RETROK_F12) return SDL_SCANCODE_F1 + (k - RETROK_F1);
  if (k >= RETROK_KP1 && k <= RETROK_KP9) return SDL_SCANCODE_KP_1 + (k - RETROK_KP1);
  switch (k) {
  case RETROK_0: return SDL_SCANCODE_0;
  case RETROK_KP0: return SDL_SCANCODE_KP_0;
  case RETROK_RETURN: return SDL_SCANCODE_RETURN;
  case RETROK_ESCAPE: return SDL_SCANCODE_ESCAPE;
  case RETROK_BACKSPACE: return SDL_SCANCODE_BACKSPACE;
  case RETROK_TAB: return SDL_SCANCODE_TAB;
  case RETROK_SPACE: return SDL_SCANCODE_SPACE;
  case RETROK_MINUS: return SDL_SCANCODE_MINUS;
  case RETROK_EQUALS: return SDL_SCANCODE_EQUALS;
  case RETROK_LEFTBRACKET: return SDL_SCANCODE_LEFTBRACKET;
  case RETROK_RIGHTBRACKET: return SDL_SCANCODE_RIGHTBRACKET;
  case RETROK_BACKSLASH: return SDL_SCANCODE_BACKSLASH;
  case RETROK_SEMICOLON: return SDL_SCANCODE_SEMICOLON;
  case RETROK_QUOTE: return SDL_SCANCODE_APOSTROPHE;
  case RETROK_BACKQUOTE: return SDL_SCANCODE_GRAVE;
  case RETROK_COMMA: return SDL_SCANCODE_COMMA;
  case RETROK_PERIOD: return SDL_SCANCODE_PERIOD;
  case RETROK_SLASH: return SDL_SCANCODE_SLASH;
  case RETROK_CAPSLOCK: return SDL_SCANCODE_CAPSLOCK;
  case RETROK_PRINT: return SDL_SCANCODE_PRINTSCREEN;
  case RETROK_SCROLLOCK: return SDL_SCANCODE_SCROLLLOCK;
  case RETROK_PAUSE: return SDL_SCANCODE_PAUSE;
  case RETROK_INSERT: return SDL_SCANCODE_INSERT;
  case RETROK_HOME: return SDL_SCANCODE_HOME;
  case RETROK_PAGEUP: return SDL_SCANCODE_PAGEUP;
  case RETROK_DELETE: return SDL_SCANCODE_DELETE;
  case RETROK_END: return SDL_SCANCODE_END;
  case RETROK_PAGEDOWN: return SDL_SCANCODE_PAGEDOWN;
  case RETROK_RIGHT: return SDL_SCANCODE_RIGHT;
  case RETROK_LEFT: return SDL_SCANCODE_LEFT;
  case RETROK_DOWN: return SDL_SCANCODE_DOWN;
  case RETROK_UP: return SDL_SCANCODE_UP;
  case RETROK_NUMLOCK: return SDL_SCANCODE_NUMLOCKCLEAR;
  case RETROK_KP_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
  case RETROK_KP_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
  case RETROK_KP_MINUS: return SDL_SCANCODE_KP_MINUS;
  case RETROK_KP_PLUS: return SDL_SCANCODE_KP_PLUS;
  case RETROK_KP_ENTER: return SDL_SCANCODE_KP_ENTER;
  case RETROK_KP_PERIOD: return SDL_SCANCODE_KP_PERIOD;
  case RETROK_LCTRL: return SDL_SCANCODE_LCTRL;
  case RETROK_LSHIFT: return SDL_SCANCODE_LSHIFT;
  case RETROK_LALT: return SDL_SCANCODE_LALT;
  case RETROK_LSUPER: return SDL_SCANCODE_LGUI;
  case RETROK_RCTRL: return SDL_SCANCODE_RCTRL;
  case RETROK_RSHIFT: return SDL_SCANCODE_RSHIFT;
  case RETROK_RALT: return SDL_SCANCODE_RALT;
  case RETROK_RSUPER: return SDL_SCANCODE_RGUI;
  case RETROK_MENU: return SDL_SCANCODE_APPLICATION;
  default: return 0;
  }
}

static void
keyboard_event_cb(bool down, unsigned keycode, uint32_t character, uint16_t mods) {
  int sc = retrok_to_scancode(keycode);
  (void) character;
  (void) mods;
  if (s_log_input) log_cb(RETRO_LOG_INFO, "arculator kbd %s retrok=%u -> scancode=%d\n", down ? "down" : "up", keycode, sc);
  if (sc) headless_key_set(sc, down);
}

/* ---- arc_* boundary ------------------------------------------------------- */

void arc_start_main_thread(void* wx_window, void* wx_menu) { (void) wx_window; (void) wx_menu; }
void arc_stop_main_thread(void) {}
void arc_pause_main_thread(void) {}
void arc_resume_main_thread(void) {}

void
arc_do_reset(void) {
  debugger_start_reset();
  arc_reset();
  debugger_end_reset();
}

void
arc_disc_change(int drive, char* fn) {
  disc_close(drive);
  strcpy(discname[drive], fn);
  disc_load(drive, discname[drive]);
  ioc_discchange(drive);
}

void
arc_disc_eject(int drive) {
  ioc_discchange(drive);
  disc_close(drive);
  discname[drive][0] = 0;
}

void arc_renderer_reset(void) {}

void
arc_set_display_mode(int new_display_mode) {
  display_mode = new_display_mode;
  clearbitmap();
  setredrawall();
}

void
arc_set_dblscan(int new_dblscan) {
  dblscan = new_dblscan;
  clearbitmap();
}

void arc_set_resizeable(void) {}
void arc_enter_fullscreen(void) {}
void arc_stop_emulation(void) { s_running = 0; }

/* UI stubs the emulation core calls into. */
void updatewindowsize(int x, int y) { (void) x; (void) y; }
void arc_update_menu(void) {}
void arc_popup_menu(void) {}

void
arc_print_error(const char* format, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, format);
  vsnprintf(buf, sizeof(buf), format, ap);
  va_end(ap);
  if (log_cb) log_cb(RETRO_LOG_ERROR, "arculator: %s\n", buf);
}

void console_output(char* s) { if (log_cb) log_cb(RETRO_LOG_INFO, "%s", s); }
int console_input_get(char* s) { (void) s; return CONSOLE_INPUT_GET_ERROR_WINDOW_CLOSED; }
void console_input_enable(void) {}
void console_input_disable(void) {}

int
podule_config_file_selector(void* window_p, const char* title, const char* default_path,
                            const char* default_fn, const char* default_ext, const char* wildcard,
                            char* dest, int dest_len, int flags) {
  (void) window_p; (void) title; (void) default_path; (void) default_fn; (void) default_ext;
  (void) wildcard; (void) dest; (void) dest_len; (void) flags;
  return 0;
}
int podule_config_open(void* window_p, podule_config_t* config, const char* prefix) {
  (void) window_p; (void) config; (void) prefix; return 0;
}
void* podule_config_get_current(void* window_p, int id) { (void) window_p; (void) id; return NULL; }
void podule_config_set_current(void* window_p, int id, void* val) { (void) window_p; (void) id; (void) val; }

/* config.c reads and writes these; no joystick is ever present. */
joystick_t joystick_state[MAX_JOYSTICKS];

/* ---- system directory ----------------------------------------------------- */

static int
mkdir_p(const char* p_path) {
  char tmp[1024];
  char* p;
  snprintf(tmp, sizeof(tmp), "%s", p_path);
  for (p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
  return 0;
}

/* Write the embedded CMOS defaults and support ROM under exname if they are
 * missing. Never overwrites, so a user's edited copies survive.
 */
static void
seed_data_files(void) {
  size_t i;
  for (i = 0; i < sizeof(s_seed_files) / sizeof(s_seed_files[0]); ++i) {
    char path[1024];
    char dir[1024];
    char* p_slash;
    FILE* f;
    snprintf(path, sizeof(path), "%s%s", exname, s_seed_files[i].p_path);
    if (access(path, R_OK) == 0) continue;
    snprintf(dir, sizeof(dir), "%s", path);
    p_slash = strrchr(dir, '/');
    if (p_slash) {
      *p_slash = 0;
      mkdir_p(dir);
    }
    f = fopen(path, "wb");
    if (!f) {
      log_cb(RETRO_LOG_WARN, "arculator: cannot write %s: %s\n", path, strerror(errno));
      continue;
    }
    fwrite(s_seed_files[i].p_data, 1, s_seed_files[i].len, f);
    fclose(f);
    log_cb(RETRO_LOG_INFO, "arculator: seeded %s\n", path);
  }
}

static int
dir_exists(const char* p_path) {
  struct stat st;
  return stat(p_path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Auto-running a disc.
 *
 * RISC OS runs the configured filing system's boot file at startup when the
 * CMOS "Boot" flag is set. The core follows archi.medes.live: the boot
 * filing system is pointed at HostFS and a !Boot Obey file in the HostFS root
 * starts the desktop and Filer_Runs the first "!" object on the floppy, which
 * is the disc's !Boot if it has one and otherwise its application.
 *
 * The CMOS for this is derived at load time from the machine's normal CMOS
 * image (the user's cmos/<machine>.<set>.cmos.bin if present, else the set
 * default) by patching the bytes archi.medes.live's boot image changes:
 * chip byte 80 bit 4 is Boot, byte 69 is the boot filing system number
 * (0x99 = HostFS), and 19 and 63 follow that image. It is written to
 * cmos/<machine>-autoboot.<set>.cmos.bin and regenerated on every load, so
 * only the normal image is the user's to edit.
 */
static const char* k_boot_file_prefix = "desktop filer_run ";
static const char* k_boot_floppy = "adfs::0.$.!*";
static const char* k_boot_hostfs = "HostFS:$.Disc.!*";

/* ---- Spark / ArcFS archives ---------------------------------------------- */

/* nspark's main(), renamed at build time. Extracts into the current directory
 * with ",xxx" filetype suffixes (-T), which is HostFS's naming convention.
 * nspark is a command-line tool and calls exit() on errors and at the end of
 * main(); the build renames that to nspark_exit(), which longjmps back here
 * so a bad archive cannot take the frontend down. */
int nspark_main(int argc, char** argv);
static jmp_buf s_nspark_jmp;
static int s_nspark_active;

void
nspark_exit(int code) {
  if (s_nspark_active) longjmp(s_nspark_jmp, code + 1);
  exit(code);
}

static int
is_archive_path(const char* p_path) {
  const char* p_base = strrchr(p_path, '/');
  const char* p_ext;
  size_t n;
  p_base = p_base ? p_base + 1 : p_path;
  n = strlen(p_base);
  if (n > 4 && !strcasecmp(p_base + n - 4, ",ddc")) return 1;
  p_ext = strrchr(p_base, '.');
  if (!p_ext) return 0;
  return !strcasecmp(p_ext, ".ddc") || !strcasecmp(p_ext, ".spk") || !strcasecmp(p_ext, ".spark") || !strcasecmp(p_ext, ".arc");
}

static void
rm_rf(const char* p_dir) {
  DIR* d = opendir(p_dir);
  struct dirent* e;
  if (!d) return;
  while ((e = readdir(d)) != NULL) {
    char path[1024];
    struct stat st;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(path, sizeof(path), "%s/%s", p_dir, e->d_name);
    if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(path);
    else unlink(path);
  }
  closedir(d);
  rmdir(p_dir);
}

/* Unpack a Spark/ArcFS archive into hostfs/Disc, replacing whatever was
 * there, as archi.medes.live unpacks into the HostFS root. */
static int
unpack_archive(const char* p_path) {
  char dir[1024], cwd[1024];
  char* argv[] = { "nspark", "-x", "-T", "-q", "-f", NULL, NULL };
  char abspath[1024];
  int rc;
  snprintf(dir, sizeof(dir), "%shostfs/Disc", exname);
  rm_rf(dir);
  if (mkdir_p(dir)) {
    log_cb(RETRO_LOG_ERROR, "arculator: cannot create %s: %s\n", dir, strerror(errno));
    return -1;
  }
  if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
  if (p_path[0] != '/' && cwd[0]) snprintf(abspath, sizeof(abspath), "%s/%s", cwd, p_path);
  else snprintf(abspath, sizeof(abspath), "%s", p_path);
  argv[5] = abspath;
  if (chdir(dir)) {
    log_cb(RETRO_LOG_ERROR, "arculator: cannot enter %s: %s\n", dir, strerror(errno));
    return -1;
  }
  s_nspark_active = 1;
  rc = setjmp(s_nspark_jmp);
  if (rc == 0) rc = nspark_main(6, argv);
  else rc -= 1;
  s_nspark_active = 0;
  if (cwd[0]) chdir(cwd);
  log_cb(rc ? RETRO_LOG_ERROR : RETRO_LOG_INFO, "arculator: nspark %s -> %s (rc=%d)\n", abspath, dir, rc);
  return rc ? -1 : 0;
}

static int
prepare_autoboot(const struct preset* p_preset, int fdc, const char* p_target) {
  char src[1024], dst[1024], boot[1024];
  const char* p_set = fdc ? "riscos3_new" : "riscos3_old";
  uint8_t cmos[256];
  FILE* f;
  size_t n = 0;

  snprintf(src, sizeof(src), "%scmos/%s.%s.cmos.bin", exname, p_preset->p_cfg_name, p_set);
  f = fopen(src, "rb");
  if (!f) {
    snprintf(src, sizeof(src), "%scmos/%s/cmos.bin", exname, p_set);
    f = fopen(src, "rb");
  }
  if (f) {
    n = fread(cmos, 1, sizeof(cmos), f);
    fclose(f);
  }
  if (n != sizeof(cmos)) {
    log_cb(RETRO_LOG_WARN, "arculator: no CMOS image to derive the auto-boot image from (%s)\n", src);
    return -1;
  }
  cmos[19] = 0x10;
  cmos[63] = 0x0a;
  cmos[69] = 0x99;
  cmos[80] |= 0x10;
  snprintf(dst, sizeof(dst), "%scmos/%s-autoboot.%s.cmos.bin", exname, p_preset->p_cfg_name, p_set);
  f = fopen(dst, "wb");
  if (!f) {
    log_cb(RETRO_LOG_WARN, "arculator: cannot write %s: %s\n", dst, strerror(errno));
    return -1;
  }
  fwrite(cmos, 1, sizeof(cmos), f);
  fclose(f);

  snprintf(boot, sizeof(boot), "%shostfs", exname);
  mkdir_p(boot);
  snprintf(boot, sizeof(boot), "%shostfs/!Boot,feb", exname);
  f = fopen(boot, "wb");
  if (!f) {
    log_cb(RETRO_LOG_WARN, "arculator: cannot write %s: %s\n", boot, strerror(errno));
    return -1;
  }
  fprintf(f, "%s%s\n", k_boot_file_prefix, p_target);
  fclose(f);
  log_cb(RETRO_LOG_INFO, "arculator: auto-run: %s%s\n", k_boot_file_prefix, p_target);
  return 0;
}

/* Remove the core's own !Boot (and only that) so a later session without
 * auto-run, or a user's HostFS boot file, is not affected. */
static void
remove_autoboot_file(void) {
  char boot[1024];
  char buf[128];
  FILE* f;
  snprintf(boot, sizeof(boot), "%shostfs/!Boot,feb", exname);
  f = fopen(boot, "rb");
  if (!f) return;
  memset(buf, 0, sizeof(buf));
  fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (!strncmp(buf, k_boot_file_prefix, strlen(k_boot_file_prefix))) unlink(boot);
}

/* Build the machine configuration. A user file at configs/<preset>.cfg under
 * exname takes precedence; otherwise the preset plus the core options is
 * written to a temporary file that loadconfig() reads once.
 */
static char s_tmp_cfg[1024];

static int
write_machine_config(const struct preset* p_preset, const char* p_autoboot) {
  const char* p;
  int cpu = p_preset->cpu_type;
  int mem = 4096;
  int fdc = 0;
  const char* p_monitor = "multisync";
  FILE* f;
  int fd;

  if (!strcmp(get_option("arculator_fdc", "WD1772"), "82C711")) fdc = 1;

  snprintf(machine_config_file, sizeof(machine_config_file), "%sconfigs/%s.cfg", exname, p_preset->p_cfg_name);
  /* machine_config_name selects the per-machine CMOS image; the auto-boot
   * variant is a derived file, see prepare_autoboot(). */
  if (p_autoboot && prepare_autoboot(p_preset, fdc, p_autoboot) == 0) {
    snprintf(machine_config_name, sizeof(machine_config_name), "%s-autoboot", p_preset->p_cfg_name);
  } else {
    remove_autoboot_file();
    snprintf(machine_config_name, sizeof(machine_config_name), "%s", p_preset->p_cfg_name);
  }
  if (access(machine_config_file, R_OK) == 0) {
    log_cb(RETRO_LOG_INFO, "arculator: using machine config %s\n", machine_config_file);
    s_tmp_cfg[0] = 0;
    return 0;
  }

  p = get_option("arculator_cpu", "Preset");
  if (!strcmp(p, "ARM2")) cpu = 0;
  else if (!strcmp(p, "ARM250")) cpu = 1;
  else if (!strcmp(p, "ARM3 (20 MHz)")) cpu = 2;
  else if (!strcmp(p, "ARM3 (25 MHz)")) cpu = 3;
  else if (!strcmp(p, "ARM3 (30 MHz)")) cpu = 5;
  else if (!strcmp(p, "ARM3 (33 MHz)")) cpu = 6;
  p = get_option("arculator_memory", "4 MB");
  if (!strcmp(p, "1 MB")) mem = 1024;
  else if (!strcmp(p, "2 MB")) mem = 2048;
  else if (!strcmp(p, "8 MB")) mem = 8192;
  else if (!strcmp(p, "16 MB")) mem = 16384;
  p = get_option("arculator_monitor", "Multisync");
  if (!strcmp(p, "Standard (TV)")) p_monitor = "standard";
  else if (!strcmp(p, "VGA")) p_monitor = "vga";
  else if (!strcmp(p, "Mono")) p_monitor = "mono";

  snprintf(s_tmp_cfg, sizeof(s_tmp_cfg), "%s/arculator-libretro-XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
  fd = mkstemp(s_tmp_cfg);
  if (fd < 0) {
    /* No writable temp directory: fall back to the system directory. */
    snprintf(s_tmp_cfg, sizeof(s_tmp_cfg), "%sconfigs/_libretro.cfg", exname);
    mkdir_p(exname);
    f = fopen(s_tmp_cfg, "wt");
  } else {
    f = fdopen(fd, "wt");
  }
  if (!f) {
    log_cb(RETRO_LOG_ERROR, "arculator: cannot write machine config: %s\n", strerror(errno));
    return -1;
  }
  fprintf(f,
          "machine = %s\n"
          "mem_size = %d\n"
          "cpu_type = %d\n"
          "memc_type = %d\n"
          "fpa = 0\n"
          "fpu_type = 0\n"
          "display_mode = %d\n"
          "double_scan = 1\n"
          "video_scale = 1\n"
          "fdc_type = %d\n"
          "st506_present = 0\n"
          "rom_set = riscos311\n"
          "monitor_type = %s\n"
          "joystick_if = none\n"
          "unique_id = 811004001\n"
          "renderer_driver = Auto\n"
          "support_rom_enabled = 1\n",
          p_preset->p_machine, mem, cpu, p_preset->memc_type, s_display_mode, fdc, p_monitor);
  fclose(f);
  snprintf(machine_config_file, sizeof(machine_config_file), "%s", s_tmp_cfg);
  log_cb(RETRO_LOG_INFO, "arculator: machine %s cpu_type=%d memc_type=%d mem=%dK fdc=%s monitor=%s\n",
         p_preset->p_name, cpu, p_preset->memc_type, mem, fdc ? "82C711" : "WD1772", p_monitor);
  return 0;
}

/* ---- libretro API ---------------------------------------------------------- */

static void
fallback_log(enum retro_log_level level, const char* fmt, ...) {
  va_list va;
  (void) level;
  va_start(va, fmt);
  vfprintf(stderr, fmt, va);
  va_end(va);
}

void
retro_set_environment(retro_environment_t cb) {
  struct retro_log_callback logging;
  bool no_game = true;
  environ_cb = cb;
  build_variables();
  cb(RETRO_ENVIRONMENT_SET_VARIABLES, s_variables);
  cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
  if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) log_cb = logging.log;
  else log_cb = fallback_log;
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void) cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_init(void) {}
void retro_deinit(void) {}
unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void
retro_get_system_info(struct retro_system_info* info) {
  memset(info, 0, sizeof(*info));
  info->library_name = "arculator";
  info->library_version = "0.1";
  info->valid_extensions = "adf|adl|apd|fdi|hfe|jfd|scp|ssd|dsd|ddc|spk|spark|arc";
  info->need_fullpath = true;
  info->block_extract = true;
}

void
retro_get_system_av_info(struct retro_system_av_info* info) {
  memset(info, 0, sizeof(*info));
  info->geometry.base_width = s_last_w ? s_last_w : 640;
  info->geometry.base_height = s_last_h ? s_last_h : 256;
  info->geometry.max_width = k_max_width;
  info->geometry.max_height = k_max_height;
  info->geometry.aspect_ratio = 4.0f / 3.0f;
  info->timing.fps = (double) k_fps;
  info->timing.sample_rate = (double) k_sample_rate;
}

void retro_set_controller_port_device(unsigned port, unsigned device) { (void) port; (void) device; }

bool
retro_load_game(const struct retro_game_info* info) {
  enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
  struct retro_keyboard_callback kcb = { keyboard_event_cb };
  const char* p_sys = NULL;
  const struct preset* p_preset = &s_presets[0];
  const char* p_name;
  size_t i;
  char path[1024];

  /* RGB565 halves the per-frame upload, which on the RG40XXV is the biggest
   * cost outside the emulation itself; the conversion is a fraction of it. */
  s_rgb565 = !strcmp(get_option("arculator_pixel_format", "RGB565"), "RGB565");
  if (s_rgb565) {
    fmt = RETRO_PIXEL_FORMAT_RGB565;
    if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
      log_cb(RETRO_LOG_WARN, "arculator: RGB565 not available, using XRGB8888\n");
      s_rgb565 = 0;
      fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    }
  }
  if (!s_rgb565 && !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
    log_cb(RETRO_LOG_ERROR, "XRGB8888 not supported\n");
    return false;
  }
  environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &kcb);
  if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &p_sys) || !p_sys || !*p_sys) {
    log_cb(RETRO_LOG_ERROR, "arculator: no system directory; ROMs must live in <system>/arculator/roms/<set>/\n");
    return false;
  }
  snprintf(s_system_dir, sizeof(s_system_dir), "%s", p_sys);
  snprintf(exname, sizeof(exname), "%s/arculator/", p_sys);
  s_log_input = getenv("ARCULATOR_LOG_INPUT") != NULL;

  s_display_mode = display_mode_from_option();
  read_live_options();
  p_name = get_option("arculator_machine", s_presets[0].p_name);
  for (i = 0; i < k_num_presets; ++i) {
    if (!strcmp(s_presets[i].p_name, p_name)) p_preset = &s_presets[i];
  }

  /* Report a missing ROM set before Arculator tries to chdir into it. */
  snprintf(path, sizeof(path), "%sroms/riscos311", exname);
  if (!dir_exists(path)) {
    char msg[1200];
    struct retro_message rmsg;
    snprintf(msg, sizeof(msg), "RISC OS 3.11 ROMs not found in %s", path);
    log_cb(RETRO_LOG_ERROR, "arculator: %s\n", msg);
    rmsg.msg = msg;
    rmsg.frames = 600;
    environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &rmsg);
    return false;
  }

  mkdir_p(exname);
  seed_data_files();

  /* Never rewrite arc.cfg, the machine .cfg or the CMOS image. */
  config_persist_enabled = 0;

  {
    const char* p_autoboot = NULL;
    int has_content = info && info->path && *info->path;
    s_is_archive = has_content && is_archive_path(info->path);
    if (s_is_archive && unpack_archive(info->path)) return false;
    if (has_content && !strcmp(get_option("arculator_autoboot", "enabled"), "enabled")) {
      p_autoboot = s_is_archive ? k_boot_hostfs : k_boot_floppy;
    }
    if (write_machine_config(p_preset, p_autoboot)) return false;
  }

  podule_build_list();
  if (arc_init()) {
    log_cb(RETRO_LOG_ERROR, "arculator: arc_init failed (ROM set under %sroms/riscos311 unreadable?)\n", exname);
    if (s_tmp_cfg[0]) unlink(s_tmp_cfg);
    return false;
  }
  if (s_tmp_cfg[0]) {
    unlink(s_tmp_cfg);
    s_tmp_cfg[0] = 0;
  }
  libretro_video_set_format(s_rgb565, !strcmp(get_option("arculator_lowres_width", "Native"), "Native"));
  if (!video_renderer_init(NULL)) {
    log_cb(RETRO_LOG_ERROR, "arculator: video init failed\n");
    return false;
  }
  input_init();
  libretro_sound_reset();

  /* The disc goes in before any cycles run, so an 82C711 machine never sees
   * an empty drive at boot (see CLAUDE.md in the Arculator tree). */
  s_disc_path[0] = 0;
  if (info && info->path && *info->path && !s_is_archive) {
    snprintf(s_disc_path, sizeof(s_disc_path), "%s", info->path);
    arc_disc_change(0, s_disc_path);
    log_cb(RETRO_LOG_INFO, "arculator: disc 0 = %s\n", s_disc_path);
  }

  s_last_w = s_last_h = 0;
  s_frame_count = 0;
  s_mouse_acc_x = s_mouse_acc_y = 0;
  memset(s_pad_down, 0, sizeof(s_pad_down));
  memset(s_dpad_down, 0, sizeof(s_dpad_down));
  s_running = 1;
  log_cb(RETRO_LOG_INFO, "arculator: running, speed_mhz=%d cycles/frame=%d\n", speed_mhz, (speed_mhz * 1000000) / k_fps);
  return true;
}

void
retro_unload_game(void) {
  int i;
  if (!s_running) return;
  s_running = 0;
  /* Debug aid: ARCULATOR_SAVE_CMOS=1 writes the CMOS image back under
   * cmos/<machine>.<set>.cmos.bin, e.g. to capture *Configure changes. */
  if (getenv("ARCULATOR_SAVE_CMOS")) {
    config_persist_enabled = 1;
    cmos_save();
    config_persist_enabled = 0;
    log_cb(RETRO_LOG_INFO, "arculator: saved CMOS\n");
  }
  /* arc_close() minus dumpregs(), which writes modules.dmp into the current
   * directory, and minus the config/CMOS saves, which are disabled anyway. */
  podules_close();
  for (i = 0; i < 4; ++i) disc_close(i);
  ddnoise_close();
  sound_dev_close();
  closevideo();
  input_close();
  video_renderer_close();
}

bool
retro_load_game_special(unsigned type, const struct retro_game_info* info, size_t num) {
  (void) type; (void) info; (void) num;
  return false;
}

/* ---- per-frame input ---------------------------------------------------------- */

static void
poll_input(void) {
  size_t i;
  int mouse_bits = 0;
  int dx = 0, dy = 0;
  int16_t ax, ay;
  static const unsigned dpad_ids[4] = { RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
                                        RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT };
  static const int dpad_keys[4] = { SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT };

  input_poll_cb();

  for (i = 0; i < k_num_pad_buttons; ++i) {
    int key = s_pad_key[i];
    int down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, s_pad_buttons[i].id) ? 1 : 0;
    if (down != s_pad_down[i]) {
      if (s_log_input) log_cb(RETRO_LOG_INFO, "arculator pad %s %s -> key=%d\n", s_pad_buttons[i].p_label, down ? "down" : "up", key);
      if (key > k_key_none && key < k_mouse_select) headless_key_set(key, down);
      s_pad_down[i] = down;
    }
    if (down) {
      if (key == k_mouse_select) mouse_bits |= 1;
      else if (key == k_mouse_adjust) mouse_bits |= 2;
      else if (key == k_mouse_menu) mouse_bits |= 4;
    }
  }

  for (i = 0; i < 4; ++i) {
    int down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, dpad_ids[i]) ? 1 : 0;
    if (s_dpad_mode == k_dpad_cursor) {
      if (down != s_dpad_down[i]) headless_key_set(dpad_keys[i], down);
    } else {
      if (s_dpad_down[i]) headless_key_set(dpad_keys[i], 0);   /* mode just switched */
      if (down) {
        int step = s_mouse_speed * 3 / 4;
        if (i == 0) dy -= step; else if (i == 1) dy += step; else if (i == 2) dx -= step; else dx += step;
      }
      down = 0;
    }
    s_dpad_down[i] = down;
  }

  ax = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
  ay = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
  if (ax > 4096 || ax < -4096) dx += (int) (((int64_t) ax * s_mouse_speed) >> 15);
  if (ay > 4096 || ay < -4096) dy += (int) (((int64_t) ay * s_mouse_speed) >> 15);

  /* Whole pixels only; keep the fraction for next frame. */
  s_mouse_acc_x += dx;
  s_mouse_acc_y += dy;
  dx = s_mouse_acc_x >> 16;
  dy = s_mouse_acc_y >> 16;
  s_mouse_acc_x -= dx << 16;
  s_mouse_acc_y -= dy << 16;

  /* A real mouse, if the frontend has one, is passed straight through. */
  dx += input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
  dy += input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
  if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT)) mouse_bits |= 1;
  if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT)) mouse_bits |= 2;
  if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_MIDDLE)) mouse_bits |= 4;

  if (dx || dy) headless_mouse_move(dx, dy);
  headless_mouse_buttons(mouse_bits);
}

/* ---- the frame --------------------------------------------------------------- */

static int s_ddnoise_slices;

static void
run_frame(void) {
  /* One 50 Hz frame of CPU cycles, in 10 ms slices as the interactive
   * front-ends do, measured against the 32:32 timestamp so rounding never
   * accumulates. */
  int64_t slice = (speed_mhz * 1000000ll) / 100;
  uint64_t target = tsc + (((uint64_t) speed_mhz * 1000000ull / k_fps) << 32);
  if (slice < 1) slice = 1;
  while (tsc < target) {
    int64_t left = (int64_t) ((target - tsc) >> 32);
    int n = (int) (left < slice ? left : slice);
    if (n < 1) n = 1;
    joystick_poll_host();
    mouse_poll_host();
    keyboard_poll_host();
    execarm(n);
    /* ddnoise_mix() is not just the drive noise: for a seek of two tracks
     * or more, it is what re-arms the floppy controller's timer when the
     * seek sample finishes, so the WD1772 stays busy forever without it.
     * arc_run() calls it every ten 10 ms slices; so do we. */
    if (++s_ddnoise_slices >= 10) {
      s_ddnoise_slices = 0;
      ddnoise_mix();
    }
  }
}

static void
drain_audio(void) {
  for (;;) {
    size_t n = libretro_sound_read(s_audio, k_audio_chunk);
    if (n == 0) break;
    audio_batch_cb(s_audio, n);
  }
}

static void
present(void) {
  unsigned w, h;
  if (!libretro_frame.valid) {
    video_cb(NULL, s_last_w ? s_last_w : 640, s_last_h ? s_last_h : 256, 0);
    return;
  }
  w = (unsigned) libretro_frame.w;
  h = (unsigned) libretro_frame.h;
  if (w > k_max_width) w = k_max_width;
  if (h > k_max_height) h = k_max_height;
  if (w != s_last_w || h != s_last_h) {
    struct retro_game_geometry geom;
    geom.base_width = w;
    geom.base_height = h;
    geom.max_width = k_max_width;
    geom.max_height = k_max_height;
    geom.aspect_ratio = 4.0f / 3.0f;
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
    log_cb(RETRO_LOG_INFO, "arculator: geometry %ux%u dblscan=%d\n", w, h, libretro_frame.dblscan);
    s_last_w = w;
    s_last_h = h;
  }
  {
    /* Debug aid: ARCULATOR_DUMP_FRAME=<path> writes the frame given to the
     * frontend at frame ARCULATOR_DUMP_AT (default 500), same format as rt.c.
     * With ARCULATOR_DUMP_EVERY=<n> it keeps dumping every n frames from
     * then on, to <path>.<frame>. */
    static int dumped;
    const char* p_dump = getenv("ARCULATOR_DUMP_FRAME");
    if (p_dump && !dumped) {
      const char* p_at = getenv("ARCULATOR_DUMP_AT");
      const char* p_every = getenv("ARCULATOR_DUMP_EVERY");
      uint32_t at = p_at ? (uint32_t) atoi(p_at) : 500;
      uint32_t every = p_every ? (uint32_t) atoi(p_every) : 0;
      if (s_frame_count >= at && (every == 0 || ((s_frame_count - at) % every) == 0)) {
        char name[1200];
        FILE* f;
        if (every) snprintf(name, sizeof(name), "%s.%u", p_dump, s_frame_count);
        else snprintf(name, sizeof(name), "%s", p_dump);
        f = fopen(name, "wb");
        if (f) {
          unsigned hdr[4] = { w, h, w * 4, 0 };
          unsigned y, x;
          fwrite(hdr, sizeof(hdr), 1, f);
          for (y = 0; y < h; ++y) {
            const uint8_t* row = (const uint8_t*) libretro_frame.p_pixels + y * libretro_frame.pitch;
            if (libretro_frame.bpp == 32) {
              fwrite(row, 4, w, f);
            } else {
              /* Expand RGB565 back to XRGB8888 so the dump format is one. */
              for (x = 0; x < w; ++x) {
                uint32_t p = ((const uint16_t*) row)[x];
                uint32_t v = ((p & 0xf800) << 8) | ((p & 0x07e0) << 5) | ((p & 0x001f) << 3);
                fwrite(&v, 4, 1, f);
              }
            }
          }
          fclose(f);
          log_cb(RETRO_LOG_INFO, "arculator: dumped frame %u (%ux%u) to %s\n", s_frame_count, w, h, name);
        }
        if (!every) dumped = 1;
      }
    }
  }
  video_cb(libretro_frame.p_pixels, w, h, libretro_frame.pitch);
}

void
retro_run(void) {
  bool updated = false;
  uint64_t t0, t1, t2, t3;

  if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) read_live_options();

  if (!s_running) {
    video_cb(NULL, s_last_w ? s_last_w : 640, s_last_h ? s_last_h : 256, 0);
    return;
  }

  s_frame_count++;
  poll_input();

  t0 = time_us();
  run_frame();
  t1 = time_us();
  present();
  t2 = time_us();
  drain_audio();
  t3 = time_us();

  s_t_emu += (t1 - t0);
  s_t_video += (t2 - t1);
  s_t_audio += (t3 - t2);
  s_t_total += (t3 - t0);
  if (++s_stats_count == k_stats_frames) {
    log_cb(RETRO_LOG_INFO, "arculator frame avg us: emu %u video_cb %u audio %u total %u pc=%08X\n",
           (unsigned) (s_t_emu / k_stats_frames), (unsigned) (s_t_video / k_stats_frames),
           (unsigned) (s_t_audio / k_stats_frames), (unsigned) (s_t_total / k_stats_frames), PC);
    s_t_emu = s_t_video = s_t_audio = s_t_total = 0;
    s_stats_count = 0;
  }
}

void
retro_reset(void) {
  if (!s_running) return;
  arc_do_reset();
  libretro_sound_reset();
}

size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void* data, size_t size) { (void) data; (void) size; return false; }
bool retro_unserialize(const void* data, size_t size) { (void) data; (void) size; return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned index, bool enabled, const char* code) { (void) index; (void) enabled; (void) code; }
unsigned retro_get_region(void) { return RETRO_REGION_PAL; }
void* retro_get_memory_data(unsigned id) { (void) id; return NULL; }
size_t retro_get_memory_size(unsigned id) { (void) id; return 0; }
