<#
.SYNOPSIS
    Flash the Metalio E-Ink4-Plus firmware over USB.

.DESCRIPTION
    Two modes:

      app   (default) writes only the application image, which keeps the existing
            bootloader, partition table and the other OTA slot untouched. Use it
            for every routine update.
      full  rewrites bootloader + partition table + application. This is a
            first-install / rescue path: it needs the exact board variant and a
            verified full-chip backup, so it stays behind -IHaveFullBackup.

    The chip is checked before anything is written, so a Pico or a plain
    E-Ink4 (ESP32-S3) can never be written with this image by accident.

    The write strategy is automatic. Behind a USB hub this board's
    USB-Serial/JTAG link drops and re-enumerates after roughly 650-740 KB of
    continuous transfer, so a single write-flash of the 5.5 MB application dies
    part way through with "Cannot configure port" / PermissionError(13).
    -Strategy auto writes the whole image in one pass and, only if that drops,
    splits it on erase-sector boundaries and rewrites it chunk by chunk, each
    chunk in its own esptool session and each verified by esptool. A direct host
    port needs ~52 s; the chunked fallback needs ~103 s. Force either path with
    -Strategy single or -Strategy chunked.

    Offsets and flash parameters are read from the build's flasher_args.json,
    so nothing here hardcodes an address.

.EXAMPLE
    ./scripts/flash_plus.ps1 -Port COM5
    ./scripts/flash_plus.ps1 -Port COM5 -Monitor
    ./scripts/flash_plus.ps1 -Port COM5 -Baud 460800 -ChunkSize 262144
    ./scripts/flash_plus.ps1 -Port COM5 -Mode full -IHaveFullBackup
#>
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [ValidateSet("app", "full")][string]$Mode = "app",
    [string]$BuildDir = "",
    [string]$IdfPath = "",
    [switch]$IHaveFullBackup,
    [switch]$Monitor,
    # USB-CDC throughput is fixed by the link, not the nominal rate: 460800 and
    # 921600 both need ~113 s for the 5.5 MB image. Keep the fast rate so retries
    # actually have room to step down.
    [int]$Baud = 921600,
    # Aligned to the 4 KiB erase sector; the value the USB link tolerates here.
    [int]$ChunkSize = 524288,
    [int]$Retries = 3,
    [int]$ResetDelayMs = 3000,
    # auto    write the whole image in one session and fall back to chunks if the
    #         link drops. 52 s on a direct USB port, still works behind a hub.
    # chunked always split, ~103 s. single never split, fails behind a hub.
    [ValidateSet("auto", "chunked", "single")][string]$Strategy = "auto",
    [switch]$NoChunk
)

# Kept for callers that predate -Strategy.
if ($NoChunk) { $Strategy = "single" }

# ESP-IDF tools write progress to stderr; "Stop" would treat each line as a
# failure. Every external call is followed by an explicit $LASTEXITCODE check.
$ErrorActionPreference = "Continue"

$root = Split-Path -Parent $PSScriptRoot
$project = Join-Path $root "firmware/rickyos-plus-1.2"
if (-not $BuildDir) { $BuildDir = Join-Path $root ".local/build-rickyos-plus" }
if (-not $IdfPath) {
    $installed = "D:/esp/v6.1/esp-idf"
    $IdfPath = if (Test-Path (Join-Path $installed "export.ps1")) { $installed }
               else { Join-Path $root "tools/esp-idf-v6.1" }
}

$env:IDF_PATH = $IdfPath
$userTools = Join-Path $env:USERPROFILE ".espressif"
$env:IDF_TOOLS_PATH = if (Test-Path (Join-Path $userTools "python_env")) { $userTools }
                      else { Join-Path $root ".local/espressif" }
$env:PYTHONUTF8 = "1"

$flasherArgs = Join-Path $BuildDir "flasher_args.json"
if (-not (Test-Path $flasherArgs)) { throw "No flasher_args.json at $flasherArgs; run ./scripts/build_plus.ps1 first" }
$spec = Get-Content $flasherArgs -Raw | ConvertFrom-Json
$writeArgs = @($spec.write_flash_args)

# The sector is 4 KiB; a chunk that is not a multiple of it would let the erase
# of the next chunk reach back into the previous one.
if ($ChunkSize -lt 4096) { $ChunkSize = 4096 }
if ($ChunkSize % 4096) {
    $aligned = [int]([Math]::Floor($ChunkSize / 4096) * 4096)
    Write-Host "ChunkSize $ChunkSize is not a multiple of 4096; using $aligned." -ForegroundColor Yellow
    $ChunkSize = $aligned
}

function ConvertFrom-HexOffset {
    param([string]$Value)
    return [Convert]::ToInt64(($Value -replace '^0[xX]', ''), 16)
}

$targets = @()
if ($Mode -eq "app") {
    $appFile = Join-Path $BuildDir $spec.app.file
    $targets = @([pscustomobject]@{
        Label  = "app"
        Offset = ConvertFrom-HexOffset $spec.app.offset
        File   = $appFile
    })
}
else {
    foreach ($entry in $spec.flash_files.PSObject.Properties) {
        $targets += [pscustomobject]@{
            Label  = [IO.Path]::GetFileNameWithoutExtension($entry.Value)
            Offset = ConvertFrom-HexOffset $entry.Name
            File   = Join-Path $BuildDir $entry.Value
        }
    }
    $targets = $targets | Sort-Object Offset
}

foreach ($t in $targets) {
    if (-not (Test-Path $t.File)) { throw "Missing image $($t.File); run ./scripts/build_plus.ps1 first" }
}

# export.ps1 writes its banner to stderr; keep it from aborting under "Stop".
$savedPreference = $ErrorActionPreference
$ErrorActionPreference = "Continue"
. "$IdfPath/export.ps1" 2>&1 | Out-Null
$ErrorActionPreference = $savedPreference

function Invoke-Esptool {
    param([string[]]$Arguments)
    python -m esptool @Arguments 2>&1 | Out-Null
    return $LASTEXITCODE
}

Write-Host "== Chip identity ==" -ForegroundColor Cyan
# esptool 5 renamed chip_id to chip-id; try both so either toolchain works.
# The probe also prints harmless warnings on stderr; keep PowerShell from
# surfacing them as NativeCommandError noise.
$savedErr = $ErrorActionPreference
$ErrorActionPreference = "SilentlyContinue"
$chip = (python -m esptool --port $Port --baud $Baud chip-id 2>&1 | Out-String)
if ($LASTEXITCODE) {
    $chip = (python -m esptool --port $Port --baud $Baud chip_id 2>&1 | Out-String)
}
$ErrorActionPreference = $savedErr
if ($chip -notmatch "ESP32-S31") {
    throw "Target is not an ESP32-S31. Refusing to flash.`n$chip"
}
Write-Host ($chip.Trim()) -ForegroundColor DarkGray

if ($Mode -eq "full" -and -not $IHaveFullBackup) {
    throw "Full-table flashing replaces the bootloader and partition layout. Take and verify a full 16 MiB backup, then pass -IHaveFullBackup."
}

function Write-Chunk {
    param([long]$Address, [string]$Path, [int]$BaudRate)

    $addrHex = "0x{0:x}" -f $Address
    for ($attempt = 1; $attempt -le $Retries; $attempt++) {
        # The port needs a moment to come back after the previous session; a
        # dropped link needs longer still to re-enumerate, so retries wait more
        # and step the rate down.
        $rate = switch ($attempt) {
            1 { $BaudRate }
            2 { [Math]::Min($BaudRate, 460800) }
            default { 115200 }
        }
        $delay = if ($attempt -eq 1) { $ResetDelayMs } else { [Math]::Max($ResetDelayMs, 6000) }
        Start-Sleep -Milliseconds $delay
        $etArgs = @("--port", $Port, "--baud", "$rate", "--before", "default-reset",
                    "--after", "no-reset", "write-flash") + $writeArgs + @($addrHex, $Path)
        if (-not (Invoke-Esptool $etArgs)) { return $true }
        Write-Host "   retry $attempt/$Retries (baud $rate)" -ForegroundColor Yellow
    }
    return $false
}

function Write-SingleImage {
    param($Target, [long]$Size)

    $addrHex = "0x{0:x}" -f $Target.Offset
    Write-Host "== $($Target.Label): $Size bytes at $addrHex (single write) ==" -ForegroundColor Cyan
    if (-not (Write-Chunk $Target.Offset $Target.File $Baud)) { return $false }
    $script:written += $Size
    return $true
}

function Write-ChunkedImage {
    param($Target, [long]$Size)

    $baseHex = "0x{0:x}" -f $Target.Offset
    $count = [int][Math]::Ceiling($Size / $ChunkSize)
    Write-Host "== $($Target.Label): $Size bytes at $baseHex in $count chunks of $ChunkSize ==" -ForegroundColor Cyan

    $bytes = [IO.File]::ReadAllBytes((Resolve-Path $Target.File))
    for ($i = 0; $i -lt $count; $i++) {
        $start = $i * $ChunkSize
        $len = [Math]::Min($ChunkSize, $bytes.Length - $start)
        $chunkFile = Join-Path $tempRoot ("chunk-{0:D3}.bin" -f $i)
        $segment = New-Object byte[] $len
        [Array]::Copy($bytes, $start, $segment, 0, $len)
        [IO.File]::WriteAllBytes($chunkFile, $segment)

        $chunkAddr = $Target.Offset + $start
        $addrHex = "0x{0:x}" -f $chunkAddr
        Write-Host ("   [{0,2}/{1}] {2}  {3:N1} KB" -f ($i + 1), $count, $addrHex, ($len / 1KB)) -NoNewline
        $ok = Write-Chunk $chunkAddr $chunkFile $Baud
        Remove-Item -Force $chunkFile -ErrorAction SilentlyContinue
        if (-not $ok) { return $false }
        Write-Host "  ok" -ForegroundColor Green
        $script:written += $len
    }
    return $true
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("rickyos-flash-" + [Guid]::NewGuid().ToString("N"))
$sw = [Diagnostics.Stopwatch]::StartNew()
$written = 0
try {
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null

    foreach ($t in $targets) {
        $size = (Get-Item $t.File).Length
        $ok = switch ($Strategy) {
            "single" { Write-SingleImage $t $size }
            "chunked" {
                if ($size -le $ChunkSize) { Write-SingleImage $t $size } else { Write-ChunkedImage $t $size }
            }
            default {
                if ($size -le $ChunkSize) { Write-SingleImage $t $size }
                elseif (Write-SingleImage $t $size) { $true }
                else {
                    # A hub in the path drops the link part way through a large
                    # image; let the port re-enumerate, then retry in chunks.
                    Write-Host "   single write dropped; falling back to $ChunkSize-byte chunks" -ForegroundColor Yellow
                    Start-Sleep -Milliseconds ([Math]::Max($ResetDelayMs, 6000))
                    Write-ChunkedImage $t $size
                }
            }
        }
        if (-not $ok) { throw ("Failed to write {0} at 0x{1:x}" -f $t.Label, $t.Offset) }
    }
}
finally {
    Remove-Item -Recurse -Force $tempRoot -ErrorAction SilentlyContinue
}

$sw.Stop()
Write-Host ("`nWritten {0:N2} MB in {1:N1} s." -f ($written / 1MB), $sw.Elapsed.TotalSeconds) -ForegroundColor Green

# Leave the chip in the bootloader after each chunk; a plain reset here starts
# the new image without touching the port again mid-transfer.
Write-Host "== Reset to run the new image ==" -ForegroundColor Cyan
Invoke-Esptool @("--port", $Port, "--before", "default-reset", "--after", "hard-reset", "read-mac") | Out-Null

Write-Host "Flashed. Expect the home screen after the boot splash." -ForegroundColor Green
if ($Monitor) {
    Push-Location $project
    try { idf.py -B $BuildDir -p $Port monitor }
    finally { Pop-Location }
}
