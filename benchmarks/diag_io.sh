#!/usr/bin/env bash
# Where does the time go in the C++ server with several I/O threads? Three measurements of cpp_bench's /io route
# (route_io, constant answer), server and load generator pinned to separate CPUs like the suite:
#   1. perf with kernel samples: which share of the cycles is kernel, which functions are hot
#   2. CPU ticks of every server thread over 3 seconds: are the connections spread evenly over the I/O threads?
#   3. counters per request: syscalls, context switches, CPU migrations, cycles, instructions (IPC), cache misses
#
#   benchmarks/diag_io.sh [IO_THREADS [SERVER_CPUS [LOAD_CPUS]]]       defaults: 4  0-7  8-15
#
# The same two measurements for Drogon, to compare the kernel share and the thread spread:
#   BENCH_CPP_BIN=build-bench/drogon_bench BENCH_DIAG_PATH=/sync benchmarks/diag_io.sh
#
# keep_alive_max is 0 (no limit, like Drogon) unless BENCH_KEEP_ALIVE_MAX says otherwise.
# Needs Linux, perf, taskset, oha and a built cpp_bench (`cmake --build build-bench --target cpp_bench`).
# Writes into build-bench/diag/<binary name>/ and prints the paths of the small result files at the end. Reading kernel samples
# needs perf_event_paranoid -1 and kptr_restrict 0 (to name kernel functions); if not, the script asks sudo to change them
# (until the next reboot).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${BENCH_CPP_BIN:-$ROOT/build-bench/cpp_bench}"
NAME="$(basename "$BIN")"
OUT="$ROOT/build-bench/diag/$NAME"
THREADS="${1:-4}"
SERVER_CPUS="${2:-0-7}"
LOAD_CPUS="${3:-8-15}"
PORT="${BENCH_DIAG_PORT:-18080}"
URL="http://127.0.0.1:$PORT${BENCH_DIAG_PATH:-/io}"

[ -x "$BIN" ] || { echo "missing $BIN: cmake --build build-bench --target $NAME" >&2; exit 1; }
for tool in perf taskset oha; do
    command -v "$tool" > /dev/null || { echo "$tool not found" >&2; exit 1; }
done
mkdir -p "$OUT"
rm -f "$OUT"/*.txt "$OUT"/perf_k.data
pkill -x "$NAME" 2> /dev/null
sleep 1

if [ "$(cat /proc/sys/kernel/perf_event_paranoid)" -gt -1 ]; then # -1: the syscall counter of part 3 is a tracepoint
    sudo sysctl kernel.perf_event_paranoid=-1 > /dev/null || { echo "could not lower perf_event_paranoid" >&2; exit 1; }
fi
if [ "$(cat /proc/sys/kernel/kptr_restrict)" -ne 0 ]; then
    sudo sysctl kernel.kptr_restrict=0 > /dev/null || { echo "could not set kptr_restrict" >&2; exit 1; }
fi

echo "== 1. perf (cycles, user and kernel), $THREADS I/O threads =="
taskset -c "$SERVER_CPUS" perf record -e cycles -g -o "$OUT/perf_k.data" -- \
    timeout -s INT 14 "$BIN" "$PORT" "$THREADS" "${BENCH_KEEP_ALIVE_MAX:-0}" > /dev/null 2> "$OUT/perf_record.txt" &
sleep 2
taskset -c "$LOAD_CPUS" oha -z 8s -c 64 --no-tui "$URL" | grep -E "Requests/sec|Success rate" | tee "$OUT/oha_perf.txt"
wait
perf report -i "$OUT/perf_k.data" --no-children --stdio -g none --sort dso 2> /dev/null | head -25 > "$OUT/perf_dso.txt"
perf report -i "$OUT/perf_k.data" --no-children --stdio -g none --sort dso,symbol 2> /dev/null | head -80 > "$OUT/perf_symbols.txt"

echo "== 2. CPU ticks per server thread (3 s) =="
pkill -x "$NAME" 2> /dev/null
sleep 1
taskset -c "$SERVER_CPUS" "$BIN" "$PORT" "$THREADS" "${BENCH_KEEP_ALIVE_MAX:-0}" > /dev/null 2>&1 &
sleep 1
SRV="$(pgrep -x "$NAME" | head -1)"
[ -n "$SRV" ] || { echo "the server did not start" >&2; exit 1; }
taskset -c "$LOAD_CPUS" oha -z 6s -c 64 --no-tui "$URL" > "$OUT/oha_threads_full.txt" 2>&1 &
LOAD=$!
sleep 2
snap() { for t in /proc/"$SRV"/task/*; do echo "$(basename "$t") $(awk '{print $14+$15}' "$t/stat") $(cat "$t/comm")"; done; }
snap | sort > "$OUT/ticks_a.tmp"
sleep 3
snap | sort > "$OUT/ticks_b.tmp"
join "$OUT/ticks_a.tmp" "$OUT/ticks_b.tmp" | awk '{print "tid " $1 "  cpu_ticks_in_3s " ($4-$2) "  name " $3}' | sort -k4 -nr > "$OUT/thread_spread.txt"
rm -f "$OUT/ticks_a.tmp" "$OUT/ticks_b.tmp"
cat "$OUT/thread_spread.txt"
wait "$LOAD" 2> /dev/null
pkill -x "$NAME" 2> /dev/null
grep -E "Requests/sec|Success rate" "$OUT/oha_threads_full.txt" > "$OUT/oha_threads.txt"
rm -f "$OUT/oha_threads_full.txt"

echo "== 3. counters per request (two windows of 2 s) =="
taskset -c "$SERVER_CPUS" "$BIN" "$PORT" "$THREADS" "${BENCH_KEEP_ALIVE_MAX:-0}" > /dev/null 2>&1 &
sleep 1
SRV="$(pgrep -x "$NAME" | head -1)"
[ -n "$SRV" ] || { echo "the server did not start" >&2; exit 1; }
taskset -c "$LOAD_CPUS" oha -z 8s -c 64 --no-tui "$URL" > "$OUT/oha_counters_full.txt" 2>&1 &
LOAD=$!
sleep 2
# Window 1: counters that always work. Window 2: the syscall tracepoint, which needs access to tracefs and may be refused (then only that line is missing).
perf stat -x, -p "$SRV" -o "$OUT/perf_stat_raw1.txt" \
    -e task-clock,context-switches,cpu-migrations,cycles,instructions,cache-misses -- sleep 2 2> "$OUT/perf_stat_errors.txt"
perf stat -x, -p "$SRV" -o "$OUT/perf_stat_raw2.txt" -e raw_syscalls:sys_enter -- sleep 2 2>> "$OUT/perf_stat_errors.txt"
wait "$LOAD" 2> /dev/null
pkill -x "$NAME" 2> /dev/null
RPS="$(awk '/Requests\/sec/ {print $2}' "$OUT/oha_counters_full.txt")"
{
    echo "requests_per_second $RPS (each counter window is 2 s, so requests = rate * 2; task-clock is in milliseconds)"
    cat "$OUT/perf_stat_raw1.txt" "$OUT/perf_stat_raw2.txt" 2> /dev/null | awk -F, -v rps="$RPS" 'BEGIN { n = rps * 2 }
        $1 ~ /^[0-9.]+$/ { name = $3; sub(/^cpu_(core|atom)\//, "", name); sub(/\/$/, "", name); printf "%-26s total %16.0f   per request %12.2f\n", name, $1, $1 / n }'
} > "$OUT/per_request.txt"
cat "$OUT/per_request.txt"
rm -f "$OUT/oha_counters_full.txt"

echo
echo "Done. Send these files:"
for f in perf_dso.txt perf_symbols.txt thread_spread.txt per_request.txt perf_stat_errors.txt oha_perf.txt oha_threads.txt; do echo "$OUT/$f"; done
