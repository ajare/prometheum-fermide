#!/usr/bin/env bash
# High analysis build (tickets #56, #79).
#
# Building.cpp grew until the compiler choked on the translation unit under
# elevated analysis settings. The behaviour moved to SimulationCoordinator,
# and this script keeps the acceptance check repeatable instead of anecdotal:
# it configures a dedicated build directory with PF_HIGH_ANALYSIS=ON, builds
# every project target, records the wall time and peak resident set of each
# translation unit compile, and fails if any translation unit crosses the
# budget. The headless smoke test runs at the end.
#
# Warnings are reported but not fatal: the codebase carries pre-existing
# diagnostics (float promotion in the rendering path, vendored ImGui) that
# are unrelated to translation-unit size. The gate this script provides is
# the one #56 asked for - no translation unit choking the compiler.
set -uo pipefail

CONFIG="Release"
BUILD_DIR="build-high-analysis"
BUILD_GUI="ON"
COMPILER="${CXX:-}"
MAX_SECONDS="120"
MAX_MIB="2048"
TOP="12"

print_usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

  --config DIR|Release   Build configuration. Defaults to Release. (Debug|Release)
  --build-dir PATH       CMake build directory. Defaults to ${BUILD_DIR}.
  --compiler PATH        C++ compiler. Defaults to \$CXX, else clang++ if present,
                       else c++.
  --max-seconds N        Fail if any translation unit compile exceeds N seconds.
                       Defaults to ${MAX_SECONDS}.
  --max-mib N            Fail if any translation unit compile peaks above N MiB.
                       Defaults to ${MAX_MIB}.
  --top N                Number of slowest translation units to list. Default ${TOP}.
  --gui                  Build the graphical application (default).
  --no-gui               Build only the core library and headless executable.
  --help, -h             Show this help.
EOF
}

usage_error() {
    printf 'Error: %s\n' "$1" >&2
    print_usage >&2
    exit 2
}

while (( $# > 0 )); do
    case "$1" in
        --config)    (( $# >= 2 )) || usage_error "--config requires a value."; CONFIG="$2"; shift 2 ;;
        --build-dir) (( $# >= 2 )) || usage_error "--build-dir requires a value."; BUILD_DIR="$2"; shift 2 ;;
        --compiler)  (( $# >= 2 )) || usage_error "--compiler requires a value."; COMPILER="$2"; shift 2 ;;
        --max-seconds) (( $# >= 2 )) || usage_error "--max-seconds requires a value."; MAX_SECONDS="$2"; shift 2 ;;
        --max-mib)   (( $# >= 2 )) || usage_error "--max-mib requires a value."; MAX_MIB="$2"; shift 2 ;;
        --top)       (( $# >= 2 )) || usage_error "--top requires a value."; TOP="$2"; shift 2 ;;
        --gui)       BUILD_GUI="ON"; shift ;;
        --no-gui)    BUILD_GUI="OFF"; shift ;;
        --help|-h)   print_usage; exit 0 ;;
        *)           usage_error "unknown parameter \"$1\"." ;;
    esac
done

case "$CONFIG" in
    Debug|Release) ;;
    *) usage_error "--config must be Debug or Release." ;;
esac

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || {
    echo "Error: unable to locate the repository directory." >&2; exit 1
}
REPO_DIR="$(dirname -- "$SCRIPT_DIR")"
cd -- "$REPO_DIR" || { echo "Error: unable to enter the repository directory." >&2; exit 1; }

for command in cmake /usr/bin/time; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Error: $command is required but was not found." >&2; exit 1; }
done

if [[ -z "$COMPILER" ]]; then
    if command -v clang++ >/dev/null 2>&1; then
        COMPILER="$(command -v clang++)"
    else
        COMPILER="$(command -v c++)"
    fi
fi
command -v "$COMPILER" >/dev/null 2>&1 || {
    echo "Error: compiler \"$COMPILER\" was not found." >&2; exit 1; }

mkdir -p "$BUILD_DIR" || exit 1
BUILD_DIR="$(cd -- "$BUILD_DIR" && pwd)"
STATS_LOG="$BUILD_DIR/high_analysis_stats.tsv"
BUILD_LOG="$BUILD_DIR/high_analysis_build.log"
: > "$STATS_LOG"

# The compile launcher: record wall time and peak RSS for the translation unit
# named by the last argument, then run the compile unchanged.
WRAPPER="$BUILD_DIR/pf_compile_stat.sh"
cat > "$WRAPPER" <<WRAPPER_EOF
#!/bin/sh
src=""
for arg in "\$@"; do src="\$arg"; done
exec /usr/bin/time -f "PFSTAT\t%e\t%M\t\$src\n" -o "$STATS_LOG" -a "\$@"
WRAPPER_EOF
chmod +x "$WRAPPER" || exit 1

echo "High analysis build: $COMPILER ($CONFIG), PF_HIGH_ANALYSIS=ON"
echo "  build directory: $BUILD_DIR"
echo "  budgets:         ${MAX_SECONDS}s and ${MAX_MIB} MiB per translation unit"
echo "  log:             $BUILD_LOG"

cmake --fresh -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$CONFIG" \
    -DPF_BUILD_GUI="$BUILD_GUI" \
    -DPF_HIGH_ANALYSIS=ON \
    -DCMAKE_CXX_COMPILER="$COMPILER" \
    -DCMAKE_CXX_COMPILER_LAUNCHER="$WRAPPER" > "$BUILD_LOG" 2>&1
result=$?
if (( result != 0 )); then
    echo "Configuration failed with exit code $result." >&2
    tail -n 40 "$BUILD_LOG" >&2
    exit "$result"
fi

cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel >> "$BUILD_LOG" 2>&1
result=$?
if (( result != 0 )); then
    echo "Build failed with exit code $result." >&2
    grep -n "error:" "$BUILD_LOG" | head -n 30 >&2
    exit "$result"
fi
echo "Build succeeded."

warning_count=$(grep -c "warning:" "$BUILD_LOG" || true)

# Report the per-translation-unit cost of every project source. Vendored
# third-party sources are listed by the log but excluded from the gate: their
# size is pinned, not authored here.
report=$(awk -F '\t' -v repo="$REPO_DIR" -v top="$TOP" '
    $1 == "PFSTAT" {
        file = $4
        sub(repo "/", "", file)
        vendored = (file ~ /_deps\// || file ~ /src\/imgui\//)
        if (vendored) { vfiles[file] = 1; next }
        t = $2 + 0; r = $3 / 1024
        files[file] = 1
        if (t > tmax[file]) { tmax[file] = t }
        if (r > rmax[file]) { rmax[file] = r }
        tsum += t; n++
    }
    END {
        printf "SUM\t%.2f\t%.1f\t%d\n", tsum, 0, n
        for (f in files) printf "T\t%.2f\t%.1f\t%s\n", tmax[f], rmax[f], f
    }' "$STATS_LOG")

echo
echo "Project translation units compiled: $(awk -F '\t' '$1 == "SUM" { print $4 }' <<< "$report")"
echo "Total compile time:                 $(awk -F '\t' '$1 == "SUM" { printf "%.1fs", $2 }' <<< "$report")"
echo "Warnings (informational):          ${warning_count}"
echo
echo "Slowest project translation units:"
printf '%10s %10s  %s\n' "seconds" "peak MiB" "translation unit"
sort -t $'\t' -k2 -nr <<< "$(awk -F '\t' '$1 == "T"' <<< "$report")" | head -n "$TOP" |
    awk -F '\t' '{ printf "%10.2f %10.1f  %s\n", $2, $3, $4 }'

echo
fail=0
while IFS=$'\t' read -r kind seconds mib file; do
    [[ "$kind" == "T" ]] || continue
    if awk -v s="$seconds" -v m="$MAX_SECONDS" 'BEGIN { exit !(s > m) }'; then
        echo "OVER BUDGET: ${file} took ${seconds}s (limit ${MAX_SECONDS}s)" >&2
        fail=1
    fi
    if awk -v r="$mib" -v m="$MAX_MIB" 'BEGIN { exit !(r > m) }'; then
        echo "OVER BUDGET: ${file} peaked at ${mib} MiB (limit ${MAX_MIB} MiB)" >&2
        fail=1
    fi
done <<< "$report"

if (( fail != 0 )); then
    echo "High analysis gate failed: a translation unit crossed the budget." >&2
    exit 1
fi
echo "High analysis gate passed: no project translation unit exceeded ${MAX_SECONDS}s or ${MAX_MIB} MiB."

echo
echo "Running the headless smoke test..."
ctest --test-dir "$BUILD_DIR" --output-on-failure
result=$?
if (( result != 0 )); then
    echo "Headless smoke test failed with exit code $result." >&2
    exit "$result"
fi

echo "High analysis build and headless smoke test passed."
