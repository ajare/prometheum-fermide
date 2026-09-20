#!/usr/bin/env bash
# Regression test for #106: bug-hunt tickets must inherit Ralph's label filters.
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fixture_root="$tmp/repo"
bin="$tmp/bin"
state="$tmp/state"
mkdir -p "$fixture_root/tools" "$bin" "$state"
cp "$repo_root/tools/ralph_loop.sh" "$repo_root/tools/bug_hunt.sh" "$fixture_root/tools/"
printf 'before\n' >"$state/head"

cat >"$bin/git" <<'EOF'
#!/usr/bin/env bash
case "${1:-} ${2:-}" in
    "rev-parse --show-toplevel") printf '%s\n' "$FIXTURE_ROOT" ;;
    "rev-parse HEAD") cat "$TEST_STATE/head" ;;
    "status --porcelain"*) ;;
    "fake-commit ") printf 'after\n' >"$TEST_STATE/head" ;;
    *) echo "unexpected git invocation: git $*" >&2; exit 1 ;;
esac
EOF

cat >"$bin/gh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

has_arg() {
    local wanted=$1 arg
    shift
    for arg in "$@"; do [[ "$arg" == "$wanted" ]] && return 0; done
    return 1
}

case "${1:-} ${2:-}" in
    "repo view") echo 'owner/repo' ;;
    "api user") echo 'tester' ;;
    "issue create")
        shift 2
        labels=()
        while (($#)); do
            case "$1" in
                --label) labels+=("$2"); shift 2 ;;
                --title|--body) shift 2 ;;
                *) echo "unexpected issue create argument: $1" >&2; exit 1 ;;
            esac
        done
        printf '%s\n' "${labels[@]}" | sort -u >"$TEST_STATE/published-labels"
        printf 'OPEN\n' >"$TEST_STATE/issue-state"
        echo 'https://github.test/owner/repo/issues/501'
        ;;
    "issue list")
        shift 2
        requested=()
        while (($#)); do
            case "$1" in
                --label) requested+=("$2"); shift 2 ;;
                --repo|--state|--limit|--json) shift 2 ;;
                *) echo "unexpected issue list argument: $1" >&2; exit 1 ;;
            esac
        done
        printf '%s\n' "${requested[*]}" >>"$TEST_STATE/list-labels"
        if [[ ! -f "$TEST_STATE/issue-state" || "$(cat "$TEST_STATE/issue-state")" != OPEN ]]; then
            echo '[]'
            exit 0
        fi
        for label in "${requested[@]}"; do
            grep -Fxq "$label" "$TEST_STATE/published-labels" || { echo '[]'; exit 0; }
        done
        cat <<'JSON'
[{"number":501,"title":"Forwarded-label regression","body":"A bug found by the hunt.","labels":[{"name":"bug"},{"name":"ready-for-agent"},{"name":"difficulty:easy"},{"name":"priority:medium"},{"name":"feature:platform-lifts"},{"name":"release:test"},{"name":"team:simulation"}],"assignees":[],"url":"https://github.test/owner/repo/issues/501"}]
JSON
        ;;
    "issue edit") : ;;
    "issue view")
        if has_arg state "$@"; then
            cat "$TEST_STATE/issue-state"
        else
            cat <<'JSON'
{"number":501,"title":"Forwarded-label regression","body":"A bug found by the hunt.","comments":[],"url":"https://github.test/owner/repo/issues/501"}
JSON
        fi
        ;;
    "issue close") printf 'CLOSED\n' >"$TEST_STATE/issue-state" ;;
    "api repos/owner/repo/issues/501") echo '0' ;;
    *) echo "unexpected gh invocation: gh $*" >&2; exit 1 ;;
esac
EOF

cat >"$bin/pi" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
name=""
previous=""
for arg in "$@"; do
    [[ "$previous" == --name ]] && name=$arg
    previous=$arg
done
prompt=${!#}
case "$name" in
    bug-hunt)
        for label in feature:platform-lifts release:test team:simulation; do
            grep -Fq -- "- $label" <<<"$prompt" \
                || { echo "bug-hunt prompt omitted $label" >&2; exit 1; }
        done
        gh issue create --title 'Forwarded-label regression' --body 'A bug found by the hunt.' \
            --label bug --label ready-for-agent --label difficulty:easy --label priority:medium \
            --label feature:platform-lifts --label release:test --label team:simulation >/dev/null
        ;;
    ralph-501)
        printf '501\n' >"$TEST_STATE/selected-ticket"
        git fake-commit
        gh issue close 501
        ;;
    *) echo "unexpected pi session name: $name" >&2; exit 1 ;;
esac
EOF

# Force bug_hunt.sh down its portable non-systemd execution path.
cat >"$bin/systemctl" <<'EOF'
#!/usr/bin/env bash
exit 1
EOF

chmod +x "$bin/git" "$bin/gh" "$bin/pi" "$bin/systemctl"

export FIXTURE_ROOT="$fixture_root"
export TEST_STATE="$state"
export PATH="$bin:$PATH"
export BUG_HUNT_MEMORY_MAX=

"$fixture_root/tools/ralph_loop.sh" \
    --agent pi \
    --labels feature:platform-lifts,release:test \
    --labels team:simulation \
    --bug-hunt test-model:low \
    --fix-bugs \
    --once \
    --quiet

expected_labels=(
    bug
    ready-for-agent
    difficulty:easy
    priority:medium
    feature:platform-lifts
    release:test
    team:simulation
)
for label in "${expected_labels[@]}"; do
    grep -Fxq "$label" "$state/published-labels" \
        || { echo "published ticket omitted label: $label" >&2; exit 1; }
done

[[ "$(cat "$state/selected-ticket")" == 501 ]] \
    || { echo 'post-hunt loop did not select the newly published ticket' >&2; exit 1; }
grep -Fq 'ready-for-agent feature:platform-lifts release:test team:simulation bug' "$state/list-labels" \
    || { echo 'post-hunt listing did not apply all original labels plus bug' >&2; exit 1; }

printf 'ok: bug-hunt labels are published and selected by the post-hunt loop\n'
