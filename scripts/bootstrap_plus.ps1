<#
.SYNOPSIS
    Prepare everything the Metalio E-Ink4-Plus firmware needs, without PlatformIO.

.DESCRIPTION
    Three independent stages, each skipped when already present:

      1. ESP-IDF release/v6.1 at the reviewed commit 9a97f6c5, installed for
         target esp32s31 (toolchain, Python env) under tools/ and .local/espressif.
      2. Third-party libraries under .local/ (see scripts/fetch_plus_deps.py),
         plus the reviewed SdFat/JPEGDEC/wolfSSL patches.
      3. TinyUSB and esp-usb sources, which need the ESP-IDF Python environment.

    Everything lands in git-ignored directories; no file in the repository is
    modified apart from the generated/ folder.

.EXAMPLE
    ./scripts/bootstrap_plus.ps1
    ./scripts/bootstrap_plus.ps1 -SkipIdf -SkipArduino      # libraries only
#>
param(
    [switch]$SkipIdf,
    [switch]$SkipInstalledIdf,
    [switch]$SkipDeps,
    [switch]$SkipUsb,
    [switch]$SkipArduino,
    [switch]$SkipResources,
    [string]$IdfPath = ""
)

# ESP-IDF tools write progress to stderr; "Stop" would treat each line as a
# failure. Every external call is followed by an explicit $LASTEXITCODE check.
$ErrorActionPreference = "Continue"
Set-StrictMode -Version Latest

$root = Split-Path -Parent $PSScriptRoot
$idfSha = "9a97f6c54ec638111ce55cd36581b3c192f15207"
if (-not $IdfPath) {
    # Reuse the machine-wide ESP-IDF when one is installed (D:\esp from the
    # ESP-IDF Installation Manager) instead of downloading a second toolchain.
    $installed = "D:/esp/v6.1/esp-idf"
    $IdfPath = if (Test-Path (Join-Path $installed "export.ps1")) { $installed }
               else { Join-Path $root "tools/esp-idf-v6.1" }
}

$env:PYTHONUTF8 = "1"
$userTools = Join-Path $env:USERPROFILE ".espressif"
$env:IDF_TOOLS_PATH = if (Test-Path (Join-Path $userTools "python_env")) { $userTools }
                      else { Join-Path $root ".local/espressif" }
# The reviewed dependency patches are applied with GNU patch/git apply.
$gitUsrBin = "C:\Program Files\Git\usr\bin"
if ((Test-Path $gitUsrBin) -and ($env:PATH -notlike "*$gitUsrBin*")) {
    $env:PATH = "$gitUsrBin;$env:PATH"
}

if (-not $SkipIdf) {
    Write-Host "== ESP-IDF ==" -ForegroundColor Cyan
    if (-not (Test-Path (Join-Path $IdfPath ".git"))) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $IdfPath) | Out-Null
        git init --quiet $IdfPath
        if ($LASTEXITCODE) { throw "git init failed" }
        git -C $IdfPath fetch --depth=1 https://github.com/espressif/esp-idf.git $idfSha
        if ($LASTEXITCODE) { throw "Fetching ESP-IDF $idfSha failed" }
        git -C $IdfPath checkout --quiet --detach FETCH_HEAD
        if ($LASTEXITCODE) { throw "Checking out ESP-IDF failed" }
    }
    # ESP-IDF ships mbedtls, lwip, the radio libraries and more as submodules;
    # without them export.ps1 and the build fail. They are pinned by the commit,
    # so a shallow recursive update is deterministic.
    Write-Host "Fetching ESP-IDF submodules (pinned by the commit)" -ForegroundColor Cyan
    git -C $IdfPath -c core.autocrlf=false submodule update --init --recursive --depth=1
    if ($LASTEXITCODE) { throw "Fetching ESP-IDF submodules failed" }
    $env:IDF_PATH = $IdfPath
    python (Join-Path $root "scripts/verify_plus_release_idf.py") --idf $IdfPath
    if ($LASTEXITCODE) { throw "ESP-IDF revision check failed" }
}

$env:IDF_PATH = $IdfPath
if (-not $SkipInstalledIdf -and (Test-Path (Join-Path $env:IDF_TOOLS_PATH "python_env"))) {
    Write-Host "== ESP-IDF tools already installed; skipping install.ps1 ==" -ForegroundColor DarkGray
    $SkipInstalledIdf = $true
}
if (-not $SkipInstalledIdf) {
    Write-Host "== ESP-IDF tools for esp32s31 ==" -ForegroundColor Cyan
    Push-Location $IdfPath
    try {
        & (Join-Path $IdfPath "install.ps1") esp32s31
        if ($LASTEXITCODE) { throw "ESP-IDF install.ps1 failed" }
    }
    finally { Pop-Location }
}

if (-not $SkipDeps) {
    Write-Host "== Third-party libraries ==" -ForegroundColor Cyan
    $args = @("-u", (Join-Path $root "scripts/fetch_plus_deps.py"))
    if ($SkipArduino) { $args += "--skip-arduino" }
    python @args
    if ($LASTEXITCODE) { throw "Dependency preparation failed" }
}

if (-not $SkipResources) {
    Write-Host "== Resource tooling (font packing, HTML) ==" -ForegroundColor Cyan
    $venv = Join-Path $root ".local/venv"
    if (-not (Test-Path (Join-Path $venv "Scripts/python.exe"))) {
        python -m venv $venv
        if ($LASTEXITCODE) { throw "Creating the resource virtual environment failed" }
    }
    & (Join-Path $venv "Scripts/python.exe") -m pip install --quiet --disable-pip-version-check -r (Join-Path $root "scripts/requirements-assets.txt")
    if ($LASTEXITCODE) { throw "Installing resource requirements failed" }
}

if (-not $SkipUsb) {
    Write-Host "== TinyUSB / esp-usb ==" -ForegroundColor Cyan
    $env:IDF_PATH = $IdfPath
    $env:IDF_COMPONENT_MANAGER = "1"
    # export.ps1 prints its banner on stderr; tolerate it under "Stop".
$savedPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
. "$IdfPath/export.ps1" 2>&1 | Out-Null
$ErrorActionPreference = $savedPreference
    python -u (Join-Path $root "scripts/prepare_plus_usb.py")
    if ($LASTEXITCODE) { throw "USB dependency preparation failed" }
}

Write-Host "`nBootstrap complete. Build with: ./scripts/build_plus.ps1" -ForegroundColor Green
