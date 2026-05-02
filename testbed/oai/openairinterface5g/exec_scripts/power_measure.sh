#!/usr/bin/env bash
set -euo pipefail

INTERVAL=1.0
CSV=0
GLOB="/sys/class/powercap/intel-rapl:*/energy_uj"

usage() {
  cat <<USAGE
Usage: $0 [-i SEC] [-g GLOB] [-c]
  -i SEC   sampling interval seconds (default: 1.0)
  -g GLOB  energy_uj file glob (default: /sys/class/powercap/intel-rapl:*/energy_uj)
  -c       CSV output (timestamp,energy_J_total,power_W_instant,mean_W,std_W)
Ctrl+C to stop -> prints samples, duration, total energy (J), mean W, std W, min/max W.
USAGE
}

while getopts ":i:g:ch" opt; do
  case "$opt" in
    i) INTERVAL="$OPTARG" ;;
    g) GLOB="$OPTARG" ;;
    c) CSV=1 ;;
    h|*) usage; exit 0 ;;
  esac
done

# collect energy files
mapfile -t ENERGY_FILES < <(compgen -G "$GLOB" || true)
if [ ${#ENERGY_FILES[@]} -eq 0 ]; then
  echo "No RAPL energy_uj files found for glob: $GLOB" >&2
  exit 1
fi

# map corresponding max range files
declare -A MAXR
for f in "${ENERGY_FILES[@]}"; do
  d="$(dirname "$f")"
  if [ -r "$d/max_energy_range_uj" ]; then
    MAXR["$f"]="$(<"$d/max_energy_range_uj")"
  else
    MAXR["$f"]=""
  fi
done

# read all energies (µJ) sum
read_sum_uJ() {
  local sum=0 val
  for f in "${ENERGY_FILES[@]}"; do
    if ! read -r val < "$f"; then continue; fi
    sum=$(( sum + val ))
  done
  echo "$sum"
}

# sleep with fractional seconds
fsleep() { python3 - <<PY "$INTERVAL"
import time; time.sleep(float("$1"))
PY
}

SAMPLES=0
SUM_P=0.0
SUMSQ_P=0.0
MIN_P=""
MAX_P=""
START_T=$(date +%s.%N)

# initial read
PREV_T=$(date +%s.%N)
PREV_SUM_UJ=$(read_sum_uJ)

# header
if [ "$CSV" -eq 1 ]; then
  echo "timestamp,energy_J_total,power_W_instant,mean_W,std_W"
else
  echo "[rapl_joules] files: ${#ENERGY_FILES[@]}  interval=${INTERVAL}s (Ctrl+C to stop)"
  for f in "${ENERGY_FILES[@]}"; do echo "  - $f"; done
fi

TOTAL_J=0.0

trap ' 
  END_T=$(date +%s.%N)
  DUR=$(python3 - <<PY
import math
print(max(0.0, float("$END_T")-float("$START_T")))
PY
)
  if [ "$SAMPLES" -eq 0 ]; then echo; echo "No samples."; exit 0; fi
  MEAN=$(python3 - <<PY
print(float("$SUM_P")/max(1,$SAMPLES))
PY
)
  STD=$(python3 - <<PY
n=$SAMPLES
mean=float("$SUM_P")/max(1,n)
var=(float("$SUMSQ_P")-n*mean*mean)/(n-1) if n>1 else 0.0
import math; print(math.sqrt(max(0.0,var)))
PY
)
  echo
  echo "--- RAPL Energy/Power Stats ---"
  echo "Samples: $SAMPLES, Duration: ${DUR}s, Interval: ${INTERVAL}s"
  echo "Total Energy: ${TOTAL_J} J"
  echo "Mean Power:   ${MEAN} W"
  echo "Std Power:    ${STD} W"
  echo "Min/Max:      ${MIN_P:-N/A} W / ${MAX_P:-N/A} W"
  exit 0
' INT

while :; do
  fsleep "$INTERVAL"
  NOW_T=$(date +%s.%N)
  NOW_SUM_UJ=$(read_sum_uJ)

  # wraparound rough handling: add largest max if sum decreased
  if [ "$NOW_SUM_UJ" -lt "$PREV_SUM_UJ" ]; then
    BIGGEST_MAX=0
    for f in "${!MAXR[@]}"; do
      [ -n "${MAXR[$f]}" ] && [ "${MAXR[$f]}" -gt "$BIGGEST_MAX" ] && BIGGEST_MAX="${MAXR[$f]}"
    done
    if [ "$BIGGEST_MAX" -gt 0 ] ; then
      NOW_SUM_UJ=$(( NOW_SUM_UJ + BIGGEST_MAX ))
    fi
  fi

  # compute & accumulate (now returns mean/std too)
  read -r DT DE_UJ TOTAL_J P_W MIN_P MAX_P SUM_P SUMSQ_P SAMPLES MEAN_W STD_W <<<"$(python3 - <<PY
import math
prev_t=float("$PREV_T"); now_t=float("$NOW_T")
prev_u=int("$PREV_SUM_UJ"); now_u=int("$NOW_SUM_UJ")
dt=max(1e-9, now_t-prev_t)
de_uj=max(0, now_u - prev_u)
p = (de_uj/dt)/1e6  # W
total_j=float("$TOTAL_J")+de_uj/1e6
samples=int("$SAMPLES")+1
sum_p=float("$SUM_P")+p
sumsq_p=float("$SUMSQ_P")+p*p
min_p="$MIN_P"
max_p="$MAX_P"
if min_p in ("", "N/A"): min_p=p
else: min_p=min(float(min_p), p)
if max_p in ("", "N/A"): max_p=p
else: max_p=max(float(max_p), p)
mean = sum_p / samples
var  = (sumsq_p - samples*mean*mean)/(samples-1) if samples>1 else 0.0
std  = math.sqrt(max(0.0, var))
print(dt, de_uj, total_j, p, min_p, max_p, sum_p, sumsq_p, samples, mean, std)
PY
)"

  PREV_T="$NOW_T"
  PREV_SUM_UJ="$NOW_SUM_UJ"

  TS=$(date +"%Y-%m-%d %H:%M:%S")
  if [ "$CSV" -eq 1 ]; then
    printf "%s,%.6f,%.6f,%.6f,%.6f\n" "$TS" "$TOTAL_J" "$P_W" "$MEAN_W" "$STD_W"
  else
    printf "%s  %8.3f W | avg %8.3f W ± %7.3f | total %10.3f J\n" "$TS" "$P_W" "$MEAN_W" "$STD_W" "$TOTAL_J"
  fi
done
