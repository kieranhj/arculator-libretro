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
  active rectangle only, with the screen's own aspect ratio of 4:3.

## Not yet

- Save states (Arculator has no serialisation), disc swapping at runtime,
  hard discs and podules, and drive noise.
- Anything other than Linux hosts; the HostFS layer is the Unix one.

## Speed

Measured on the RG40XXV at 1.5 GHz, emulating one 50 Hz frame, in the
minimal host (`scripts/rt.c`). Inside RetroArch add about 2.5 ms per frame
for the RGB565 upload.

| Preset | Content | ms per 20 ms frame |
|---|---|---|
| A3000 (ARM2, 8 MHz) | RISC OS boot and desktop | 12 to 13 |
| A3000 (ARM2, 8 MHz) | Chipo Django 1 musicdisk | 19 to 20 |
| A3010/A3020 (ARM250, 12 MHz) | Chipo Django 1 | 23 to 24 |
| A5000 (ARM3, 25 MHz) | Chipo Django 1 | 49 |

So an 8 MHz ARM2 runs at roughly real time when the demo is working hard,
and a little below it inside RetroArch; the A3020 preset is about 0.85x and
the A5000 about 0.4x. Build flags that mattered: LTO gave 7%, and Zig's
default UBSan traps and stack protector cost a few percent more. There has
been no profiling or work on the interpreter itself yet.

## How it works

Arculator's emulator/UI boundary is the `arc_*` function set declared in
`arc.h`; the core (`core.c`) is the fourth implementation of it, after the
two interactive front-ends and the headless one. Like the headless build it
is single threaded: `retro_run()` advances the machine by exactly one 50 Hz
frame of CPU cycles against Arculator's 32:32 timestamp, presents whatever
the VIDC last handed to `video_renderer_present()`, and drains a sound ring.
The frontend is the only clock.

The platform backends are chosen at link time, as in Arculator's own build:

- `libretro_video.c` copies the VIDC's visible rectangle straight into a
  buffer the core owns, instead of composing into a texture.
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
real measure. `scripts/dev.py` is a tiny paramiko helper for devices whose
SFTP subsystem cannot write under `/opt`.

## Licence

GPL version 2, the same as Arculator. See `LICENSE`. Arculator is copyright
Sarah Walker; the RISC OS ROM images are Acorn's and are not included.
nspark is free software under the terms in its own `README`, which stays
intact in the submodule; zlib is under the zlib licence.
