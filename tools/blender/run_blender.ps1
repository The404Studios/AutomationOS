# run_blender.ps1 -- run a repo Blender script HEADLESS (factory settings: never touches your open session,
# your preferences, add-ons, or your .blend files).
#   powershell -File tools\blender\run_blender.ps1 tools\blender\meridian_ribbons.py build\meridian
param([Parameter(Mandatory = $true)][string]$Script, [string[]]$ScriptArgs = @())
$blender = "C:\Program Files\Blender Foundation\Blender 5.0\blender.exe"
if (-not (Test-Path $blender)) { throw "Blender not found at $blender" }
$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Push-Location $root
try {
    & $blender -b --factory-startup --python $Script -- @ScriptArgs 2>&1
    exit $LASTEXITCODE
} finally { Pop-Location }
