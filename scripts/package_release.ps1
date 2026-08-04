param(
    [string]$Version = "",
    [string]$BuildPreset = "windows-mingw-release",
    [string]$BuildDir = "build/windows-mingw-release",
    [string]$ReleaseDir = "build/release",
    [string]$InnoCompiler = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepoPath {
    param([string]$RelativePath)
    return Join-Path $script:RepoRoot $RelativePath
}

function Get-ProjectVersionFromCMake {
    $cmakeFile = Resolve-RepoPath "CMakeLists.txt"
    $content = Get-Content -LiteralPath $cmakeFile -Raw -Encoding utf8
    $match = [regex]::Match($content, 'project\s*\(\s*DirBridge\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)', [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
    if (-not $match.Success) {
        throw "Failed to read project version from CMakeLists.txt."
    }

    return $match.Groups[1].Value
}

function Copy-IfExists {
    param(
        [string]$Source,
        [string]$Destination
    )

    if (Test-Path -LiteralPath $Source) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
    }
}

function Find-InnoCompiler {
    param([string]$ExplicitPath)

    if ($ExplicitPath -ne "") {
        if (Test-Path -LiteralPath $ExplicitPath) {
            return (Resolve-Path -LiteralPath $ExplicitPath).Path
        }

        throw "The specified Inno Setup compiler was not found: $ExplicitPath"
    }

    $fromPath = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($null -eq $fromPath) {
        return ""
    }
    return $fromPath.Source
}

function New-ZipWithRetry {
    param(
        [string]$SourcePath,
        [string]$DestinationPath
    )

    for ($attempt = 1; $attempt -le 5; $attempt++) {
        try {
            if (Test-Path -LiteralPath $DestinationPath) {
                Remove-Item -LiteralPath $DestinationPath -Force
            }

            Compress-Archive -Path $SourcePath -DestinationPath $DestinationPath -Force
            return
        }
        catch {
            if ($attempt -eq 5) {
                throw
            }

            Write-Warning "Zip attempt $attempt failed: $($_.Exception.Message)"
            Start-Sleep -Seconds 2
        }
    }
}

function Remove-PathWithRetry {
    param([string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    for ($attempt = 1; $attempt -le 5; $attempt++) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force
            return
        }
        catch {
            if ($attempt -eq 5) {
                throw
            }

            Write-Warning ("Remove attempt {0} failed for {1}: {2}" -f $attempt, $Path, $_.Exception.Message)
            Start-Sleep -Seconds 2
        }
    }
}

$script:RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
Set-Location $script:RepoRoot

if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = Get-ProjectVersionFromCMake
}

$releaseRoot = Resolve-RepoPath $ReleaseDir
$packageName = "DirBridge-v$Version-win64"
$stageDir = Join-Path $releaseRoot $packageName
$zipPath = Join-Path $releaseRoot "$packageName.zip"
$buildOutputDir = Resolve-RepoPath $BuildDir

Write-Host "==> Building $BuildPreset"
cmake --preset $BuildPreset
cmake --build --preset $BuildPreset

$sourceExe = Join-Path $buildOutputDir "DirBridge.exe"
if (-not (Test-Path -LiteralPath $sourceExe)) {
    throw "Build output was not found: $sourceExe"
}

Write-Host "==> Preparing release directory $stageDir"
New-Item -ItemType Directory -Force -Path $releaseRoot | Out-Null
if (Test-Path -LiteralPath $stageDir) {
    Remove-Item -LiteralPath $stageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

foreach ($fileName in @(
    "DirBridge.exe",
    "DirBridgeTerminalBroker.exe",
    "DirBridgeSshAskPass.exe",
    "ghostty-vt.dll",
    "libcurl-x64.dll",
    "curl-ca-bundle.crt",
    "libgcc_s_seh-1.dll",
    "libstdc++-6.dll",
    "libwinpthread-1.dll"
)) {
    Copy-IfExists -Source (Join-Path $buildOutputDir $fileName) -Destination (Join-Path $stageDir $fileName)
}

$stageExe = Join-Path $stageDir "DirBridge.exe"
$windeployqt = Get-Command windeployqt.exe -ErrorAction SilentlyContinue
if ($null -eq $windeployqt) {
    throw "windeployqt.exe was not found. Add the Qt bin directory to PATH first."
}

Write-Host "==> Deploying Qt runtime"
& $windeployqt.Source --compiler-runtime --no-translations $stageExe

foreach ($docName in @("README.md", "CHANGELOG.md", "RELEASE.md", "LICENSE", "THIRD_PARTY_NOTICES.md")) {
    Copy-IfExists -Source (Resolve-RepoPath $docName) -Destination (Join-Path $stageDir $docName)
}

$docsImageSource = Resolve-RepoPath "docs/images"
if (Test-Path -LiteralPath $docsImageSource) {
    $docsImageDestination = Join-Path $stageDir "docs/images"
    New-Item -ItemType Directory -Force -Path $docsImageDestination | Out-Null
    Get-ChildItem -LiteralPath $docsImageSource -File |
        ForEach-Object {
            Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $docsImageDestination $_.Name) -Force
        }
}

Copy-IfExists -Source (Resolve-RepoPath "installer/LICENSE.zh-CN.txt") -Destination (Join-Path $stageDir "LICENSE.zh-CN.txt")

$licenseDir = Join-Path $stageDir "licenses"
New-Item -ItemType Directory -Force -Path $licenseDir | Out-Null
Copy-IfExists -Source (Resolve-RepoPath "resources/licenses/FluentUI-System-Icons-LICENSE.txt") -Destination (Join-Path $licenseDir "FluentUI-System-Icons-LICENSE.txt")
Copy-IfExists -Source (Resolve-RepoPath "third_party/installed/ghostty-vt/licenses/Ghostty-LICENSE.txt") -Destination (Join-Path $licenseDir "Ghostty-LICENSE.txt")

$thirdPartySource = Resolve-RepoPath "third_party/_source"
if (Test-Path -LiteralPath $thirdPartySource) {
    $licensePatterns = @("LICENSE*", "COPYING*")
    foreach ($pattern in $licensePatterns) {
        Get-ChildItem -LiteralPath $thirdPartySource -Recurse -File -Filter $pattern -ErrorAction SilentlyContinue |
            ForEach-Object {
                $relative = $_.FullName.Substring($thirdPartySource.Length).TrimStart([char[]]@(92, 47))
                $destination = Join-Path (Join-Path $licenseDir "third_party") $relative
                Copy-IfExists -Source $_.FullName -Destination $destination
            }
    }
}

Write-Host "==> Creating zip $zipPath"
New-ZipWithRetry -SourcePath (Join-Path $stageDir "*") -DestinationPath $zipPath

$iscc = Find-InnoCompiler -ExplicitPath $InnoCompiler
if ($iscc -eq "") {
    Write-Warning "ISCC.exe was not found in PATH. Installer generation was skipped. Zip package: $zipPath"
}
else {
    $installerPath = Join-Path $releaseRoot "DirBridge-v$Version-win64-setup.exe"
    Remove-PathWithRetry -Path $installerPath
    Write-Host "==> Building installer with $iscc"
    & $iscc "/DAppVersion=$Version" "/DReleaseDir=$stageDir" "/DOutputDir=$releaseRoot" (Resolve-RepoPath "installer/DirBridge.iss")
    if ($LASTEXITCODE -ne 0) {
        throw "Installer generation failed with exit code $LASTEXITCODE."
    }
    Write-Host "    $installerPath"
}

Write-Host "==> Release package ready:"
Write-Host "    $stageDir"
Write-Host "    $zipPath"
