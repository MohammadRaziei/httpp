#!/usr/bin/env bash
# Runs the benchmark suite through CMake and writes benchmarks/results/full/*.json (quick: results/quick/) (ADR-0063).
#
#   benchmarks/run.sh quick   about 5 minutes, marked "smoke": the docs site ignores the result
#   benchmarks/run.sh full    about 25 minutes, marked "published": commit the .json to show it on the docs site
#
# The JSON records the hardware itself (CPU, cores, memory, OS, laptop or not), no name needed. Anything after the mode is passed to
# cmake as it is, e.g.  benchmarks/run.sh full -DBENCH_PYS="3.14t" -DBENCH_ONLY=client
# Close heavy programs and plug in the charger before a full run.
set -euo pipefail

mode="${1:-}"
[ $# -ge 1 ] && shift
extra=("$@")

case "$mode" in
    quick) target=bench_quick; purpose=smoke; kind=quick ;;
    full)  target=bench;       purpose=published; kind=full ;;
    *) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="$root/build-bench"
export UV_LINK_MODE=copy  # the venvs may live on a drive that cannot hard link (NTFS, network)

for tool in cmake ninja uv; do
    command -v "$tool" >/dev/null || { echo "missing tool: $tool" >&2; exit 1; }
done
# the mbedtls sources are generated at configure time by a script that needs these two
python3 -c "import jinja2, jsonschema" 2>/dev/null || { echo "missing python packages: pip install jinja2 jsonschema" >&2; exit 1; }
# Drogon (optional) needs the uuid headers: sudo apt install uuid-dev

args=(-S "$root/benchmarks" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DBENCH_PURPOSE=$purpose")
cmake "${args[@]}" "${extra[@]}"
cmake --build "$build" --target "$target"

echo
echo "results in $root/benchmarks/results/$kind:"
ls "$root/benchmarks/results/$kind"
