param(
    [string]$SourceRoot = ""
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$lock = Get-Content -Raw -Encoding utf8 -LiteralPath (Join-Path $repoRoot "deps.lock.json") |
    ConvertFrom-Json
$dependency = $lock.dependencies.ghostty_vt

if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
    $SourceRoot = Join-Path $repoRoot (
        "build/_deps/libghostty-vt-{0}/zig-out-gnu" -f $dependency.commit)
}

$sourceDll = Join-Path $SourceRoot "bin/ghostty-vt.dll"
$sourceHeaders = Join-Path $SourceRoot "include/ghostty"
$sourceLicense = Join-Path (Split-Path -Parent $SourceRoot) "LICENSE"

foreach ($required in @($sourceDll, $sourceHeaders, $sourceLicense)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Ghostty VT verified artifact is missing: $required"
    }
}

$actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sourceDll).Hash
if ($actualHash -ne $dependency.dllSha256) {
    throw "ghostty-vt.dll SHA-256 mismatch. Expected $($dependency.dllSha256), got $actualHash"
}

$installRoot = Join-Path $repoRoot "third_party/installed/$($dependency.installDir)"
$includeRoot = Join-Path $installRoot "include"
$binRoot = Join-Path $installRoot "bin"
$licenseRoot = Join-Path $installRoot "licenses"
New-Item -ItemType Directory -Force -Path $includeRoot, $binRoot, $licenseRoot | Out-Null

Copy-Item -Recurse -Force -LiteralPath $sourceHeaders -Destination $includeRoot
Copy-Item -Force -LiteralPath $sourceDll -Destination (Join-Path $binRoot "ghostty-vt.dll")
Copy-Item -Force -LiteralPath $sourceLicense -Destination (Join-Path $licenseRoot "Ghostty-LICENSE.txt")

Write-Host "Installed verified Ghostty VT DLL to $installRoot"
