param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$LockPath = Join-Path $Root "deps.lock.json"
$ThirdParty = Join-Path $Root "third_party"
$Downloads = Join-Path $ThirdParty "_downloads"
$Source = Join-Path $ThirdParty "_source"
$Installed = Join-Path $ThirdParty "installed"

function Ensure-Directory {
    param([string]$Path)
    New-Item -ItemType Directory -Force $Path | Out-Null
}

function Download-File {
    param(
        [string]$Url,
        [string]$OutFile
    )

    if ((Test-Path $OutFile) -and -not $Force) {
        Write-Host "Using cached package: $OutFile"
        return
    }

    Write-Host "Downloading $Url"
    Invoke-WebRequest -Uri $Url -OutFile $OutFile
}

function Expand-Package {
    param(
        [string]$PackagePath,
        [string]$Destination,
        [string]$ExpectedSourceDir
    )

    $ExpectedPath = Join-Path $Destination $ExpectedSourceDir
    if ((Test-Path $ExpectedPath) -and -not $Force) {
        Write-Host "Using existing source: $ExpectedPath"
        return
    }

    if (Test-Path $ExpectedPath) {
        Remove-Item -Recurse -Force $ExpectedPath
    }

    Write-Host "Extracting $PackagePath"
    Expand-Archive -Force $PackagePath $Destination
}

function Copy-DirectoryContent {
    param(
        [string]$SourcePath,
        [string]$DestinationPath
    )

    Ensure-Directory $DestinationPath
    Copy-Item -Recurse -Force (Join-Path $SourcePath "*") $DestinationPath
}

Ensure-Directory $Downloads
Ensure-Directory $Source
Ensure-Directory $Installed

$Lock = Get-Content -Raw -Encoding UTF8 $LockPath | ConvertFrom-Json

$curl = $Lock.dependencies.curl
$curlPackage = Join-Path $Downloads $curl.package
Download-File $curl.url $curlPackage
Expand-Package $curlPackage $Source $curl.sourceDir
$curlSource = Join-Path $Source $curl.sourceDir
$curlInstall = Join-Path $Installed $curl.installDir
Ensure-Directory (Join-Path $curlInstall "include")
Ensure-Directory (Join-Path $curlInstall "lib")
Ensure-Directory (Join-Path $curlInstall "bin")
Copy-DirectoryContent (Join-Path $curlSource "include") (Join-Path $curlInstall "include")
Copy-Item -Force (Join-Path $curlSource "lib/libcurl.dll.a") (Join-Path $curlInstall "lib/libcurl.dll.a")
Copy-Item -Force (Join-Path $curlSource "bin/libcurl-x64.dll") (Join-Path $curlInstall "bin/libcurl-x64.dll")
Copy-Item -Force (Join-Path $curlSource "bin/curl-ca-bundle.crt") (Join-Path $curlInstall "bin/curl-ca-bundle.crt")

$json = $Lock.dependencies.nlohmann_json
$jsonPackage = Join-Path $Downloads $json.package
Download-File $json.url $jsonPackage
Expand-Package $jsonPackage $Source $json.sourceDir
$jsonSource = Join-Path $Source $json.sourceDir
$jsonInstall = Join-Path $Installed $json.installDir
Ensure-Directory (Join-Path $jsonInstall "include")
Copy-DirectoryContent (Join-Path $jsonSource "single_include") (Join-Path $jsonInstall "include")

$spdlog = $Lock.dependencies.spdlog
$spdlogPackage = Join-Path $Downloads $spdlog.package
Download-File $spdlog.url $spdlogPackage
Expand-Package $spdlogPackage $Source $spdlog.sourceDir
$spdlogSource = Join-Path $Source $spdlog.sourceDir
$spdlogInstall = Join-Path $Installed $spdlog.installDir
Ensure-Directory (Join-Path $spdlogInstall "include")
Copy-DirectoryContent (Join-Path $spdlogSource "include") (Join-Path $spdlogInstall "include")

Write-Host "Third-party dependencies are ready in: $Installed"
