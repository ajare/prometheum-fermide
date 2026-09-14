<#
.SYNOPSIS
Validates that a labelled GitHub issue frontier is ready for ralph-loop.ps1.

.DESCRIPTION
Validates executable open tickets carrying one supplied label in the current
GitHub repository. Parent specification issues referenced by a ticket's
"## Parent" section are ignored, matching ralph-loop.ps1. The script requires
at least one executable ticket, ready-for-agent on every ticket, at least one
ticket with native GitHub blocked_by metadata, and supported difficulty and
priority labels on every ticket.

Difficulty and priority namespaces may use ':' or '/'. Supported difficulty
values are trivial, small, low, medium, large, high, and hard. This repository's
bare priority labels critical, high, medium, and low are accepted, as are the
aliases urgent, p0, p1, normal, p2, and p3 and namespaced non-negative integers.

.PARAMETER Label
The single label used to select open tickets, for example feature:portals.

.EXAMPLE
.\tools\pre_ralph_validate.ps1 -Label feature:portals

.EXAMPLE
.\tools\pre_ralph_validate.ps1 feature:portals
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateNotNullOrEmpty()]
    [string]$Label
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

function Add-TicketFailure {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [System.Collections.Generic.List[string]]$Failures,
        [Parameter(Mandatory = $true)]
        [string]$Message
    )

    $Failures.Add($Message)
}

try {
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
        throw "gh is required but was not found on PATH."
    }

    $repository = (Invoke-Gh @("repo", "view", "--json", "nameWithOwner", "--jq", ".nameWithOwner")).Trim()
    $json = Invoke-Gh @(
        "issue", "list", "--repo", $repository,
        "--state", "open", "--label", $Label,
        "--limit", "1000",
        "--json", "number,title,body,labels,url"
    )
    $parsedIssues = [object[]]($json | ConvertFrom-Json)

    if ($parsedIssues.Count -eq 0) {
        Write-Error "No open tickets carry label '$Label' in $repository." -ErrorAction Continue
        exit 1
    }

    # A feature's parent specification commonly carries the feature and ready
    # labels too, but ralph-loop deliberately does not execute it. Derive the
    # same parent set from child tickets before validating the frontier.
    $parentNumbers = @{}
    foreach ($issue in $parsedIssues) {
        if ([string]$issue.body -match '(?im)^## Parent\s*\r?\n+\s*#(\d+)') {
            $parentNumbers[[int]$Matches[1]] = $true
        }
    }
    $issues = @($parsedIssues | Where-Object { -not $parentNumbers.ContainsKey([int]$_.number) })

    if ($issues.Count -eq 0) {
        Write-Error "No executable open tickets carry label '$Label' in $repository." -ErrorAction Continue
        exit 1
    }

    $ignoredCount = $parsedIssues.Count - $issues.Count
    $ignoredText = if ($ignoredCount -gt 0) { " ($ignoredCount parent specification issue(s) ignored)" } else { "" }
    Write-Host "Validating $($issues.Count) executable open ticket(s) carrying '$Label' in $repository$ignoredText."

    $supportedDifficulties = @("trivial", "small", "low", "medium", "large", "high", "hard")
    $supportedPriorities = @("critical", "urgent", "p0", "high", "p1", "medium", "normal", "p2", "low", "p3")
    $failureCount = 0
    $blockedTicketCount = 0

    foreach ($issue in $issues) {
        $ticketFailures = [System.Collections.Generic.List[string]]::new()
        $labelNames = @($issue.labels | ForEach-Object { ([string]$_.name).Trim().ToLowerInvariant() })

        if ($labelNames -notcontains "ready-for-agent") {
            Add-TicketFailure $ticketFailures "missing ready-for-agent"
        }

        $difficultyCount = 0
        $priorityCount = 0
        foreach ($issueLabel in @($issue.labels)) {
            $name = ([string]$issueLabel.name).Trim()
            if ($name -match '^(?i:difficulty)[/:]\s*(.*)$') {
                ++$difficultyCount
                $value = $Matches[1].ToLowerInvariant()
                if ($value -notin $supportedDifficulties) {
                    Add-TicketFailure $ticketFailures "unsupported difficulty label '$name'"
                }
            }

            if ($name -match '^(?i:critical|urgent|p0|high|p1|medium|normal|p2|low|p3)$') {
                ++$priorityCount
            } elseif ($name -match '^(?i:priority)[/:]\s*(.*)$') {
                ++$priorityCount
                $value = $Matches[1].ToLowerInvariant()
                if ($value -notin $supportedPriorities -and $value -notmatch '^\d+$') {
                    Add-TicketFailure $ticketFailures "unsupported priority label '$name'"
                }
            }
        }

        if ($difficultyCount -eq 0) {
            Add-TicketFailure $ticketFailures "missing difficulty label"
        }
        if ($priorityCount -eq 0) {
            Add-TicketFailure $ticketFailures "missing priority label"
        }

        $blockedByText = Invoke-Gh @(
            "api", "repos/$repository/issues/$($issue.number)",
            "--jq", ".issue_dependencies_summary.blocked_by // 0"
        )
        $blockedBy = [int]$blockedByText.Trim()
        if ($blockedBy -gt 0) {
            ++$blockedTicketCount
        }

        if ($ticketFailures.Count -eq 0) {
            Write-Host "PASS #$($issue.number): $($issue.title) (blocked by $blockedBy)"
        } else {
            Write-Error "FAIL #$($issue.number): $($issue.title)" -ErrorAction Continue
            foreach ($failure in $ticketFailures) {
                Write-Error "  - $failure" -ErrorAction Continue
            }
            $failureCount += $ticketFailures.Count
        }
    }

    if ($blockedTicketCount -eq 0) {
        Write-Error "At least one matching ticket must have native GitHub blocked_by metadata." -ErrorAction Continue
        ++$failureCount
    }

    if ($failureCount -gt 0) {
        Write-Error "Validation failed with $failureCount problem(s)." -ErrorAction Continue
        exit 1
    }

    Write-Host "Validation passed: $($issues.Count) ticket(s), $blockedTicketCount blocked ticket(s)."
    exit 0
} catch {
    Write-Error $_.Exception.Message -ErrorAction Continue
    exit 2
}
