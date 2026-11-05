<#
.SYNOPSIS
    Build the Metalio E-Ink4-Plus firmware with plain ESP-IDF (no PlatformIO).

.DESCRIPTION
    Mirrors the reviewed build order: regenerate the i18n and HTML resources,
    derive the Plus JPEGDEC copy, reconfigure, build, then re-run every guard
    (IDF revision, source hashes, Wi-Fi security policy, web memory budget,
    linked touch backend, clock ownership).

.EXAMPLE
    ./scripts/build_plus.ps1
    ./scripts/build_plus.ps1 -BuildDir .local/build-debug -KeepSdkconfig
#>
param(
    [string]$BuildDir = "",
    [string]$IdfPath = "",
    [string]$Sdkconfig = "",
    [switch]$SkipChecks
)

# ESP-IDF tools write progress to stderr; "Stop" would treat each line as a
# failure. Every external call is followed by an explicit $LASTEXITCODE check.
$ErrorActionPreference = "Continue"

$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root "firmware/rickyos-plus-1.2"
if (-not $BuildDir) { $BuildDir = Join-Path $root ".local/build-rickyos-plus" }
if (-not $IdfPath) {
    # Prefer the machine-wide ESP-IDF installation, fall back to the in-repo one.
    $installed = "D:/esp/v6.1/esp-idf"
    $IdfPath = if (Test-Path (Join-Path $installed "export.ps1")) { $installed }
               else { Join-Path $root "tools/esp-idf-v6.1" }
}

$env:IDF_PATH = $IdfPath
$userTools = Join-Path $env:USERPROFILE ".espressif"
$env:IDF_TOOLS_PATH = if (Test-Path (Join-Path $userTools "python_env")) { $userTools }
                      else { Join-Path $root ".local/espressif" }
$env:IDF_COMPONENT_MANAGER = "0"
$env:IDF_TARGET = "esp32s31"
$env:PYTHONUTF8 = "1"
$gitUsrBin = "C:\Program Files\Git\usr\bin"
if ((Test-Path $gitUsrBin) -and ($env:PATH -notlike "*$gitUsrBin*")) {
    $env:PATH = "$gitUsrBin;$env:PATH"
}

python (Join-Path $root "scripts/verify_plus_release_idf.py") --idf $IdfPath
if ($LASTEXITCODE) { throw "ESP-IDF is not the reviewed release/v6.1 commit" }
python (Join-Path $root "scripts/check_rickyos_plus_sources.py")
if ($LASTEXITCODE) { throw "Restored source snapshot is not the reviewed one" }

python (Join-Path $root "scripts/prepare_plus_jpeg.py")
if ($LASTEXITCODE) { throw "Preparing the Plus JPEGDEC copy failed" }

# Dot-source so idf.py and PATH land in this scope; export.ps1 writes its banner to
# stderr, which would abort under $ErrorActionPreference = "Stop".
$savedPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
. "$IdfPath/export.ps1" 2>&1 | Out-Null
$ErrorActionPreference = $savedPreference

python -u (Join-Path $root "scripts/prepare_plus_usb.py")
if ($LASTEXITCODE) { throw "Preparing TinyUSB sources failed" }

# Resource generation needs zopfli/pillow/numpy, which live in the local venv.
$resourcePython = Join-Path $root ".local/venv/Scripts/python.exe"
if (-not (Test-Path $resourcePython)) { $resourcePython = "python" }
Write-Host "== Resources ==" -ForegroundColor Cyan
Push-Location (Join-Path $project "upstream")
try {
    & $resourcePython scripts/gen_i18n.py lib/I18n/translations ../generated --src-dirs src lib --strip-unused --builtin-langs en,zh_cn
    if ($LASTEXITCODE) { throw "i18n generation failed" }
    & $resourcePython scripts/build_html.py
    if ($LASTEXITCODE) { throw "HTML resource generation failed" }
}
finally { Pop-Location }
& $resourcePython (Join-Path $root "scripts/compress_plus_builtin_fonts.py")
if ($LASTEXITCODE) { throw "Font packing failed" }
if (-not $SkipChecks) {
    & $resourcePython (Join-Path $root "scripts/check_plus_font_resources.py")
    if ($LASTEXITCODE) { throw "Font resources are missing or stale" }
}

Write-Host "== Configure ==" -ForegroundColor Cyan
Push-Location $project
try {
    $idfArgs = @("-B", $BuildDir)
    if ($Sdkconfig) {
        $idfArgs += "-D"
        $idfArgs += "SDKCONFIG=$Sdkconfig"
    }
    idf.py @idfArgs reconfigure
    if ($LASTEXITCODE) { throw "reconfigure failed" }

    python (Join-Path $root "scripts/check_plus_wifi_security.py") (Join-Path $BuildDir "config/sdkconfig.h")
    if ($LASTEXITCODE) { throw "Wi-Fi security policy check failed" }

    Write-Host "== Build ==" -ForegroundColor Cyan
    idf.py @idfArgs build
    if ($LASTEXITCODE) { throw "build failed" }

    python (Join-Path $root "scripts/check_plus_web_memory.py") (Join-Path $BuildDir "config/sdkconfig.h")
    if ($LASTEXITCODE) { throw "Web memory budget check failed" }
    python (Join-Path $root "scripts/verify_plus_release_idf.py") --idf $IdfPath --project (Join-Path $BuildDir "project_description.json")
    if ($LASTEXITCODE) { throw "Build description does not match the reviewed ESP-IDF" }
    python (Join-Path $root "scripts/check_rickyos_plus_sources.py")
    if ($LASTEXITCODE) { throw "Sources changed during the build" }

    # Same link-time guard as the reviewed build script: the Plus touch backend
    # must actually be reached, not silently dropped by the linker.
    python (Join-Path $root "tests/check_plus_touch_symbol.py") (Join-Path $BuildDir "RickyOS-Plus-1.2.elf")
    if ($LASTEXITCODE) { throw "Linked application is missing the Plus touch backend" }
    python (Join-Path $root "tests/check_plus_clock_objects.py")
    if ($LASTEXITCODE) { throw "Clock ownership check failed" }
}
finally { Pop-Location }

$bin = Join-Path $BuildDir "RickyOS-Plus-1.2.bin"
if (Test-Path $bin) {
    $size = (Get-Item $bin).Length
    $slot = 0x640000
    Write-Host ("`nApplication image: {0:N0} bytes; slot 0x640000 leaves {1:N0} bytes ({2:P1} used)" -f $size, ($slot - $size), ($size / $slot)) -ForegroundColor Green
    if ($size -gt $slot) { throw "Application image exceeds the OTA slot" }
}
Write-Host "Flash with: ./scripts/flash_plus.ps1 -Port COMx" -ForegroundColor Green
