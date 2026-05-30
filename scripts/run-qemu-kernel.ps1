param(
    [switch]$Debug,
    [string]$Memory = "4G",
    [int]$Cpus = 4
)

$Qemu     = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$Kernel   = "build\kernel.elf"
$Initrd   = "build\initrd.img"
$SerialLog = "build\serial.log"

if (-not (Test-Path -LiteralPath $Qemu)) {
    Write-Host "ERROR: QEMU not found at $Qemu" -ForegroundColor Red
    exit 1
}

$QemuArgs = @("-display", "gtk", "-m", $Memory, "-smp", $Cpus)
$QemuArgs += "-no-reboot", "-no-shutdown"
$QemuArgs += "-serial", "file:$SerialLog"
$QemuArgs += "-kernel", $Kernel
if (Test-Path -LiteralPath $Initrd) {
    $QemuArgs += "-initrd", $Initrd
}
if ($Debug) {
    $QemuArgs += @("-s", "-S")
}

Write-Host "Starting QEMU with -kernel..."
Write-Host "Kernel: $Kernel"
Write-Host "Initrd: $Initrd"
Write-Host "Serial: $SerialLog"

& $Qemu @QemuArgs
