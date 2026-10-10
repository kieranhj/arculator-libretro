#!/usr/bin/env bash
# Speed and determinism check on the handheld. Runs a fixed set of workloads
# in the minimal host (scripts/rt.c) and prints, for each, the mean
# retro_run() time once the demo is running, and a hash of every frame and
# audio sample. An optimisation that keeps the emulation identical leaves the
# hashes unchanged; compare against a run of the previous build.
#   scripts/perf-check.sh [so] [workload...]
# Workloads: desktop django allstar django250 (default: all). Needs out/rt
# (zig cc -target aarch64-linux-gnu.2.31 -O2 -o out/rt scripts/rt.c -ldl).
set -euo pipefail
HOST="${RG_HOST:-root@192.168.1.175}"
SO="${1:-out/arculator_libretro.so}"
shift || true
W="${*:-desktop django allstar django250}"
D=/mnt/sdcard/ROMS/Archimedes/Bitshifters

scp -q -O -o BatchMode=yes "$SO" "$HOST:/tmp/arc/perf.so"
scp -q -O -o BatchMode=yes out/rt "$HOST:/tmp/arc/rt"
ssh -o BatchMode=yes "$HOST" "
  if pidof retroarch >/dev/null; then echo 'RetroArch running on device; aborting'; exit 2; fi
  G=\$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)
  echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
  cd /tmp/arc; chmod +x rt
  run() {
    name=\$1; disc=\$2; frames=\$3; from=\$4; shift 4
    r=\$(RT_SYSTEM_DIR=/run/muos/storage/bios RT_HASH=1 RT_RGB565=1 RT_TIME_FROM=\$from \
         ./rt ./perf.so \"\$disc\" \$frames /tmp/arc/perf.\$name.bgra arculator_pixel_format=RGB565 \"\$@\" 2>&1 | grep -E '^(time|hash):' | tr '\n' ' ')
    printf '%-10s %s\n' \$name \"\$r\"
  }
  for w in $W; do
    case \$w in
      desktop)   run desktop - 1500 500 ;;
      django)    run django $D/bs-django01.adf 3000 1500 ;;
      allstar)   run allstar $D/bs-allstar.ddc 3000 1500 ;;
      django250) run django250 $D/bs-django01.adf 3000 1500 'arculator_machine=A3010/A3020' ;;
    esac
  done
  echo \$G > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
"
