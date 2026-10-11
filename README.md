# arculator-libretro

A [libretro](https://www.libretro.com/) core that wraps Sarah Walker's
[Arculator](https://github.com/sarah-walker-pcem/arculator), an Acorn
Archimedes emulator, so Archimedes discs run in RetroArch. The first target
is the Anbernic RG40XXV (Allwinner H700, quad Cortex-A53 at 1.5 GHz) running
muOS.

This core was written with [Claude Code](https://claude.com/claude-code),
Anthropic's agentic coding tool, working from Kieran Connell's direction. The
emulator itself is entirely Arculator, pulled in unmodified as a git
submodule; this repository only adds the libretro glue.

## What works

- RISC OS 3.11 boots to the desktop. Machine presets for the A3000,
  A3010/A3020, A5000, A440/1 and A310, with the CPU, memory size, floppy
  controller and monitor type as separate core options.
- Disc images in any format Arculator reads: `.adf`, `.adl`, `.apd`,
  `.fdi`, `.hfe`, `.jfd`, `.scp`, `.ssd`, `.dsd`.
- Spark and ArcFS archives (`.ddc`, `.spk`, `.arc`, or a RISC OS style
  `name,ddc`), the form most Archimedes software is distributed in. The
  archive is unpacked with nspark into `hostfs/Disc` and run from HostFS,
  again as archi.medes.live does. Frontends pick content by extension, so
  name a `,ddc` download `name.ddc`.
- Auto-running a disc. RISC OS is configured to boot from HostFS, where the
  core writes a `!Boot` Obey file that starts the desktop and Filer_Runs the
  first `!` object on the floppy, which is the disc's `!Boot` if it has one
  and otherwise its application. This is how archi.medes.live does it. Turn
  it off with the "Auto-run disc" option to get the desktop.
- Mouse on the left analogue stick, with A, B and X as Select, Adjust and
  Menu. A real mouse on the host is passed straight through. Keyboard
  passthrough for host keyboards, and every RetroPad button mappable to an
  Archimedes key from the core options. The D-pad is the cursor keys or, by
  option, the mouse.
- Sound at 48 kHz stereo, produced in lockstep with the emulation.
- RGB565 or XRGB8888 output. The frame handed to the frontend is the VIDC's
  active rectangle only. Its aspect ratio follows the screen mode, the way
  RISC OS sees it: 320x256 and 640x256 are 5:4, 640x480 is 4:3, and a demo's
  320x180 mode is 16:9. The "Aspect ratio" option set to 4:3 stretches every
  mode to fill the monitor instead. The frontend has to be set to use the
  core's aspect ratio (RetroArch's "Core provided").

## Not yet

- Save states (Arculator has no serialisation), disc swapping at runtime,
  hard discs and podules, and drive noise.
- Anything other than Linux hosts; the HostFS layer is the Unix one.

## Speed

Time to emulate one 50 Hz frame (budget 20 ms) on the RG40XXV at 1.5 GHz,
measured in the minimal host (`scripts/perf-check.sh`), over 1000 frames
once each demo is running. "Before" is the unmodified emulator; "after" is
the `libretro-perf` branch of the Arculator submodule.

| Content | A3000 (ARM2, 8 MHz) | A3010 (ARM250, 12 MHz) | A5000 (ARM3, 25 MHz) |
|---|---|---|---|
| RISC OS desktop | 11.6 → 9.0 | | |
| Chipo Django 1 | 19.4 → 10.0 | 23.8 → 13.5 | |
| Tipsy Cube | 16.6 → 11.1 | 21.7 → 15.3 | 41.2 → 31.9 |
| Push | 14.1 → 10.7 | 18.5 → 14.0 | 39.8 → 32.8 |
| Mikroreise | 13.1 → 10.1 | 18.2 → 13.9 | 40.8 → 33.3 |
| Horizon | 13.0 → 10.0 | 15.7 → 12.3 | 39.1 → 32.3 |

Inside RetroArch the emulation time is the same, and the frame's upload adds
1.3 ms. So the A3000 and A3010 presets run at full speed with room to spare;
the A5000 runs at about 0.6x. Every frame and audio sample of all of these
was checked to be identical before and after.

What made the difference, found with the sampling profiler in
`scripts/rt.c` (`RT_PROFILE`) and `scripts/profile.py`:

- Palette writes rebuilt all 276 host colours, with three divisions each.
  Demos that change the palette per scanline spent a third of their time
  there. Now a lookup table, and only the entries that depend on the
  register written are recomputed.
- The VIDC's bitmap rows were 16 KB apart. At a power-of-two pitch every row
  maps to the same cache sets on the A53, so the frame never stayed in
  cache; reading it back at frame end cost 1.5 ms. The rows are now padded,
  and the core converts each scanline to RGB565 just after it is drawn.
- The memory timing model's fetch path was an out-of-line call per
  instruction, and so was the "is any DMA due" test. Both are now inline for
  the no-cache case.
- IOC timers 2 and 3, which only clock the serial and keyboard baud rates,
  were re-armed every period: every 2 us at the keyboard's baud rate. They
  are now computed when read.

Build flags that mattered: LTO gave 7%, and Zig's default UBSan traps and
stack protector cost a few percent more. The remaining time is mostly the
interpreter loop itself and the cycle-accurate memory model; the A5000
would need a different execution model to reach full speed.

## How it works

Arculator's emulator/UI boundary is the `arc_*` function set declared in
`arc.h`; the core (`core.c`) is the fourth implementation of it, after the
two interactive front-ends and the headless one. Like the headless build it
is single threaded: `retro_run()` advances the machine by exactly one 50 Hz
frame of CPU cycles against Arculator's 32:32 timestamp, presents whatever
the VIDC last handed to `video_renderer_present()`, and drains a sound ring.
The frontend is the only clock.

The platform backends are chosen at link time, as in Arculator's own build:

- `libretro_video.c` converts each scanline to RGB565 as the VIDC finishes
  drawing it (through `vidc_line_callback`), and at frame end copies the
  visible rectangle into a buffer the core owns, instead of composing into a
  texture.
- `libretro_sound.c` is a ring buffer behind `sound_givebuffer()`.
- Arculator's own `headless_input.c` provides `key[]` (indexed by SDL
  scancode) and the mouse; `platform/SDL.h` is a stand-in header holding only
  the scancode values, so the emulator compiles without SDL.

Files Arculator expects beside its executable live under
`<system>/arculator/`: `roms/<set>/` for the ROM images (not distributed),
and `cmos/`, `hostfs/` and `ddnoise/` which the core seeds from copies
embedded at build time (`gen_data.py`). Sessions never write the config or
CMOS files back, so every run starts the same.

Three things worth knowing if you touch the core:

- `ddnoise_mix()` is not just drive noise. For a seek of two tracks or more
  it is what re-arms the floppy controller's timer when the seek sample
  ends, so a front-end that never calls it leaves the WD1772 busy forever
  and anything loading from floppy hangs. The core calls it every 100 ms of
  emulated time, and ships the WAV samples it indexes.
- `rom_load_arc_support_extrom()` does not check that `roms/arcrom_ext`
  opened, hence seeding it.
- `arc_close()` calls `dumpregs()`, which writes `modules.dmp` into the
  current directory; the core unloads with the useful subset instead.

Debug aids, all read from the environment: `ARCULATOR_LOG_INPUT=1` traces
pad and key events; `ARCULATOR_DUMP_FRAME=<path>` writes one frame at
`ARCULATOR_DUMP_AT`, or every `ARCULATOR_DUMP_EVERY` frames from then on;
`ARCULATOR_SAVE_CMOS=1` writes the CMOS back on unload. `scripts/rt.c` is a
minimal libretro host for running the core headlessly on the device, with
`RT_KEYS` for scripted keystrokes (`scripts/rt-keys.py` builds them) and
`RT_SYSTEM_DIR` for the system directory; `scripts/bgra2png.py` converts its
frames.

## Building

Native Linux build:

    git clone --recursive https://github.com/kieranhj/arculator-libretro
    cd arculator-libretro
    make

Cross-build for an aarch64 handheld from any host with
[Zig](https://ziglang.org/) and GNU make on the PATH, no cross toolchain or
sysroot needed:

    scripts/build-aarch64.sh clean

The output is `out/arculator_libretro.so` plus the `.info` file. See
`device/muos/README.md` for installing on muOS. zlib is bundled as a
submodule for the compressed `.apd` and `.jfd` formats, and
[nspark](https://github.com/pdjstone/nspark-wasm) (the fork archi.medes.live
uses) for Spark archives. nspark is a command-line tool; its `main()` is
renamed at build time and its `exit()` calls are routed back to the core, so
a broken archive cannot take the frontend down.

`scripts/test-core.sh` and `scripts/bench-core.sh` run a core in RetroArch on
the device over SSH, grab a screenshot, and report CPU use and speed.
RetroArch's video upload blocks on the panel's 60 Hz vsync, so the wall-clock
benchmark caps near 1.1x; the "frame avg us" lines the core logs are the
real measure. `scripts/perf-check.sh` runs a fixed set of workloads in the
minimal host and prints each one's time per frame and a hash of every frame
and audio sample: an optimisation that leaves the hashes unchanged has not
changed the emulation. `scripts/dev.py` is a tiny paramiko helper for devices whose
SFTP subsystem cannot write under `/opt`.

## Licence

GPL version 2, the same as Arculator. See `LICENSE`. Arculator is copyright
Sarah Walker; the RISC OS ROM images are Acorn's and are not included.
nspark is free software under the terms in its own `README`, which stays
intact in the submodule; zlib is under the zlib licence.
