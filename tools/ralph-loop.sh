#!/usr/bin/env bash
# Run ready GitHub tickets through pi or Claude Code until no ready work remains.
set -uo pipefail

usage() {
    cat <<'EOF'
Usage: ralph-loop.sh --agent {pi|claude} [options]

Options:
  --agent NAME                      pi or claude (required)
  --model MODEL                     Agent model (default: Sol for pi, opus for claude)
  --effort LEVEL                    off|minimal|low|medium|high|xhigh|max
  --adaptive-model-and-effort       Select model/effort from difficulty label
  --repo OWNER/NAME                 Repository (inferred when omitted)
  --ready-label LABEL               Eligibility label (default: ready-for-agent)
  --labels LABEL[,LABEL...]         Additional required labels; may be repeated
  --use-branch BRANCH               Check out/create this branch
  --initial-retry-interval-seconds N (default: 30)
  --max-retry-interval-seconds N     (default: 900)
  --usage-poll-seconds N             (default: 600)
  --once                            Process at most one ticket
  --dry-run                         Print selection without claiming/running
  --quiet                           Suppress routine and agent output
  --verbose                         Enable loop and agent diagnostics
  --help
EOF
}

die() { echo "error: $*" >&2; exit 1; }
warn() { echo "warning: $*" >&2; }
status() { ((quiet)) || echo "$*"; }
verbose_log() { ((verbose)) && echo "verbose: $*"; return 0; }
require_value() { [[ $# -ge 2 ]] || { echo "error: $1 requires a value" >&2; exit 2; }; }

agent="" model="" effort="medium" repo="" ready_label="ready-for-agent" use_branch=""
adaptive=0 once=0 dry_run=0 quiet=0 verbose=0
initial_retry=30 max_retry=900 usage_poll=600
labels=()
while (($#)); do
    case "$1" in
        --agent) require_value "$@"; agent=$2; shift 2 ;;
        --model) require_value "$@"; model=$2; shift 2 ;;
        --effort) require_value "$@"; effort=$2; shift 2 ;;
        --repo) require_value "$@"; repo=$2; shift 2 ;;
        --ready-label) require_value "$@"; ready_label=$2; shift 2 ;;
        --labels)
            require_value "$@"
            IFS=',' read -ra new_labels <<<"$2"
            labels+=("${new_labels[@]}")
            shift 2 ;;
        --use-branch) require_value "$@"; use_branch=$2; shift 2 ;;
        --initial-retry-interval-seconds) require_value "$@"; initial_retry=$2; shift 2 ;;
        --max-retry-interval-seconds) require_value "$@"; max_retry=$2; shift 2 ;;
        --usage-poll-seconds) require_value "$@"; usage_poll=$2; shift 2 ;;
        --adaptive-model-and-effort) adaptive=1; shift ;;
        --once) once=1; shift ;;
        --dry-run) dry_run=1; shift ;;
        --quiet) quiet=1; shift ;;
        --verbose) verbose=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "error: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[[ "$agent" == pi || "$agent" == claude ]] || { echo "error: --agent must be pi or claude" >&2; exit 2; }
[[ "$effort" =~ ^(off|minimal|low|medium|high|xhigh|max)$ ]] || { echo "error: unsupported --effort '$effort'" >&2; exit 2; }
[[ "$initial_retry" =~ ^[0-9]+$ && "$max_retry" =~ ^[0-9]+$ && "$usage_poll" =~ ^[0-9]+$ ]] || die "Retry intervals must be integers."
((initial_retry >= 1 && max_retry >= initial_retry)) || die "Retry intervals must be positive and max must be at least initial."
((usage_poll >= 1)) || die "Usage poll interval must be positive."
((quiet && verbose)) && die "--quiet and --verbose cannot be used together."
[[ "$agent" != claude || "$effort" =~ ^(low|medium|high|xhigh|max)$ ]] \
    || die "Effort '$effort' is not supported by claude. Use low, medium, high, xhigh, or max."

for command in gh git jq perl curl "$agent"; do
    command -v "$command" >/dev/null 2>&1 || die "$command is required but was not found on PATH."
done

if [[ -z "$model" ]]; then
    [[ "$agent" == pi ]] && model="openai-codex/gpt-5.6-sol" || model="opus"
fi
repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || die "Run this script from inside a Git repository."
cd "$repo_root" || exit 1
[[ -z "$(git status --porcelain --untracked-files=no)" ]] \
    || die "The tracked worktree is not clean. Commit or restore tracked changes before starting the loop."

if [[ -n "$use_branch" && "$(git rev-parse --abbrev-ref HEAD)" != "$use_branch" ]]; then
    if git show-ref --verify --quiet "refs/heads/$use_branch"; then
        git checkout "$use_branch" >/dev/null 2>&1
    elif git ls-remote --exit-code --heads origin "$use_branch" >/dev/null 2>&1; then
        git checkout -b "$use_branch" --track "origin/$use_branch" >/dev/null 2>&1
    else
        git checkout -b "$use_branch" >/dev/null 2>&1
    fi || die "Failed to check out branch '$use_branch'."
    status "Switched to branch '$use_branch'."
fi

[[ -n "$repo" ]] || repo=$(gh repo view --json nameWithOwner --jq .nameWithOwner) || exit 1
current_user=$(gh api user --jq .login) || exit 1
log_directory="${TMPDIR:-/tmp}/$agent-ralph-loop"
mkdir -p "$log_directory"

priority_of() {
    jq -r '[.labels[].name | ascii_downcase |
        if test("^(priority[:/]\\s*)?(critical|urgent|p0)$") then 0
        elif test("^(priority[:/]\\s*)?(high|p1)$") then 1
        elif test("^(priority[:/]\\s*)?(medium|normal|p2)$") then 2
        elif test("^(priority[:/]\\s*)?(low|p3)$") then 3
        elif test("^priority[:/]\\s*[0-9]+$") then capture("(?<n>[0-9]+)$").n|tonumber
        else 100 end] | min // 100'
}

get_next_ticket() {
    local args=(issue list --repo "$repo" --state open --label "$ready_label") extra json parents issue number assignee_count assigned blocked rank priority
    for extra in "${labels[@]}"; do [[ -n "$extra" ]] && args+=(--label "$extra"); done
    args+=(--limit 100 --json number,title,body,labels,assignees,url)
    json=$(gh "${args[@]}") || return 2
    [[ $(jq length <<<"$json") -gt 0 ]] || return 1
    parents=$(jq '[.[].body // "" | capture("(?im)^## Parent\\s*\\r?\\n+\\s*#(?<n>[0-9]+)").n | tonumber]' <<<"$json")
    local best="" best_key=""
    while IFS= read -r issue; do
        number=$(jq -r .number <<<"$issue")
        jq -e --argjson n "$number" 'index($n) != null' <<<"$parents" >/dev/null && continue
        assignee_count=$(jq '.assignees | length' <<<"$issue")
        assigned=$(jq -r --arg user "$current_user" '[.assignees[].login] | index($user) != null' <<<"$issue")
        [[ "$assignee_count" -eq 0 || "$assigned" == true ]] || continue
        blocked=$(gh api "repos/$repo/issues/$number" --jq '.issue_dependencies_summary.blocked_by // 0') || return 2
        ((blocked == 0)) || continue
        [[ "$assigned" == true ]] && rank=0 || rank=1
        priority=$(priority_of <<<"$issue")
        printf -v key '%d:%010d:%010d' "$rank" "$priority" "$number"
        if [[ -z "$best_key" || "$key" < "$best_key" ]]; then best_key=$key; best=$issue; fi
    done < <(jq -c '.[]' <<<"$json")
    [[ -n "$best" ]] || return 1
    printf '%s\n' "$best"
}

select_adaptive() {
    local issue=$1 difficulty count
    mapfile -t difficulties < <(jq -r '.labels[].name | ascii_downcase | capture("^difficulty[:/]\\s*(?<d>trivial|small|low|medium|large|high|hard)$").d' <<<"$issue" | sort -u)
    count=${#difficulties[@]}
    ((count > 0)) || die "Adaptive model and effort requires one supported difficulty label."
    ((count == 1)) || die "Adaptive model and effort found conflicting difficulty labels: ${difficulties[*]}."
    difficulty=${difficulties[0]}
    if [[ "$agent" == pi ]]; then small="openai-codex/gpt-5.6-terra"; large="openai-codex/gpt-5.6-sol"; else small=sonnet; large=opus; fi
    case "$difficulty" in
        trivial) ticket_model=$small; ticket_effort=medium ;;
        small|low) ticket_model=$small; ticket_effort=high ;;
        medium) ticket_model=$large; ticket_effort=medium ;;
        large|high|hard) ticket_model=$large; ticket_effort=high ;;
    esac
    selection_source="adaptive difficulty:$difficulty"
}

get_ticket_prompt() {
    local number=$1 issue comments
    issue=$(gh issue view "$number" --repo "$repo" --json number,title,body,comments,url) || return
    comments=$(jq -r 'if (.comments|length)==0 then "(No comments.)" else [.comments[].body] | join("\n\n---\n\n") end' <<<"$issue")
    cat <<EOF
Implement GitHub ticket #$(jq -r .number <<<"$issue"): $(jq -r .title <<<"$issue")
$(jq -r .url <<<"$issue")

You are running non-interactively. Work autonomously through implementation; do not stop at a plan and do not ask the user questions. Read and follow the repository instructions and domain documentation. Inspect the current worktree first because this may be a retry after a provider failure.

Only implement this ticket, not its parent or blocked follow-up tickets. Use the ticket's acceptance criteria as the contract. Run focused tests while developing, then the relevant builds, formatting checks, and tests before completion. Preserve unrelated and pre-existing untracked files.

When the ticket is fully implemented and verified:
1. Commit all tracked changes on the current branch with a message referencing #$(jq -r .number <<<"$issue").
2. Close #$(jq -r .number <<<"$issue") with a concise comment containing the commit hash and validation performed.
3. Finish with a concise implementation summary.

If implementation cannot be completed for a code, test, or specification reason, leave the issue open, do not commit partial work merely to satisfy this prompt, and explain the blocker in your final response.

## Ticket body

$(jq -r .body <<<"$issue")

## Ticket comments

$comments
EOF
}

strip_control() { perl -pe 's/\e\][^\a]*(?:\a|\e\\)//g; s/\e[PX^_].*?\e\\//g; s/\e\[[0-?]*[ -\/]*[@-~]//g; s/\e[@-_]//g; s/[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]//g'; }

ticket_complete() {
    local number=$1 starting_head=$2 state
    state=$(gh issue view "$number" --repo "$repo" --json state --jq .state) || return 1
    [[ "$state" == CLOSED && "$(git rev-parse HEAD)" != "$starting_head" && -z "$(git status --porcelain --untracked-files=no)" ]]
}

session_usage() {
    local kind=$1 source=$2 files=()
    if [[ "$kind" == pi ]]; then
        mapfile -d '' files < <(find "$source" -type f -name '*.jsonl' -print0 2>/dev/null)
        ((${#files[@]})) || return 1
        jq -Rn '[inputs | fromjson? | select(.type=="message" and .message.role=="assistant" and .message.usage) |
          {provider:(.message.provider//""),model:(.message.model//""),input:(.message.usage.input//0),output:(.message.usage.output//0),cacheRead:(.message.usage.cacheRead//0),cacheWrite:(.message.usage.cacheWrite//0),reasoning:(.message.usage.reasoning//0),total:(.message.usage.totalTokens//0),cost:(.message.usage.cost.total//0)}] |
          if length==0 then empty else {provider:(map(.provider)|map(select(length>0))|last//""),model:(map(.model)|map(select(length>0))|last//""),input:(map(.input)|add),output:(map(.output)|add),cacheRead:(map(.cacheRead)|add),cacheWrite:(map(.cacheWrite)|add),reasoning:(map(.reasoning)|add),total:(map(.total)|add),cost:(map(.cost)|add),costKnown:true} end' "${files[@]}"
    else
        local projects="${CLAUDE_CONFIG_DIR:-$HOME/.claude}/projects" id file
        [[ -d "$projects" ]] || return 1
        IFS=',' read -ra ids <<<"$source"
        for id in "${ids[@]}"; do while IFS= read -r -d '' file; do files+=("$file"); done < <(find "$projects" -type f -name "$id.jsonl" -print0 2>/dev/null); done
        ((${#files[@]})) || return 1
        jq -Rn '[inputs | fromjson? | select(.type=="assistant" and .message.usage) |
          {provider:"anthropic",model:(.message.model//""),input:(.message.usage.input_tokens//0),output:(.message.usage.output_tokens//0),cacheRead:(.message.usage.cache_read_input_tokens//0),cacheWrite:(.message.usage.cache_creation_input_tokens//0),reasoning:(.message.usage.output_tokens_details.thinking_tokens//0)}] |
          if length==0 then empty else {provider:"anthropic",model:(map(.model)|map(select(length>0))|last//""),input:(map(.input)|add),output:(map(.output)|add),cacheRead:(map(.cacheRead)|add),cacheWrite:(map(.cacheWrite)|add),reasoning:(map(.reasoning)|add),total:(map(.input+.output+.cacheRead+.cacheWrite)|add),cost:0,costKnown:false} end' "${files[@]}"
    fi
}

show_ticket_summary() {
    local number=$1 started=$2 ended=$3 usage=${4:-} duration=$((ended-started))
    echo "Ticket #$number summary"
    echo "  Started: $(date -d "@$started" --iso-8601=seconds)"
    echo "  Ended: $(date -d "@$ended" --iso-8601=seconds)"
    printf '  Duration: %02d:%02d:%02d\n' $((duration/3600)) $(((duration%3600)/60)) $((duration%60))
    if [[ -z "$usage" ]]; then echo "  Total tokens spent: unavailable"; return; fi
    if [[ $(jq -r .costKnown <<<"$usage") == true ]]; then
        jq -r '"  Total tokens spent: \(.total) (input \(.input), output \(.output), reasoning \(.reasoning), cache read \(.cacheRead), cache write \(.cacheWrite)); cost $\(.cost)"' <<<"$usage"
    else
        jq -r '"  Total tokens spent: \(.total) (input \(.input), output \(.output), reasoning \(.reasoning), cache read \(.cacheRead), cache write \(.cacheWrite)); cost not reported by this agent"' <<<"$usage"
    fi
}

format_window() {
    local name=$1 window=$2
    if [[ -z "$window" || "$window" == null ]]; then echo "  $name: not reported"; return; fi
    local used reset_after reset_text
    used=$(jq -r '.used_percent // .utilization // empty' <<<"$window")
    reset_after=$(jq -r '.reset_after_seconds // (if .resets_at then ((.resets_at|fromdateiso8601)-now) else empty end)' <<<"$window" 2>/dev/null)
    if [[ -z "$reset_after" ]]; then reset_text="reset unknown"; else
        reset_after=${reset_after%.*}; ((reset_after < 0)) && reset_after=0
        if ((reset_after >= 86400)); then reset_text="resets in $((reset_after/86400))d $(((reset_after%86400)/3600))h"
        elif ((reset_after >= 3600)); then reset_text="resets in $((reset_after/3600))h $(((reset_after%3600)/60))m"
        else reset_text="resets in $(((reset_after+59)/60))m"; fi
    fi
    if [[ -z "$used" ]]; then echo "  $name: usage not reported; $reset_text"; else
        awk -v n="$name" -v u="$used" -v r="$reset_text" 'BEGIN { rem=100-u; if(rem<0)rem=0; printf "  %s: %.1f%% used, %.1f%% remaining; %s\n",n,u,rem,r }'
    fi
}

show_provider_usage() {
    local usage=$1 provider credential response primary secondary current weekly
    provider=$(jq -r .provider <<<"$usage")
    echo "Provider usage ($provider/$(jq -r .model <<<"$usage"))"
    if [[ "$provider" == anthropic ]]; then
        credential="${CLAUDE_CONFIG_DIR:-$HOME/.claude}/.credentials.json"
        if [[ ! -f "$credential" ]] || ! token=$(jq -er '.claudeAiOauth.accessToken' "$credential" 2>/dev/null) || ! response=$(curl -fsS -H "Authorization: Bearer $token" -H 'anthropic-beta: oauth-2025-04-20' https://api.anthropic.com/api/oauth/usage); then
            warn "Could not read provider usage."; echo "  Current window: unavailable"; echo "  Weekly: unavailable"; return
        fi
        format_window "Current window" "$(jq -c '.five_hour // null' <<<"$response")"
        format_window "Weekly" "$(jq -c '.seven_day // null' <<<"$response")"
    elif [[ "$provider" == openai-codex ]]; then
        credential="${PI_CODING_AGENT_DIR:-$HOME/.pi/agent}/auth.json"
        if [[ ! -f "$credential" ]] || ! token=$(jq -er '.["openai-codex"] | select(.type=="oauth") | .access' "$credential" 2>/dev/null); then
            warn "Could not read provider usage."; echo "  Current window: unavailable"; echo "  Weekly: unavailable"; return
        fi
        account=$(jq -r '.["openai-codex"].accountId // ""' "$credential")
        if ! response=$(curl -fsS -H "Authorization: Bearer $token" -H "ChatGPT-Account-Id: $account" https://chatgpt.com/backend-api/wham/usage); then
            warn "Could not read provider usage."; echo "  Current window: unavailable"; echo "  Weekly: unavailable"; return
        fi
        primary=$(jq -c '.rate_limit.primary_window // null' <<<"$response"); secondary=$(jq -c '.rate_limit.secondary_window // null' <<<"$response")
        current=$primary; weekly=$secondary
        if [[ "$primary" != null && "$secondary" == null && $(jq -r '.limit_window_seconds >= 518400' <<<"$primary") == true ]]; then current=null; weekly=$primary; fi
        format_window "Current window" "$current"; format_window "Weekly" "$weekly"
    else
        echo "  Current window: not available from this provider"; echo "  Weekly: not available from this provider"
    fi
}

usage_error_re='usage limit|usage_limit_reached|usage cap|quota exceeded|insufficient_quota|out of credits|credit balance|billing limit|subscription limit|weekly limit|monthly limit|weighted tokens|token limit.*reset|rate limit.*reset|limit resets? at'
server_error_re='HTTP[[:space:]]*(408|409|425|429|5[0-9][0-9])|status[[:space:]]*(408|409|425|429|5[0-9][0-9])|server error|internal server error|service unavailable|bad gateway|gateway timeout|overloaded|temporarily unavailable|request timeout|timed out|ECONNRESET|ECONNREFUSED|ENETUNREACH|EAI_AGAIN|socket hang up|connection reset|connection closed|fetch failed|network error|server_error|stream.*(closed|terminated)'

while true; do
    ticket=$(get_next_ticket); ticket_result=$?
    ((ticket_result != 2)) || die "Failed to query eligible tickets."
    if ((ticket_result == 1)); then status "No unblocked, unclaimed '$ready_label' tickets are available."; break; fi
    number=$(jq -r .number <<<"$ticket"); title=$(jq -r .title <<<"$ticket")
    status "Selected #$number: $title"
    ticket_model=$model; ticket_effort=$effort; selection_source="command line/default"
    ((adaptive)) && select_adaptive "$ticket"
    echo "Ticket #$number model: $ticket_model; effort: $ticket_effort ($selection_source)."
    if ((dry_run)); then status "Dry run: would start $agent with model '$ticket_model' and effort '$ticket_effort'."; break; fi

    if [[ $(jq '.assignees|length' <<<"$ticket") -eq 0 ]]; then
        gh issue edit "$number" --repo "$repo" --add-assignee @me >/dev/null || exit 1
        status "Claimed #$number as $current_user."
    fi
    ticket_started=$(date +%s); timestamp=$(date +%Y%m%d-%H%M%S)
    ticket_session_directory="$log_directory/issue-$number-$timestamp-sessions"; session_ids=()
    [[ "$agent" == pi ]] && mkdir -p "$ticket_session_directory"
    status "Ticket #$number started at $(date -d "@$ticket_started" --iso-8601=seconds)."
    starting_head=$(git rev-parse HEAD); original_prompt=$(get_ticket_prompt "$number") || exit 1
    prompt=$original_prompt; retry_interval=$initial_retry; attempt=0

    while true; do
        ((++attempt)); attempt_timestamp=$(date +%Y%m%d-%H%M%S)
        log_path="$log_directory/issue-$number-$attempt_timestamp-attempt-$attempt.log"
        status "Starting $agent for #$number (attempt $attempt). Log: $log_path"
        if [[ "$agent" == pi ]]; then
            session_directory="$ticket_session_directory/attempt-$attempt"; mkdir -p "$session_directory"
            verbose_log "Session directory: $session_directory"
            agent_args=(--print --approve --model "$ticket_model" --thinking "$ticket_effort" --name "ralph-$number" --session-dir "$session_directory")
        else
            session_id=$(cat /proc/sys/kernel/random/uuid 2>/dev/null || uuidgen); session_ids+=("$session_id")
            verbose_log "Session id: $session_id"
            agent_args=(--print --dangerously-skip-permissions --model "$ticket_model" --effort "$ticket_effort" --session-id "$session_id")
        fi
        ((verbose)) && agent_args+=(--verbose)
        agent_args+=("$prompt")

        if ((quiet)); then "$agent" "${agent_args[@]}" 2>&1 | strip_control >"$log_path"; agent_exit=${PIPESTATUS[0]}
        else "$agent" "${agent_args[@]}" 2>&1 | strip_control | tee "$log_path"; agent_exit=${PIPESTATUS[0]}; fi

        if ticket_complete "$number" "$starting_head"; then status "Ticket #$number completed successfully."; break; fi
        if ((agent_exit == 0)); then
            warn "$agent exited successfully without completing #$number. Starting a recovery attempt using $log_path."
            prompt=$(cat <<EOF
$original_prompt

## Recovery attempt

A previous agent attempt exited successfully without closing ticket #$number.
Inspect the previous attempt log at:

$log_path

Determine why that attempt did not complete the original request, then resolve the issue recorded in the log and finish the original ticket. Continue from the current worktree and repository state. Do not merely repeat the previous explanation or stop after describing the blocker; try to resolve it and carry the original request through implementation, verification, commit, and issue closure.
EOF
)
            continue
        fi
        if grep -Eiq "$usage_error_re" "$log_path"; then warn "Usage limit detected for #$number. Retrying in $usage_poll seconds."; sleep "$usage_poll"; continue; fi
        if grep -Eiq "$server_error_re" "$log_path"; then
            warn "Server/API failure detected for #$number. Retrying in $retry_interval seconds."; sleep "$retry_interval"
            retry_interval=$((retry_interval*2)); ((retry_interval > max_retry)) && retry_interval=$max_retry
            continue
        fi
        die "$agent failed for a non-retryable implementation reason on #$number. The issue remains assigned and open. Inspect $log_path."
    done

    ticket_ended=$(date +%s)
    if [[ "$agent" == pi ]]; then usage_source=$ticket_session_directory; usage=$(session_usage pi "$ticket_session_directory" || true)
    else usage_source="Claude Code sessions ${session_ids[*]}"; ids=$(IFS=,; echo "${session_ids[*]}"); usage=$(session_usage claude "$ids" || true); fi
    show_ticket_summary "$number" "$ticket_started" "$ticket_ended" "$usage"
    if [[ -z "$usage" ]]; then warn "Could not read current-ticket usage from $usage_source."; else show_provider_usage "$usage"; fi
    ((once)) && break
done
