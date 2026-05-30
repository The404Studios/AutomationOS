<#
.SYNOPSIS
    AutomationOS QEMU Launcher (Windows / PowerShell)
.DESCRIPTION
    Launches AutomationOS ISO in QEMU with graphical display, UEFI boot,
    PS/2 input and optional debug mode.
.PARAMETER Debug
    Start QEMU with GDB server on port 1234.
.PARAMETER Memory
    Set RAM size (default: 4G).
.PARAMETER Cpus
    Set CPU count (default: 4).
.PARAMETER Vnc
    Use VNC display instead of GTK graphical window.
.PARAMETER Headless
    No display (serial only).
.PARAMETER Help
    Show help.
.EXAMPLE
    .\scripts\run-qemu.ps1
    Normal boot with graphical display.
.EXAMPLE
    .\scripts\run-qemu.ps1 -Debug
    Debug mode — wait for GDB on port 1234.
#>

param(
    [switch]$Debug,
    [switch]$Help,
    [string]$Memory = "4G",
    [int]$Cpus = 4,
    [switch]$Vnc,
    [switch]$Headless
)

# ---------------------------------------------------------------------------
#  Paths
# ---------------------------------------------------------------------------
$Qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$Iso      = "build\automationos.iso"
$Initrd   = "build\initrd.img"
$SerialLog = "build\serial.log"

$OVMF     = $null
$OVMF_PATHS = @(
    "C:\Program Files\qemu\share\edk2-x86_64-code.fd"
    "C:\Program Files (x86)\qemu\share\edk2-x86_64-code.fd"
)
foreach ($path in $OVMF_PATHS) {
    if (Test-Path -LiteralPath $path) {
        $OVMF = $path
        break
    }
}

# ---------------------------------------------------------------------------
#  Help
# ---------------------------------------------------------------------------
if ($Help) {
    Write-Host @"
AutomationOS QEMU Launcher (PowerShell)

Usage: .\scripts\run-qemu.ps1 [OPTIONS]

Options:
    -Debug          Start QEMU with GDB server on port 1234
    -Memory <size>  Set RAM size (default: $Memory)
    -Cpus <n>       Set CPU count (default: $Cpus)
    -Vnc            Use VNC instead of graphical display
    -Headless       No display (serial only)
    -Help           Show this help

Examples:
    .\scripts\run-qemu.ps1                  # Normal boot (graphical)
    .\scripts\run-qemu.ps1 -Debug           # Debug mode
    .\scripts\run-qemu.ps1 -Memory 8G -Cpus 8

Debug:
    gdb build\kernel.elf -ex 'target remote :1234'

Serial output:
    Saved to: $SerialLog
    To watch live: Get-Content -Wait $SerialLog
"@
    exit 0
}

# ---------------------------------------------------------------------------
#  Pre-flight checks
# ---------------------------------------------------------------------------
if (-not (Test-Path -LiteralPath $Qemu)) {
    Write-Host "ERROR: QEMU not found at $Qemu" -ForegroundColor Red
    Write-Host "Install QEMU from https://qemu.org/download"
    exit 1
}

if (-not (Test-Path -LiteralPath $Iso)) {
    Write-Host "ERROR: ISO not found: $Iso" -ForegroundColor Red
    Write-Host "Run 'make iso' first to build the ISO image"
    exit 1
}

if (-not $OVMF) {
    Write-Host "WARNING: OVMF UEFI firmware not found; boot may fail without it" -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
#  Pick display
# ---------------------------------------------------------------------------
$DisplayArgs = @()
if ($Headless) {
    $DisplayArgs = @("-display", "none")
} elseif ($Vnc) {
    $DisplayArgs = @("-display", "vnc=:0")
} else {
    $DisplayArgs = @("-display", "gtk")
}

# ---------------------------------------------------------------------------
#  Build argument list
# ---------------------------------------------------------------------------
$QemuArgs = @()

# UEFI firmware
if ($OVMF) {
    $QemuArgs += @("-bios", $OVMF)
}

# Boot media
$QemuArgs += @("-cdrom", $Iso)

# Hardware
$QemuArgs += @("-m", $Memory)
$QemuArgs += @("-smp", $Cpus)

# PS/2 keyboard & mouse
$QemuArgs += @("-device", "isa-ps2")

# Serial output — log to file
$QemuArgs += @("-serial", "file:$SerialLog")

# Display
$QemuArgs += $DisplayArgs

# Behaviour
$QemuArgs += "-no-reboot"
$QemuArgs += "-no-shutdown"

# Optional initrd
if (Test-Path -LiteralPath $Initrd) {
    $QemuArgs += @("-initrd", $Initrd)
}

# Debug mode
if ($Debug) {
    $QemuArgs += @("-s", "-S")
}

# ---------------------------------------------------------------------------
#  Launch info
# ---------------------------------------------------------------------------
Write-Host "========================================" -ForegroundColor Green
Write-Host "  AutomationOS QEMU"                     -ForegroundColor Green
Write-Host "========================================" -ForegroundColor Green
Write-Host ""
Write-Host "QEMU:    $Qemu"
Write-Host "ISO:     $Iso"
Write-Host "OVMF:    $(if ($OVMF) { $OVMF } else { '<none>' })"
Write-Host "Memory:  $Memory"
Write-Host "CPUs:    $Cpus"
Write-Host "Display: $($DisplayArgs -join ' ')"
Write-Host "Debug:   $($Debug.IsPresent)"
Write-Host "Serial:  $SerialLog"
Write-Host ""

if ($Debug) {
    Write-Host "GDB server listening on port 1234" -ForegroundColor Green
    Write-Host "Attach: gdb build\kernel.elf -ex 'target remote :1234'" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "Press any key to start QEMU..." -ForegroundColor Yellow
    $null = $Host.UI.RawUI.ReadKey("IncludeKeyDown,NoEcho")
    Write-Host ""
}

Write-Host "Starting QEMU..." -ForegroundColor Green
Write-Host ""

# ---------------------------------------------------------------------------
#  Launch
# ---------------------------------------------------------------------------
& $Qemu @QemuArgs

Write-Host ""
Write-Host "QEMU exited" -ForegroundColor Green
if (Test-Path -LiteralPath $SerialLog) {
    Write-Host "Serial output saved to: $SerialLog"
}
