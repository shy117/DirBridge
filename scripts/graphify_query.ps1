param(
    [ValidateSet("Query", "Explain", "Path", "Affected")]
    [string]$Command = "Explain",

    [Parameter(Mandatory = $true)]
    [string]$Query,

    [string]$Target = "",

    [ValidateRange(100, 10000)]
    [int]$Budget = 1500,

    [ValidateRange(1, 10)]
    [int]$Depth = 2,

    [string[]]$Relation = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Find-GraphifyCommand {
    $command = Get-Command graphify -CommandType Application -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        $command = Get-Command graphify.exe -CommandType Application -ErrorAction SilentlyContinue
    }
    if ($null -eq $command) {
        throw "graphify was not found in PATH. Install the official graphifyy package first."
    }
    return $command.Source
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$graphPath = Join-Path $repoRoot "graphify-out/graph.json"

if (-not (Test-Path -LiteralPath $graphPath)) {
    throw "Graphify graph was not found. Run scripts/graphify_build.ps1 first."
}

if ($Command -eq "Query" -and $Query -notmatch "[A-Za-z_]") {
    Write-Warning "Pure Chinese Graphify queries may not match without the official chinese extra. Prefer a short query containing exact C++ symbols or English identifiers."
}

$graphify = Find-GraphifyCommand
$commandArguments = switch ($Command) {
    "Query" {
        @("query", $Query, "--budget", [string]$Budget, "--graph", $graphPath)
    }
    "Explain" {
        @("explain", $Query, "--graph", $graphPath)
    }
    "Path" {
        if ([string]::IsNullOrWhiteSpace($Target)) {
            throw "Path queries require -Target."
        }
        @("path", $Query, $Target, "--graph", $graphPath)
    }
    "Affected" {
        $affectedArguments = @(
            "affected",
            $Query,
            "--depth",
            [string]$Depth,
            "--graph",
            $graphPath
        )
        foreach ($item in $Relation) {
            $affectedArguments += @("--relation", $item)
        }
        $affectedArguments
    }
}

$commandOutput = @(& $graphify @commandArguments)
$exitCode = $LASTEXITCODE
$commandOutput | ForEach-Object { Write-Output $_ }

if ($exitCode -ne 0) {
    throw "graphify failed with exit code $exitCode."
}

$outputText = $commandOutput -join [Environment]::NewLine
if ($outputText -match "No unique node match") {
    throw "Graphify did not resolve a unique node. Use a canonical node ID or fall back to source search."
}
if ($outputText -match "No matching nodes found") {
    throw "Graphify found no matching nodes. Use exact C++ symbols or fall back to source search."
}
if ($outputText -match "(\d+) nodes found") {
    $matchedNodeCount = [int]$Matches[1]
    if ($matchedNodeCount -gt 50) {
        Write-Warning "Graphify returned $matchedNodeCount nodes. Treat the result as discovery only and fall back to exact symbols or source search."
    }
}
if ($outputText -match "truncated") {
    Write-Warning "Graphify output was truncated. Do not use it as a complete call chain or impact analysis."
}
