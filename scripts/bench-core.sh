#!/usr/bin/env bash
# Benchmark a core on the handheld: run N frames in RetroArch with vsync and
# audio sync disabled and time it. Speed = (N/50) / seconds; 1.0 = real time.
# RetroArch's video upload still blocks on the panel's 60 Hz vsync, so this
# caps near 1.1x; the "frame avg us" lines in the log are the real measure.
#   scripts/bench-core.sh [so] [disc] [frames]
set -euo pipefail
HOST="${RG_HOST:-root@192.168.1.175}"
SO="${1:-out/arculator_libretro.so}"
DISC="${2:-/mnt/sdcard/ROMS/Archimedes/bs-django01.adf}"
N="${3:-1500}"

scp -q -O -o BatchMode=yes "$SO" "$HOST:/tmp/arcbench.so"
ssh -o BatchMode=yes "$HOST" "
  if pidof retroarch >/dev/null; then echo 'RetroArch already running on device; aborting'; exit 2; fi
  . /opt/muos/script/var/func.sh
  HOME=\"\$(GET_VAR device board/home)\"; export HOME
  SETUP_SDL_ENVIRONMENT >/dev/null 2>&1
  RA_ARGS=\$(CONFIGURE_RETROARCH)
  # muOS's launcher sets the governor; a bare retroarch would run at the idle 480 MHz.
  G=\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor); echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
  printf 'video_vsync = \"false\"\naudio_sync = \"false\"\nvideo_frame_delay = \"0\"\nfastforward_ratio = \"0.0\"\nvideo_font_enable = \"false\"\n' > /tmp/bench.cfg
  cd /tmp
  S=\$(date +%s.%N)
  timeout -s KILL 180 retroarch -f \$RA_ARGS --appendconfig=/tmp/bench.cfg --max-frames=$N -L /tmp/arcbench.so '$DISC' > /tmp/bench.log 2>&1 || true
  E=\$(date +%s.%N)
  echo \$G > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
  awk -v s=\$S -v e=\$E -v n=$N 'BEGIN{t=e-s; printf(\"frames=%d  wall=%.2fs  speed=%.2fx real time\n\", n, t, (n/50)/t)}'
  grep 'frame avg' /tmp/bench.log | tail -4
"
