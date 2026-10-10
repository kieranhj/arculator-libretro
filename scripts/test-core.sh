#!/usr/bin/env bash
# Copy a core to the handheld as /tmp/arctest.so and run it in RetroArch for N
# frames headlessly, saving a screenshot of the last frame. Prints whether it
# survived and its CPU use (500 = one full core). Does not touch the installed
# core. Refuses to run if RetroArch is already running (someone is using it).
#   scripts/test-core.sh [so] [disc] [frames]
# Fetches the screenshot to out/ra.png.
set -euo pipefail
HOST="${RG_HOST:-root@192.168.1.175}"
SO="${1:-out/arculator_libretro.so}"
DISC="${2:-/mnt/sdcard/ROMS/Archimedes/bs-django01.adf}"
N="${3:-1500}"

scp -O -q -o BatchMode=yes "$SO" "$HOST:/tmp/arctest.so"
ssh -o BatchMode=yes "$HOST" "
  if pidof retroarch >/dev/null; then echo 'RetroArch already running on device; aborting'; exit 2; fi
  . /opt/muos/script/var/func.sh
  HOME=\"\$(GET_VAR device board/home)\"; export HOME
  SETUP_SDL_ENVIRONMENT >/dev/null 2>&1
  RA_ARGS=\$(CONFIGURE_RETROARCH)
  # muOS's launcher sets the governor; a bare retroarch would run at the idle 480 MHz.
  G=\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor); echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
  cd /tmp
  rm -f /tmp/ra.png
  (timeout -s KILL 90 retroarch -v -f \$RA_ARGS --max-frames=$N --max-frames-ss --max-frames-ss-path=/tmp/ra.png -L /tmp/arctest.so '$DISC' > /tmp/arctest.log 2>&1 &)
  sleep 10
  P=\$(pidof retroarch || true)
  if [ -z \"\$P\" ]; then
    echo 'RESULT: died within 10 s'
  else
    a=\$(awk '{print \$14+\$15}' /proc/\$P/stat); sleep 5; b=\$(awk '{print \$14+\$15}' /proc/\$P/stat)
    echo \"RESULT: alive, cpu ticks/5s=\$((b-a)) (500 = one full core)\"
  fi
  while pidof retroarch >/dev/null; do sleep 1; done
  echo \$G > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
  grep -n 'arculator\|Segm\|Geometry\|Set audio input rate' /tmp/arctest.log | grep -v 'geometry 640x2[0-9][0-9] dbl' | head -30
  ls -la /tmp/ra.png 2>&1
"
mkdir -p out
scp -O -q -o BatchMode=yes "$HOST:/tmp/ra.png" out/ra.png && echo "screenshot: out/ra.png"
