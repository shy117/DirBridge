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

function Invoke-Graphify {
    param(
        [string]$Executable,
        [string[]]$Arguments
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "graphify failed with exit code $LASTEXITCODE`: $($Arguments -join ' ')"
    }
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$ignorePath = Join-Path $PSScriptRoot "graphify/graphify.ignore"
$activeIgnorePath = Join-Path $repoRoot ".graphifyignore"
$graphPath = Join-Path $repoRoot "graphify-out/graph.json"
$manifestPath = Join-Path $repoRoot "graphify-out/manifest.json"
$labelsPath = Join-Path $repoRoot "graphify-out/.graphify_labels.json"
$labelSignaturesPath = Join-Path $repoRoot "graphify-out/.graphify_labels.json.sig"

if (-not (Test-Path -LiteralPath $ignorePath)) {
    throw "Graphify ignore configuration was not found: $ignorePath"
}

$graphify = Find-GraphifyCommand
$originalLocation = (Get-Location).Path
$activeIgnoreExisted = Test-Path -LiteralPath $activeIgnorePath
$activeIgnoreBytes = if ($activeIgnoreExisted) {
    [System.IO.File]::ReadAllBytes($activeIgnorePath)
}
else {
    $null
}

try {
    [System.IO.File]::WriteAllBytes(
        $activeIgnorePath,
        [System.IO.File]::ReadAllBytes($ignorePath)
    )
    Set-Location $repoRoot

    Write-Host "==> Graphify version"
    Invoke-Graphify -Executable $graphify -Arguments @("--version")

    Write-Host "==> Building Graphify graph"
    Invoke-Graphify -Executable $graphify -Arguments @(
        "extract",
        ".",
        "--code-only",
        # Custom output directories produced incomplete incremental graphs in
        # Graphify 0.9.20, so every build must be a complete extraction.
        "--force",
        "--no-cluster"
    )

    if (-not (Test-Path -LiteralPath $graphPath)) {
        throw "Graphify output was not found: $graphPath"
    }

    # Graphify reuses saved labels even when the graph is rebuilt. Remove old
    # labels so the official clusterer regenerates deterministic names from
    # each community's highest-degree symbol.
    foreach ($labelCachePath in @($labelsPath, $labelSignaturesPath)) {
        if (Test-Path -LiteralPath $labelCachePath) {
            Remove-Item -LiteralPath $labelCachePath -Force
        }
    }

    Write-Host "==> Clustering Graphify graph with readable community labels"
    Invoke-Graphify -Executable $graphify -Arguments @(
        "cluster-only",
        $repoRoot,
        "--graph",
        $graphPath,
        # Keep hub-based names local even if an API key is added later.
        "--backend="
    )

    Write-Host "==> Diagnosing Graphify graph"
    Invoke-Graphify -Executable $graphify -Arguments @(
        "diagnose",
        "multigraph",
        "--graph",
        $graphPath
    )

    $graphJson = Get-Content -Raw -Encoding utf8 -LiteralPath $graphPath
    $graph = $graphJson | ConvertFrom-Json
    if (-not (Test-Path -LiteralPath $labelsPath)) {
        throw "Graphify community labels were not found: $labelsPath"
    }
    $communityLabels = Get-Content -Raw -Encoding utf8 -LiteralPath $labelsPath | ConvertFrom-Json
    $communityLabelProperties = @($communityLabels.PSObject.Properties)
    $placeholderLabelCount = @(
        $communityLabelProperties | Where-Object { [string]$_.Value -match "^Community \d+$" }
    ).Count
    $nodeCount = @($graph.nodes).Count
    $edgeCount = @($graph.links).Count
    $documentNodeCount = @(
        $graph.nodes | Where-Object { $_.file_type -eq "document" }
    ).Count
    $sourceLessNodeCount = @(
        $graph.nodes | Where-Object { [string]::IsNullOrWhiteSpace([string]$_.source_file) }
    ).Count

    if ($documentNodeCount -ne 0) {
        throw "Graphify graph contains $documentNodeCount document nodes; expected code-only output."
    }
    if ($communityLabelProperties.Count -eq 0) {
        throw "Graphify community labels are empty."
    }
    if ($placeholderLabelCount -ne 0) {
        throw "Graphify contains $placeholderLabelCount placeholder community labels; expected readable hub labels."
    }
    if ($graphJson.Contains($repoRoot)) {
        throw "Graphify graph contains the absolute repository path."
    }
    if (-not (Test-Path -LiteralPath $manifestPath)) {
        throw "Graphify manifest was not found: $manifestPath"
    }

    Write-Host "==> Graphify graph ready"
    Write-Host "    Graph: $graphPath"
    Write-Host "    Nodes: $nodeCount"
    Write-Host "    Edges: $edgeCount"
    Write-Host "    Community labels: $($communityLabelProperties.Count)"
    Write-Host "    Placeholder labels: $placeholderLabelCount"
    Write-Host "    Source-less nodes: $sourceLessNodeCount"
    Write-Host "    Document nodes: $documentNodeCount"
}
finally {
    Set-Location $originalLocation
    if ($activeIgnoreExisted) {
        [System.IO.File]::WriteAllBytes($activeIgnorePath, $activeIgnoreBytes)
    }
    elseif (Test-Path -LiteralPath $activeIgnorePath) {
        Remove-Item -LiteralPath $activeIgnorePath -Force
    }
}
