#!/usr/bin/env bash
# Regression test for #107: the renamed Ralph script must document its real path.
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
script="$repo_root/tools/ralph_loop.sh"

[[ -x "$script" ]] || { echo "FAIL: $script is missing or not executable" >&2; exit 1; }

# No stale hyphenated executable references may remain in the script.
if rg -n 'ralph-loop\.sh' "$script"; then
    echo "FAIL: stale ralph-loop.sh references remain in $script" >&2
    exit 1
fi

# The usage banner must name ralph_loop.sh.
rg -q '^Usage: ralph_loop\.sh ' "$script" || {
    echo "FAIL: usage banner does not identify ralph_loop.sh" >&2
    exit 1
}

# The documented invocation path must actually run.
output=$("$repo_root/tools/ralph_loop.sh" --help)
rg -q '^Usage: ralph_loop\.sh ' <<<"$output" || {
    echo "FAIL: --help output does not identify ralph_loop.sh" >&2
    exit 1
}

bash -n "$script"

echo "ok: ralph_loop.sh documents and runs its real path"
