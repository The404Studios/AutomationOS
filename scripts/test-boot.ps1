#!/usr/bin/env pwsh
<#
.SYNOPSIS
    AutomationOS Boot Test Runner (PowerShell)
.DESCRIPTION
    Runs AutomationOS ISO in QEMU with display, captures serial output,
    and checks for key boot success indicators.
.EXIT CODE
    0 - All checks passed
    1 - One or more checks failed
    2 - Setup error (QEMU not found, ISO missing)
#>

param(
    [int]$Timeout = 30,
    [switch]$Verbose
)

$Root = Split-Path -Parent $PSScriptRoot
$IsoPath = Join-Path $Root "build" "automationos.iso"
$SerialLog = Join-Path $Root "build" "serial.log"

function Write-Step($Msg) { Write-Host "`n==> $Msg" -ForegroundColor Yellow }
function Write-Pass($Msg) { Write-Host "  [PASS] $Msg" -ForegroundColor Green }
function Write-Fail($Msg) { Write-Host "  [FAIL] $Msg" -ForegroundColor Red }
function Write-Info($Msg)  { if ($Verbose) { Write-Host "  [INFO] $Msg" -ForegroundColor Cyan } }

# ------------------------------------------------------------
# Find QEMU
# ------------------------------------------------------------
function Find-QEMU {
    $candidates = @(
        "qemu-system-x86_64.exe"
        "qemu-system-x86_64"
    )

    # Check PATH first
    foreach ($bin in $candidates) {
        $path = Get-Command $bin -ErrorAction SilentlyContinue
        if ($path) { return $path.Source }
    }

    # Check common install locations
    $paths = @(
        "${env:ProgramFiles}\qemu\qemu-system-x86_64.exe"
        "${env:ProgramFiles(x86)}\qemu\qemu-system-x86_64.exe"
        "${env:LOCALAPPDATA}\Programs\qemu\qemu-system-x86_64.exe"
    )
    foreach ($p in $paths) {
        if (Test-Path $p) { return $p }
    }
    return $null
}

# ------------------------------------------------------------
# Check prerequisites
# ------------------------------------------------------------
Write-Step "Checking prerequisites"

$QemuPath = Find-QEMU
if (-not $QemuPath) {
    Write-Fail "qemu-system-x86_64 not found"
    Write-Host "  Install QEMU from https://qemu.org/download" -ForegroundColor Yellow
    exit 2
}
Write-Info "QEMU: $QemuPath"

if (-not (Test-Path $IsoPath)) {
    Write-Fail "ISO not found: $IsoPath"
    Write-Host "  Run 'make iso' first" -ForegroundColor Yellow
    exit 2
}
Write-Info "ISO: $IsoPath"

Write-Pass "All prerequisites met"
Write-Host ""

# ------------------------------------------------------------
# Pick display backend
# ------------------------------------------------------------
function Get-DisplayFlag {
    # Try GTK first, then SDL, fall back to VGA
    $gtkTest = & $QemuPath -display gtk -help 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -and -not $gtkTest.Contains("invalid")) {
        return "-display gtk"
    }
    $sdlTest = & $QemuPath -display sdl -help 2>&1 | Out-String
    if ($LASTEXITCODE -eq 0 -and -not $sdlTest.Contains("invalid")) {
        return "-display sdl"
    }
    return "-vga std"
}

$DisplayFlag = Get-DisplayFlag
Write-Info "Display: $DisplayFlag"

# ------------------------------------------------------------
# Run QEMU
# ------------------------------------------------------------
Write-Step "Starting QEMU (timeout: ${Timeout}s)"

# Remove old serial log
if (Test-Path $SerialLog) { Remove-Item $SerialLog -Force }

$QemuArgs = @(
    "-cdrom", "`"$IsoPath`""
    "-m", "4G"
    "-smp", "4"
    "-serial", "file:$SerialLog"
    $DisplayFlag
    "-no-reboot"
    "-no-shutdown"
)

Write-Info "Command: $QemuPath $($QemuArgs -join ' ')"

try {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $QemuPath
    $psi.Arguments = $QemuArgs -join ' '
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true

    $proc = [System.Diagnostics.Process]::Start($psi)
    Write-Info "QEMU PID: $($proc.Id)"

    Start-Sleep -Seconds $Timeout

    if (-not $proc.HasExited) {
        Write-Info "Terminating QEMU..."
        $proc.Kill()
        $proc.WaitForExit(5000) | Out-Null
    }

    Write-Pass "QEMU run complete"
}
catch {
    Write-Fail "Failed to run QEMU: $_"
    exit 2
}

# ------------------------------------------------------------
# Check serial log
# ------------------------------------------------------------
Write-Step "Reading serial output"

if (-not (Test-Path $SerialLog)) {
    Write-Fail "Serial log not found: $SerialLog"
    exit 2
}

$output = Get-Content $SerialLog -Raw -Encoding UTF8
Write-Info "Serial output: $($output.Length) bytes"

if ($Verbose) {
    Write-Host "`n$('=' * 60)" -ForegroundColor Cyan
    Write-Host "Serial Console Output" -ForegroundColor Cyan
    Write-Host "$('=' * 60)" -ForegroundColor Cyan
    Write-Host $output
    Write-Host "$('=' * 60)" -ForegroundColor Cyan
}

# ------------------------------------------------------------
# Run checks
# ------------------------------------------------------------
Write-Step "Running boot tests"

$tests = @(
    @{ Label = "Kernel banner printed";           Pattern = "AutomationOS v0.1.0" }
    @{ Label = "Physical Memory Manager init";    Pattern = "[PMM]" }
    @{ Label = "Virtual Memory Manager init";     Pattern = "[VMM]" }
    @{ Label = "Kernel heap initialized";         Pattern = "[HEAP]" }
    @{ Label = "Init process started";            Pattern = "Init process started" }
    @{ Label = "Userspace running";               Pattern = "Hello from userspace" }
    @{ Label = "Terminal spawned";                Pattern = "(?i)terminal" }
)

$passed = 0
$failed = 0

foreach ($t in $tests) {
    if ($output -match $t.Pattern) {
        Write-Pass $t.Label
        $passed++
    }
    else {
        Write-Fail $t.Label
        $failed++
    }
}

# ------------------------------------------------------------
# Summary
# ------------------------------------------------------------
Write-Host "`n$('=' * 50)" -ForegroundColor Cyan
Write-Host "Test Summary" -ForegroundColor Cyan
Write-Host "$('=' * 50)" -ForegroundColor Cyan
Write-Host "Passed: $passed"
Write-Host "Failed: $failed"
Write-Host "Total:  $($passed + $failed)"
Write-Host "$('=' * 50)" -ForegroundColor Cyan

if ($Verbose -and $failed -gt 0) {
    Write-Host "`n$('=' * 60)" -ForegroundColor Cyan
    Write-Host "Serial Console Output" -ForegroundColor Cyan
    Write-Host "$('=' * 60)" -ForegroundColor Cyan
    Write-Host $output
    Write-Host "$('=' * 60)" -ForegroundColor Cyan
}

if ($failed -eq 0) {
    Write-Host "`nAll tests passed!" -ForegroundColor Green
    exit 0
}
else {
    Write-Host "`n$failed test(s) failed" -ForegroundColor Red
    exit 1
}
