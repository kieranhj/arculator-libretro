# Installing on muOS (Anbernic RG40XXV and friends)

Paths below are for muOS "NeXT" (2025). Copy with `scp -O` over SSH (user
`root`); the SFTP subsystem on this build cannot write under `/opt`.

1. Core and info file:

       scp -O out/arculator_libretro.so root@DEVICE:/opt/muos/share/core/
       scp -O arculator_libretro.info   root@DEVICE:/opt/muos/share/emulator/retroarch/info/

   If RetroArch is running, copy to `/tmp` first and `mv` into place so the
   running process keeps its old mapping.

2. ROM images. muOS's RetroArch system directory is
   `/run/muos/storage/bios`, so put the RISC OS 3.11 ROM there as
   `/run/muos/storage/bios/arculator/roms/riscos311/` (one file, usually
   called `ros311`, 2 MB). The core creates everything else it needs under
   `bios/arculator/` on first run.

3. Tell muOS about the system. Create
   `/opt/muos/share/info/assign/Acorn Archimedes/` containing `global.ini`
   and `arculator.ini` from this folder.

4. Put disc images in `ROMS/Archimedes` on the SD card. Any folder name
   listed under `[friendly]` in `global.ini` (arc, archimedes, riscos,
   acornarchimedes) works. Spark archives work too; rename a `name,ddc`
   download to `name.ddc` so muOS lists it.

Launching a disc boots RISC OS 3.11 and runs the disc's `!Boot`, or its
application if it has none, after about 20 seconds. Turn this off with the
"Auto-run disc" core option to land on the desktop instead.

Default pad mapping (change in RetroArch core options):

| Pad | Archimedes |
|---|---|
| Left stick | Mouse |
| A / B / X | Mouse Select / Adjust / Menu |
| Y | Return |
| D-pad | Cursor keys (or the mouse, with the "D-pad" option) |
| L / R | Shift / Ctrl |
| L2 / R2 | Escape / Space |
| Start / Select | Return / F12 |

Select (F12) opens the RISC OS command line on the desktop; Return on an
empty line closes it.

muOS uses MENU as the RetroArch hotkey modifier on the RG40XXV (from
`/opt/muos/device/control/retroarch.device.cfg`): MENU plus X opens the
RetroArch quick menu, MENU plus START quits to muOS, MENU plus Y toggles the
FPS counter.

The machine preset, CPU, memory and other options are under Core Options in
the quick menu; the ones marked "(restart)" take effect when the content is
restarted. Save them as a game option for a disc that needs an ARM3 or the
82C711 floppy controller.
