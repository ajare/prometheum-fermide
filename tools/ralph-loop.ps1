<#
.SYNOPSIS
Runs ready GitHub tickets through pi or Claude Code until no dependency-free
work remains.

.DESCRIPTION
Selects open issues carrying the ready-for-agent label, excludes issues with
open native GitHub blockers, and resumes tickets already assigned to the
current user before claiming new work. New work is ordered by priority labels
(critical/P0, high/P1, medium/P2, low/P3, or priority:<number>) and then issue
number. Priority and difficulty namespace labels accept either `:` or `/`. Issues referenced by another ticket's "## Parent" section are treated
as specs/maps rather than executable tickets.

The agent is chosen with the mandatory -Agent parameter and runs in
non-interactive print mode. Provider failures use capped exponential backoff;
usage-limit failures poll at ten-minute intervals by default. If an agent exits
successfully without closing its ticket, the loop starts another attempt with
the original request plus a recovery prompt pointing to the previous attempt's
log. The agent is told to inspect and resolve the issue recorded there rather
than merely repeating it. After each completed ticket the loop reports its ISO
8601 start and end timestamps, duration, and total token usage across every
retry attempt. Where the provider
exposes it, the loop also reports current-window and weekly usage. Logs and
sessions are written below the system temporary directory, in pi-ralph-loop or
claude-ralph-loop according to the agent.

Model names and effort levels differ by agent. Pi takes provider/model names -
use `pi --list-models` to see what the providers configured on this machine
offer - and accepts off, minimal, low, medium, high, xhigh, and max. Claude
Code takes an alias such as opus or sonnet, or a full model name, and accepts
low, medium, high, xhigh, and max; off and minimal are rejected for Claude
because that agent has no equivalent.

Token usage is read from the agent's own session transcripts. Pi reports cost
per message and the loop totals it; Claude Code does not record cost in its
transcripts, so cost is reported as unavailable rather than guessed.

PowerShell's common -Verbose switch also passes --verbose to the agent and
enables extra loop diagnostics.

.PARAMETER Agent
Coding agent to run tickets through: pi or claude. Mandatory. The agent's
command must be on PATH, and it determines model naming, effort levels, and
where session transcripts are read from.

.PARAMETER Model
Model for the selected agent. For pi, a provider/model name defaulting to
openai-codex/gpt-5.6-sol. For claude, an alias or full model name defaulting
to opus. Ignored when AdaptiveModelAndEffort is specified.

.PARAMETER Effort
Thinking effort passed to the agent. Defaults to medium. Pi accepts off,
minimal, low, medium, high, xhigh, and max; claude accepts low, medium, high,
xhigh, and max. Ignored when AdaptiveModelAndEffort is specified.

.PARAMETER AdaptiveModelAndEffort
Selects the model and effort from the ticket's difficulty label. The bigger
model of the pair handles the harder tickets, and effort rises within each
model:

  difficulty:trivial            smaller model, medium effort
  difficulty:small (or :low)    smaller model, high effort
  difficulty:medium             larger model, medium effort
  difficulty:large (or :high/:hard)
                                larger model, high effort

For pi the pair is GPT-5.6 Terra and Sol; for claude it is Sonnet and Opus.
The script stops if an eligible ticket has no supported difficulty label or
has conflicting labels.

.PARAMETER Repo
GitHub repository in owner/name form. When omitted, the repository is inferred
from the current checkout's origin remote.

.PARAMETER ReadyLabel
Label used to identify executable tickets. Defaults to ready-for-agent.

.PARAMETER Labels
Additional labels a ticket must carry, on top of ReadyLabel, to be eligible.
Repeat the parameter or pass a comma-separated list.

.PARAMETER UseBranch
Branch to run the loop against. If it exists locally or on the remote it is
checked out; otherwise it is created from the current HEAD. When omitted, the
loop runs on whatever branch is already checked out.

.PARAMETER InitialRetryIntervalSeconds
Initial delay after a retryable provider or server failure. Defaults to 30.

.PARAMETER MaxRetryIntervalSeconds
Maximum exponential-backoff delay for provider or server failures. Defaults to
900.

.PARAMETER UsagePollSeconds
Delay between retries after a provider usage-limit response. Defaults to 600.

.PARAMETER Once
Processes at most one ticket, then exits.

.PARAMETER DryRun
Prints the next eligible ticket without claiming it or starting the agent.

.PARAMETER Quiet
Suppresses routine loop and agent output while retaining warnings, errors, and
the required end-of-ticket usage summary.

.EXAMPLE
.\tools\ralph-loop.ps1 -Agent pi

Runs the full frontier through pi with its defaults: GPT-5.6 Sol at medium
effort.

.EXAMPLE
.\tools\ralph-loop.ps1 -Agent claude

Runs the full frontier through Claude Code with its defaults: Opus at medium
effort.

.EXAMPLE
.\tools\ralph-loop.ps1 -Agent pi -Model openai-codex/gpt-5.6-terra -Effort high

Uses GPT-5.6 Terra with high reasoning effort for every eligible ticket.

.EXAMPLE
.\tools\ralph-loop.ps1 -Agent claude -Model sonnet -Effort low -Once

Uses Sonnet at low effort and stops after one ticket.

.EXAMPLE
.\tools\ralph-loop.ps1 -Agent pi -AdaptiveModelAndEffort

Chooses GPT-5.6 Terra or Sol and medium or high effort independently for each
ticket, based on its difficulty label.

.EXAMPLE
.\tools\ralph-loop.ps1 -Agent claude -AdaptiveModelAndEffort

Chooses Sonnet or Opus and medium or high effort independently for each ticket,
based on its difficulty label.

.EXAMPLE
.\tools\ralph-loop.ps1 -Agent pi -Model openai-codex/gpt-5.4-mini -Effort minimal -Quiet

Uses a smaller model at minimal effort and shows only essential output and the
usage summary.

.EXAMPLE
.\tools\ralph-loop.ps1 -Agent claude -Effort xhigh -Once -Verbose

Uses Claude Code's default model at extra-high effort for one ticket, with
verbose agent and loop diagnostics.

.EXAMPLE
.\tools\ralph-loop.ps1 -Agent pi -Repo ajare/prometheum-fermide -ReadyLabel ready-for-agent -DryRun

Shows the next eligible ticket in an explicit repository without claiming or
running it.

.EXAMPLE
.\tools\ralph-loop.ps1 -Agent pi -AdaptiveModelAndEffort -Labels "feature:editor-3d-preview" -UseBranch feature/editor-3d-preview

Runs only tickets also labeled feature:editor-3d-preview, on the
feature/editor-3d-preview branch (created from the current HEAD if it doesn't
already exist).

.EXAMPLE
.\tools\ralph-loop.ps1 -InitialRetryIntervalSeconds 15 -MaxRetryIntervalSeconds 300 -UsagePollSeconds 900

Overrides provider-failure backoff and usage-limit polling intervals.

.EXAMPLE
Get-Help .\tools\ralph-loop.ps1 -Detailed

Shows parameter descriptions and these examples in PowerShell help.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("pi", "claude")]
    [string]$Agent,

    [string]$Model = "",

    [ValidateSet("off", "minimal", "low", "medium", "high", "xhigh", "max")]
    [string]$Effort = "medium",

    [switch]$AdaptiveModelAndEffort,

    [string]$Repo = "",
    [string]$ReadyLabel = "ready-for-agent",
    [string[]]$Labels = @(),
    [string]$UseBranch = "",
    [int]$InitialRetryIntervalSeconds = 30,
    [int]$MaxRetryIntervalSeconds = 900,
    [int]$UsagePollSeconds = 600,
    [switch]$Once,
    [switch]$DryRun,
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Invoke-Gh {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $output = & gh @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "gh $($Arguments -join ' ') failed:`n$($output -join [Environment]::NewLine)"
    }
    return ($output -join [Environment]::NewLine)
}

function Invoke-GitQuiet {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    # Windows PowerShell turns ordinary native stderr (including git's
    # successful "Switched to..." messages) into error records when stderr is
    # merged while ErrorActionPreference is Stop. Probe the exit code with
    # native output suppressed under Continue, then restore the caller's mode.
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & git @Arguments *> $null
        return $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

function Get-Priority {
    param([object[]]$Labels)

    $rank = 100
    foreach ($label in $Labels) {
        $name = ([string]$label.name).ToLowerInvariant().Trim()
        switch -Regex ($name) {
            '^(priority[:/]\s*)?(critical|urgent|p0)$' { $rank = [Math]::Min($rank, 0); continue }
            '^(priority[:/]\s*)?(high|p1)$'            { $rank = [Math]::Min($rank, 1); continue }
            '^(priority[:/]\s*)?(medium|normal|p2)$'  { $rank = [Math]::Min($rank, 2); continue }
            '^(priority[:/]\s*)?(low|p3)$'            { $rank = [Math]::Min($rank, 3); continue }
            '^priority[:/]\s*(\d+)$'                  { $rank = [Math]::Min($rank, [int]$Matches[1]); continue }
        }
    }
    return $rank
}

function Get-AgentModelPair {
    param([Parameter(Mandatory = $true)][string]$AgentName)

    # The larger model of each pair is also that agent's default.
    switch ($AgentName) {
        "pi" {
            return [pscustomobject]@{ Smaller = "openai-codex/gpt-5.6-terra"; Larger = "openai-codex/gpt-5.6-sol" }
        }
        "claude" {
            return [pscustomobject]@{ Smaller = "sonnet"; Larger = "opus" }
        }
        default {
            throw "Unsupported agent '$AgentName'."
        }
    }
}

function Test-EffortSupported {
    param(
        [Parameter(Mandatory = $true)][string]$AgentName,
        [Parameter(Mandatory = $true)][string]$EffortLevel
    )

    # Claude Code has no equivalent of pi's off and minimal levels.
    if ($AgentName -eq "claude") {
        return $EffortLevel -in @("low", "medium", "high", "xhigh", "max")
    }
    return $true
}

function Get-AdaptiveModelAndEffort {
    param(
        [Parameter(Mandatory = $true)][string]$AgentName,
        [Parameter(Mandatory = $true)][object[]]$Labels
    )

    $difficultyLabels = @($Labels | ForEach-Object {
        $name = ([string]$_.name).ToLowerInvariant().Trim()
        if ($name -match '^difficulty[:/]\s*(trivial|small|low|medium|large|high|hard)$') {
            $Matches[1]
        }
    } | Select-Object -Unique)

    if ($difficultyLabels.Count -eq 0) {
        throw "Adaptive model and effort requires one supported difficulty label using ':' or '/', for example difficulty:medium or difficulty/medium."
    }
    if ($difficultyLabels.Count -gt 1) {
        throw "Adaptive model and effort found conflicting difficulty labels: $($difficultyLabels -join ', ')."
    }

    $models = Get-AgentModelPair -AgentName $AgentName

    switch ($difficultyLabels[0]) {
        "trivial" {
            return [pscustomobject]@{ Difficulty = "trivial"; Model = $models.Smaller; Effort = "medium" }
        }
        { $_ -in @("small", "low") } {
            return [pscustomobject]@{ Difficulty = $_; Model = $models.Smaller; Effort = "high" }
        }
        "medium" {
            return [pscustomobject]@{ Difficulty = "medium"; Model = $models.Larger; Effort = "medium" }
        }
        { $_ -in @("large", "high", "hard") } {
            return [pscustomobject]@{ Difficulty = $_; Model = $models.Larger; Effort = "high" }
        }
        default {
            throw "Unsupported difficulty label '$($difficultyLabels[0])'."
        }
    }
}

function Get-NextTicket {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][string]$Label,
        [string[]]$ExtraLabels = @(),
        [Parameter(Mandatory = $true)][string]$CurrentUser
    )

    $listArguments = @(
        "issue", "list", "--repo", $Repository,
        "--state", "open", "--label", $Label
    )
    foreach ($extraLabel in $ExtraLabels) {
        $listArguments += @("--label", $extraLabel)
    }
    $listArguments += @(
        "--limit", "100",
        "--json", "number,title,body,labels,assignees,url"
    )
    $json = Invoke-Gh $listArguments
    $issues = $json | ConvertFrom-Json
    if ($issues.Count -eq 0) {
        return $null
    }

    # Specs/maps can carry the ready label too. A ready issue referenced by the
    # ticket convention's "## Parent" section is not itself executable work.
    $parentNumbers = @{}
    foreach ($issue in $issues) {
        if ([string]$issue.body -match '(?im)^## Parent\s*\r?\n+\s*#(\d+)') {
            $parentNumbers[[int]($Matches[1])] = $true
        }
    }

    $candidates = @()
    foreach ($issue in $issues) {
        if ($parentNumbers.ContainsKey([int]($issue.number))) {
            continue
        }

        $assignees = @($issue.assignees)
        $assignedToCurrentUser = @($assignees | Where-Object { $_.login -eq $CurrentUser }).Count -gt 0
        if ($assignees.Count -gt 0 -and -not $assignedToCurrentUser) {
            continue
        }

        $detailJson = Invoke-Gh @("api", "repos/$Repository/issues/$($issue.number)")
        $detail = $detailJson | ConvertFrom-Json
        if ([int]($detail.issue_dependencies_summary.blocked_by) -gt 0) {
            continue
        }

        $candidates += [pscustomobject]@{
            Issue = $issue
            ResumeRank = if ($assignedToCurrentUser) { 0 } else { 1 }
            Priority = Get-Priority @($issue.labels)
        }
    }

    if ($candidates.Count -eq 0) {
        return $null
    }

    return ($candidates |
        Sort-Object ResumeRank, Priority, @{ Expression = { [int]($_.Issue.number) } } |
        Select-Object -First 1).Issue
}

function Get-TicketPrompt {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][int]$Number
    )

    $json = Invoke-Gh @(
        "issue", "view", [string]$Number, "--repo", $Repository,
        "--json", "number,title,body,comments,url"
    )
    $issue = $json | ConvertFrom-Json
    $comments = @($issue.comments | ForEach-Object { $_.body })
    $commentText = if ($comments.Count -eq 0) {
        "(No comments.)"
    } else {
        ($comments -join "`n`n---`n`n")
    }

    return @"
Implement GitHub ticket #$($issue.number): $($issue.title)
$($issue.url)

You are running non-interactively. Work autonomously through implementation; do not stop at a plan and do not ask the user questions. Read and follow the repository instructions and domain documentation. Inspect the current worktree first because this may be a retry after a provider failure.

Only implement this ticket, not its parent or blocked follow-up tickets. Use the ticket's acceptance criteria as the contract. Run focused tests while developing, then the relevant builds, formatting checks, and tests before completion. Preserve unrelated and pre-existing untracked files.

When the ticket is fully implemented and verified:
1. Commit all tracked changes on the current branch with a message referencing #$($issue.number).
2. Close #$($issue.number) with a concise comment containing the commit hash and validation performed.
3. Finish with a concise implementation summary.

If implementation cannot be completed for a code, test, or specification reason, leave the issue open, do not commit partial work merely to satisfy this prompt, and explain the blocker in your final response.

## Ticket body

$($issue.body)

## Ticket comments

$commentText
"@
}

function Write-Status {
    param([Parameter(Mandatory = $true)][string]$Message)

    if (-not $Quiet) {
        Write-Host $Message
    }
}

function Remove-TerminalControlSequences {
    param([AllowEmptyString()][Parameter(Mandatory = $true)][string]$Text)

    # Agents can emit terminal teardown commands on stderr even in print mode.
    # Never replay cursor movement, screen switching, or other control codes in
    # the parent terminal or preserve them in the attempt log.
    return $Text `
        -replace '\x1B\][^\x07]*(?:\x07|\x1B\\)', '' `
        -replace '\x1B[PX^_].*?\x1B\\', '' `
        -replace '\x1B\[[0-?]*[ -/]*[@-~]', '' `
        -replace '\x1B[@-_]', '' `
        -replace '[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]', ''
}

function Get-NumericProperty {
    param(
        [object]$Object,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($null -eq $Object) {
        return [double]0
    }
    $property = $Object.psobject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) {
        return [double]0
    }
    return [double]$property.Value
}

function Get-ClaudeConfigDirectory {
    if ($env:CLAUDE_CONFIG_DIR) {
        return $env:CLAUDE_CONFIG_DIR
    }
    return (Join-Path $HOME ".claude")
}

function Get-ClaudeSessionUsage {
    param([Parameter(Mandatory = $true)][string[]]$SessionIds)

    # Claude Code writes one transcript per session under its projects
    # directory, named for the session id. Find them by that name rather than
    # by reconstructing the project's directory slug.
    $projectsDirectory = Join-Path (Get-ClaudeConfigDirectory) "projects"
    if (-not (Test-Path $projectsDirectory)) {
        return $null
    }

    $usage = [ordered]@{
        Provider = "anthropic"
        Model = ""
        Input = [int64]0
        Output = [int64]0
        CacheRead = [int64]0
        CacheWrite = [int64]0
        Reasoning = [int64]0
        TotalTokens = [int64]0
        Cost = [double]0
        CostKnown = $false
    }

    $sawUsage = $false
    foreach ($sessionId in $SessionIds) {
        $transcripts = @(Get-ChildItem -Path $projectsDirectory -Filter "$sessionId.jsonl" -File -Recurse -ErrorAction SilentlyContinue)
        foreach ($transcript in $transcripts) {
            foreach ($line in [System.IO.File]::ReadLines($transcript.FullName)) {
                try {
                    $entry = $line | ConvertFrom-Json
                } catch {
                    continue
                }

                $typeProperty = $entry.psobject.Properties["type"]
                if ($null -eq $typeProperty -or ([string]$typeProperty.Value) -ne "assistant") {
                    continue
                }
                $messageProperty = $entry.psobject.Properties["message"]
                if ($null -eq $messageProperty -or $null -eq $messageProperty.Value) {
                    continue
                }
                $message = $messageProperty.Value
                $usageProperty = $message.psobject.Properties["usage"]
                if ($null -eq $usageProperty -or $null -eq $usageProperty.Value) {
                    continue
                }

                $messageUsage = $usageProperty.Value
                $modelProperty = $message.psobject.Properties["model"]
                if ($null -ne $modelProperty) {
                    $usage.Model = [string]$modelProperty.Value
                }
                $usage.Input += [int64](Get-NumericProperty $messageUsage "input_tokens")
                $usage.Output += [int64](Get-NumericProperty $messageUsage "output_tokens")
                $usage.CacheRead += [int64](Get-NumericProperty $messageUsage "cache_read_input_tokens")
                $usage.CacheWrite += [int64](Get-NumericProperty $messageUsage "cache_creation_input_tokens")
                $detailsProperty = $messageUsage.psobject.Properties["output_tokens_details"]
                if ($null -ne $detailsProperty) {
                    $usage.Reasoning += [int64](Get-NumericProperty $detailsProperty.Value "thinking_tokens")
                }
                $sawUsage = $true
            }
        }
    }

    if (-not $sawUsage) {
        return $null
    }

    # Claude Code transcripts carry no cost, so it is totalled from the parts
    # rather than reported.
    $usage.TotalTokens = $usage.Input + $usage.Output + $usage.CacheRead + $usage.CacheWrite
    return [pscustomobject]$usage
}

function Get-PiSessionUsage {
    param([Parameter(Mandatory = $true)][string]$SessionDirectory)

    $sessionFiles = @(Get-ChildItem -Path $SessionDirectory -Filter "*.jsonl" -File -Recurse -ErrorAction SilentlyContinue)
    if ($sessionFiles.Count -eq 0) {
        return $null
    }

    $usage = [ordered]@{
        Provider = ""
        Model = ""
        Input = [int64]0
        Output = [int64]0
        CacheRead = [int64]0
        CacheWrite = [int64]0
        Reasoning = [int64]0
        TotalTokens = [int64]0
        Cost = [double]0
        CostKnown = $true
    }

    foreach ($sessionFile in $sessionFiles) {
        foreach ($line in [System.IO.File]::ReadLines($sessionFile.FullName)) {
            try {
                $entry = $line | ConvertFrom-Json
            } catch {
                continue
            }
            if ($entry.type -ne "message" -or $entry.message.role -ne "assistant" -or $null -eq $entry.message.usage) {
                continue
            }

            $message = $entry.message
            $messageUsage = $message.usage
            $usage.Provider = [string]$message.provider
            $usage.Model = [string]$message.model
            $usage.Input += [int64](Get-NumericProperty $messageUsage "input")
            $usage.Output += [int64](Get-NumericProperty $messageUsage "output")
            $usage.CacheRead += [int64](Get-NumericProperty $messageUsage "cacheRead")
            $usage.CacheWrite += [int64](Get-NumericProperty $messageUsage "cacheWrite")
            $usage.Reasoning += [int64](Get-NumericProperty $messageUsage "reasoning")
            $usage.TotalTokens += [int64](Get-NumericProperty $messageUsage "totalTokens")
            $costProperty = $messageUsage.psobject.Properties["cost"]
            if ($null -ne $costProperty) {
                $usage.Cost += Get-NumericProperty $costProperty.Value "total"
            }
        }
    }

    return [pscustomobject]$usage
}

function Format-ResetDuration {
    param([object]$Window)

    if ($null -eq $Window) {
        return "reset unknown"
    }
    $resetAfterProperty = $Window.psobject.Properties["reset_after_seconds"]
    $resetAtProperty = $Window.psobject.Properties["resets_at"]
    if ($null -ne $resetAfterProperty -and $null -ne $resetAfterProperty.Value) {
        $duration = [TimeSpan]::FromSeconds([double]$resetAfterProperty.Value)
    } elseif ($null -ne $resetAtProperty -and $resetAtProperty.Value) {
        $resetAt = [DateTimeOffset]::Parse([string]$resetAtProperty.Value)
        $duration = $resetAt - [DateTimeOffset]::UtcNow
        if ($duration.TotalSeconds -lt 0) {
            $duration = [TimeSpan]::Zero
        }
    } else {
        return "reset unknown"
    }
    if ($duration.TotalDays -ge 1) {
        return "resets in $([Math]::Floor($duration.TotalDays))d $($duration.Hours)h"
    }
    if ($duration.TotalHours -ge 1) {
        return "resets in $([Math]::Floor($duration.TotalHours))h $($duration.Minutes)m"
    }
    return "resets in $([Math]::Max(0, [Math]::Ceiling($duration.TotalMinutes)))m"
}

function Write-UsageWindow {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [object]$Window
    )

    if ($null -eq $Window) {
        Write-Host "  ${Name}: not reported"
        return
    }
    $usedProperty = $Window.psobject.Properties["used_percent"]
    if ($null -eq $usedProperty) {
        $usedProperty = $Window.psobject.Properties["utilization"]
    }
    if ($null -eq $usedProperty -or $null -eq $usedProperty.Value) {
        Write-Host "  ${Name}: usage not reported; $(Format-ResetDuration $Window)"
        return
    }
    $used = [double]$usedProperty.Value
    $remaining = [Math]::Max([double]0, 100.0 - $used)
    Write-Host ("  {0}: {1:N1}% used, {2:N1}% remaining; {3}" -f $Name, $used, $remaining, (Format-ResetDuration $Window))
}

function Get-OAuthCredential {
    param([Parameter(Mandatory = $true)][string]$Provider)

    if ($Agent -eq "claude") {
        $credentialPath = Join-Path (Get-ClaudeConfigDirectory) ".credentials.json"
        if (-not (Test-Path $credentialPath)) {
            throw "No Claude Code credential file was found at $credentialPath."
        }
        $claudeAuth = Get-Content -Path $credentialPath -Raw | ConvertFrom-Json
        $oauthProperty = $claudeAuth.psobject.Properties["claudeAiOauth"]
        if ($null -eq $oauthProperty -or -not $oauthProperty.Value.accessToken) {
            throw "No OAuth credential is configured for Claude Code."
        }
        return [pscustomobject]@{ access = $oauthProperty.Value.accessToken; accountId = "" }
    }

    $configDirectory = if ($env:PI_CODING_AGENT_DIR) { $env:PI_CODING_AGENT_DIR } else { Join-Path $HOME ".pi/agent" }
    $authPath = Join-Path $configDirectory "auth.json"
    $authFile = Get-Content -Path $authPath -Raw | ConvertFrom-Json
    $credentialProperty = $authFile.psobject.Properties[$Provider]
    if ($null -eq $credentialProperty -or $credentialProperty.Value.type -ne "oauth") {
        throw "No OAuth credential is configured for $Provider."
    }
    return $credentialProperty.Value
}

function Show-TicketSummary {
    param(
        [Parameter(Mandatory = $true)][int]$Number,
        [Parameter(Mandatory = $true)][DateTimeOffset]$StartedAt,
        [Parameter(Mandatory = $true)][DateTimeOffset]$EndedAt,
        [object]$SessionUsage
    )

    Write-Host "Ticket #$Number summary"
    Write-Host "  Started: $($StartedAt.ToString('o'))"
    Write-Host "  Ended: $($EndedAt.ToString('o'))"
    Write-Host "  Duration: $($EndedAt - $StartedAt)"
    if ($null -eq $SessionUsage) {
        Write-Host "  Total tokens spent: unavailable"
        return
    }
    if ($SessionUsage.CostKnown) {
        Write-Host ('  Total tokens spent: {0:N0} (input {1:N0}, output {2:N0}, reasoning {3:N0}, cache read {4:N0}, cache write {5:N0}); cost ${6:N4}' -f
            $SessionUsage.TotalTokens, $SessionUsage.Input, $SessionUsage.Output, $SessionUsage.Reasoning,
            $SessionUsage.CacheRead, $SessionUsage.CacheWrite, $SessionUsage.Cost)
    } else {
        Write-Host ('  Total tokens spent: {0:N0} (input {1:N0}, output {2:N0}, reasoning {3:N0}, cache read {4:N0}, cache write {5:N0}); cost not reported by this agent' -f
            $SessionUsage.TotalTokens, $SessionUsage.Input, $SessionUsage.Output, $SessionUsage.Reasoning,
            $SessionUsage.CacheRead, $SessionUsage.CacheWrite)
    }
}

function Show-ProviderUsage {
    param([Parameter(Mandatory = $true)][object]$SessionUsage)

    $provider = if ($SessionUsage.Provider) { $SessionUsage.Provider } else { ($Model -split '/', 2)[0] }
    Write-Host "Provider usage ($provider/$($SessionUsage.Model))"

    if ($provider -notin @("openai-codex", "anthropic")) {
        Write-Host "  Current window: not available from this provider"
        Write-Host "  Weekly: not available from this provider"
        return
    }

    try {
        $credential = Get-OAuthCredential $provider
        if ($provider -eq "anthropic") {
            $headers = @{
                Authorization = "Bearer $($credential.access)"
                "anthropic-beta" = "oauth-2025-04-20"
            }
            $providerUsage = Invoke-RestMethod -Method Get -Uri "https://api.anthropic.com/api/oauth/usage" -Headers $headers
            Write-UsageWindow -Name "Current window" -Window $providerUsage.five_hour
            Write-UsageWindow -Name "Weekly" -Window $providerUsage.seven_day
            return
        }

        $headers = @{
            Authorization = "Bearer $($credential.access)"
            "ChatGPT-Account-Id" = [string]$credential.accountId
        }
        $providerUsage = Invoke-RestMethod -Method Get -Uri "https://chatgpt.com/backend-api/wham/usage" -Headers $headers
        $primary = $providerUsage.rate_limit.primary_window
        $secondary = $providerUsage.rate_limit.secondary_window

        # Codex normally reports a short rolling primary window and a weekly
        # secondary window. Some plans expose only one seven-day primary window.
        $currentWindow = $primary
        $weeklyWindow = $secondary
        if ($null -ne $primary -and [double]$primary.limit_window_seconds -ge 518400 -and $null -eq $secondary) {
            $currentWindow = $null
            $weeklyWindow = $primary
        }
        Write-UsageWindow -Name "Current window" -Window $currentWindow
        Write-UsageWindow -Name "Weekly" -Window $weeklyWindow
    } catch {
        Write-Warning "Could not read provider usage: $($_.Exception.Message)"
        Write-Host "  Current window: unavailable"
        Write-Host "  Weekly: unavailable"
    }
}

function Test-UsageLimitError {
    param([Parameter(Mandatory = $true)][string]$Text)

    return $Text -match '(?is)(usage limit|usage_limit_reached|usage cap|quota exceeded|insufficient_quota|out of credits|credit balance|billing limit|subscription limit|weekly limit|monthly limit|weighted tokens|token limit.*reset|rate limit.*reset|limit resets? at)'
}

function Test-ServerOrApiError {
    param([Parameter(Mandatory = $true)][string]$Text)

    return $Text -match '(?is)(HTTP\s*(408|409|425|429|5\d\d)|status\s*(408|409|425|429|5\d\d)|server error|internal server error|service unavailable|bad gateway|gateway timeout|overloaded|temporarily unavailable|request timeout|timed out|ECONNRESET|ECONNREFUSED|ENETUNREACH|EAI_AGAIN|socket hang up|connection reset|connection closed|fetch failed|network error|server_error|stream.*(closed|terminated))'
}

function Test-TicketComplete {
    param(
        [Parameter(Mandatory = $true)][string]$Repository,
        [Parameter(Mandatory = $true)][int]$Number,
        [Parameter(Mandatory = $true)][string]$StartingHead
    )

    $state = (Invoke-Gh @("issue", "view", [string]$Number, "--repo", $Repository, "--json", "state", "--jq", ".state")).Trim()
    $currentHead = (& git rev-parse HEAD).Trim()
    $trackedChanges = @(& git status --porcelain --untracked-files=no)
    return $state -eq "CLOSED" -and $currentHead -ne $StartingHead -and $trackedChanges.Count -eq 0
}

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw "gh is required but was not found on PATH."
}
if (-not (Get-Command $Agent -ErrorAction SilentlyContinue)) {
    throw "$Agent is required for -Agent $Agent but was not found on PATH."
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "git is required but was not found on PATH."
}
if ($InitialRetryIntervalSeconds -lt 1 -or $MaxRetryIntervalSeconds -lt $InitialRetryIntervalSeconds) {
    throw "Retry intervals must be positive and MaxRetryIntervalSeconds must be at least InitialRetryIntervalSeconds."
}
if ($UsagePollSeconds -lt 1) {
    throw "UsagePollSeconds must be positive."
}
if ($Quiet -and $VerbosePreference -eq "Continue") {
    throw "-Quiet and -Verbose cannot be used together."
}
if (-not $Model) {
    $Model = (Get-AgentModelPair -AgentName $Agent).Larger
}
if (-not (Test-EffortSupported -AgentName $Agent -EffortLevel $Effort)) {
    throw "Effort '$Effort' is not supported by $Agent. Use low, medium, high, xhigh, or max."
}

$repoRoot = (& git rev-parse --show-toplevel 2>$null).Trim()
if ($LASTEXITCODE -ne 0 -or -not $repoRoot) {
    throw "Run this script from inside a Git repository."
}
Set-Location $repoRoot
$initialTrackedChanges = @(& git status --porcelain --untracked-files=no)
if ($initialTrackedChanges.Count -gt 0) {
    throw "The tracked worktree is not clean. Commit or restore tracked changes before starting the loop."
}

if ($UseBranch) {
    $currentBranch = (& git rev-parse --abbrev-ref HEAD).Trim()
    if ($currentBranch -ne $UseBranch) {
        if ((Invoke-GitQuiet @("rev-parse", "--verify", "--quiet", "refs/heads/$UseBranch")) -eq 0) {
            $checkoutResult = Invoke-GitQuiet @("checkout", $UseBranch)
        } elseif ((Invoke-GitQuiet @("ls-remote", "--exit-code", "--heads", "origin", $UseBranch)) -eq 0) {
            $checkoutResult = Invoke-GitQuiet @("checkout", "-b", $UseBranch, "--track", "origin/$UseBranch")
        } else {
            $checkoutResult = Invoke-GitQuiet @("checkout", "-b", $UseBranch)
        }
        if ($checkoutResult -ne 0) {
            throw "Failed to check out branch '$UseBranch'."
        }
        Write-Status "Switched to branch '$UseBranch'."
    }
}

if (-not $Repo) {
    $Repo = (Invoke-Gh @("repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner")).Trim()
}
$currentUser = (Invoke-Gh @("api", "user", "--jq", ".login")).Trim()
$logDirectory = Join-Path ([System.IO.Path]::GetTempPath()) "$Agent-ralph-loop"
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null

while ($true) {
    $ticket = Get-NextTicket -Repository $Repo -Label $ReadyLabel -ExtraLabels $Labels -CurrentUser $currentUser
    if ($null -eq $ticket) {
        Write-Status "No unblocked, unclaimed '$ReadyLabel' tickets are available."
        break
    }

    $number = [int]$ticket.number
    Write-Status "Selected #${number}: $($ticket.title)"

    $ticketModel = $Model
    $ticketEffort = $Effort
    $selectionSource = "command line/default"
    if ($AdaptiveModelAndEffort) {
        $adaptiveSelection = Get-AdaptiveModelAndEffort -AgentName $Agent -Labels @($ticket.labels)
        $ticketModel = $adaptiveSelection.Model
        $ticketEffort = $adaptiveSelection.Effort
        $selectionSource = "adaptive difficulty:$($adaptiveSelection.Difficulty)"
    }
    Write-Host "Ticket #$number model: $ticketModel; effort: $ticketEffort ($selectionSource)."

    if ($DryRun) {
        Write-Status "Dry run: would start $Agent with model '$ticketModel' and effort '$ticketEffort'."
        break
    }

    if (@($ticket.assignees).Count -eq 0) {
        Invoke-Gh @("issue", "edit", [string]$number, "--repo", $Repo, "--add-assignee", "@me") | Out-Null
        Write-Status "Claimed #$number as $currentUser."
    }

    $ticketStartedAt = [DateTimeOffset]::Now
    $ticketTimestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $ticketSessionDirectory = Join-Path $logDirectory "issue-$number-$ticketTimestamp-sessions"
    $ticketSessionIds = @()
    if ($Agent -eq "pi") {
        New-Item -ItemType Directory -Force -Path $ticketSessionDirectory | Out-Null
    }
    Write-Status "Ticket #$number started at $($ticketStartedAt.ToString('o'))."

    $startingHead = (& git rev-parse HEAD).Trim()
    $originalPrompt = Get-TicketPrompt -Repository $Repo -Number $number
    $prompt = $originalPrompt
    $retryInterval = $InitialRetryIntervalSeconds
    $attempt = 0

    while ($true) {
        ++$attempt
        $attemptTimestamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $logPath = Join-Path $logDirectory "issue-$number-$attemptTimestamp-attempt-$attempt.log"
        Write-Status "Starting $Agent for #$number (attempt $attempt). Log: $logPath"

        if ($Agent -eq "pi") {
            $sessionDirectory = Join-Path $ticketSessionDirectory "attempt-$attempt"
            New-Item -ItemType Directory -Force -Path $sessionDirectory | Out-Null
            Write-Verbose "Session directory: $sessionDirectory"

            $agentArguments = @(
                "--print", "--approve", "--model", $ticketModel, "--thinking", $ticketEffort,
                "--name", "ralph-$number", "--session-dir", $sessionDirectory
            )
        } else {
            # Claude Code names sessions by id rather than taking a directory,
            # so each attempt gets its own id and the transcripts are found by
            # those ids afterwards.
            $sessionId = [guid]::NewGuid().ToString()
            $ticketSessionIds += $sessionId
            Write-Verbose "Session id: $sessionId"

            $agentArguments = @(
                "--print", "--dangerously-skip-permissions", "--model", $ticketModel,
                "--effort", $ticketEffort, "--session-id", $sessionId
            )
        }
        if ($VerbosePreference -eq "Continue") {
            $agentArguments += "--verbose"
        }
        $agentArguments += $prompt

        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = [System.Collections.Generic.List[string]]::new()
            & $Agent @agentArguments 2>&1 |
                ForEach-Object {
                    # Emit each line as it arrives instead of handing the host one
                    # screen-sized string after the agent exits. Splitting bare carriage
                    # returns also turns progress-style redraws into scrolling lines.
                    foreach ($line in ([string]$_ -split "`r`n|`n|`r")) {
                        Remove-TerminalControlSequences $line
                    }
                } |
                Tee-Object -FilePath $logPath -Append |
                ForEach-Object {
                    $line = [string]$_
                    [void]$output.Add($line)
                    if (-not $Quiet) {
                        Write-Host $line
                    }
                }
            $agentExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        $outputText = if (Test-Path $logPath) {
            Get-Content -Path $logPath -Raw
        } else {
            $output -join [Environment]::NewLine
        }

        # A provider can fail after the agent has already committed and closed.
        if (Test-TicketComplete -Repository $Repo -Number $number -StartingHead $startingHead) {
            Write-Status "Ticket #$number completed successfully."
            break
        }

        if ($agentExitCode -eq 0) {
            Write-Warning "$Agent exited successfully without completing #$number. Starting a recovery attempt using $logPath."
            $prompt = @"
$originalPrompt

## Recovery attempt

A previous agent attempt exited successfully without closing ticket #$number.
Inspect the previous attempt log at:

$logPath

Determine why that attempt did not complete the original request, then resolve
the issue recorded in the log and finish the original ticket. Continue from the
current worktree and repository state. Do not merely repeat the previous
explanation or stop after describing the blocker; try to resolve it and carry
the original request through implementation, verification, commit, and issue
closure.
"@
            continue
        }

        if (Test-UsageLimitError $outputText) {
            Write-Warning "Usage limit detected for #$number. Retrying in $UsagePollSeconds seconds."
            Start-Sleep -Seconds $UsagePollSeconds
            continue
        }

        if (Test-ServerOrApiError $outputText) {
            Write-Warning "Server/API failure detected for #$number. Retrying in $retryInterval seconds."
            Start-Sleep -Seconds $retryInterval
            $retryInterval = [Math]::Min($retryInterval * 2, $MaxRetryIntervalSeconds)
            continue
        }

        throw "$Agent failed for a non-retryable implementation reason on #$number. The issue remains assigned and open. Inspect $logPath."
    }

    $ticketEndedAt = [DateTimeOffset]::Now
    if ($Agent -eq "pi") {
        $sessionUsage = Get-PiSessionUsage -SessionDirectory $ticketSessionDirectory
        $usageSource = $ticketSessionDirectory
    } else {
        $sessionUsage = Get-ClaudeSessionUsage -SessionIds $ticketSessionIds
        $usageSource = "Claude Code sessions $($ticketSessionIds -join ', ')"
    }
    Show-TicketSummary -Number $number -StartedAt $ticketStartedAt -EndedAt $ticketEndedAt -SessionUsage $sessionUsage
    if ($null -eq $sessionUsage) {
        Write-Warning "Could not read current-ticket usage from $usageSource."
    } else {
        Show-ProviderUsage -SessionUsage $sessionUsage
    }

    if ($Once) {
        break
    }
}
