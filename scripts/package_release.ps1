param(
    [string]$Version = "0.5.6",
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
    if ($null -ne $fromPath) {
        return $fromPath.Source
    }

    $candidates = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 5\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 5\ISCC.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    return ""
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

function Invoke-PackagedCheck {
    param(
        [string]$ExecutablePath,
        [string]$WorkingDirectory,
        [string]$Argument
    )

    Remove-PathWithRetry -Path (Join-Path $WorkingDirectory "config")
    Remove-PathWithRetry -Path (Join-Path $WorkingDirectory "logs")

    Push-Location $WorkingDirectory
    try {
        & $env:ComSpec /d /c "`"$ExecutablePath`" $Argument"
        $exitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }

    if ($exitCode -ne 0) {
        throw "Packaged check failed ($Argument) with exit code $exitCode."
    }
}

$script:RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
Set-Location $script:RepoRoot

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

$licenseDir = Join-Path $stageDir "licenses"
New-Item -ItemType Directory -Force -Path $licenseDir | Out-Null
Copy-IfExists -Source (Resolve-RepoPath "resources/licenses/FluentUI-System-Icons-LICENSE.txt") -Destination (Join-Path $licenseDir "FluentUI-System-Icons-LICENSE.txt")

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

Write-Host "==> Verifying packaged executable"
Invoke-PackagedCheck -ExecutablePath $stageExe -WorkingDirectory $stageDir -Argument "--check-deps"
Invoke-PackagedCheck -ExecutablePath $stageExe -WorkingDirectory $stageDir -Argument "--smoke-test"
Invoke-PackagedCheck -ExecutablePath $stageExe -WorkingDirectory $stageDir -Argument "--ui-remote-smoke-test"
Invoke-PackagedCheck -ExecutablePath $stageExe -WorkingDirectory $stageDir -Argument "--ui-remote-workflow-smoke-test"

Start-Sleep -Seconds 2
Remove-PathWithRetry -Path (Join-Path $stageDir "config")
Remove-PathWithRetry -Path (Join-Path $stageDir "logs")

Write-Host "==> Creating zip $zipPath"
New-ZipWithRetry -SourcePath (Join-Path $stageDir "*") -DestinationPath $zipPath

$iscc = Find-InnoCompiler -ExplicitPath $InnoCompiler
if ($iscc -eq "") {
    Write-Warning "ISCC.exe was not found. Installer generation was skipped. Zip package: $zipPath"
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
