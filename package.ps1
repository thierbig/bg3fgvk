# Builds the release zip: bg3fgvk-<version>.zip, laid out like the game's bin\ folder so the
# user extracts it straight into bin\.
#   .\package.ps1 -Version v0.1.0            (uses the existing build\Release output)
#   .\package.ps1 -Version v0.1.0 -Build     (rebuilds first)
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [switch]$Build
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

if ($Build) {
    & cmake -S $root -B "$root\build" -A x64
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
    & cmake --build "$root\build" --config Release
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
}

$dll   = "$root\build\Release\fgvk.dll"
$stack = "$root\build\Release\fgvk-stack.exe"
if (-not (Test-Path $dll))   { throw "missing $dll - build first (-Build)" }
if (-not (Test-Path $stack)) { throw "missing $stack - build first (-Build)" }

$runtime = @('sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.pcl.dll','sl.reflex.dll','nvngx_dlssg.dll','NvLowLatencyVk.dll')
foreach ($f in $runtime) { if (-not (Test-Path "$root\redist\$f")) { throw "missing redist\$f" } }

$dist  = "$root\dist"
$stage = "$dist\bg3fgvk-$Version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\NativeMods" | Out-Null

# bin\NativeMods\ : the mod
Copy-Item $dll   "$stage\NativeMods\fgvk.dll"
Copy-Item $stack "$stage\NativeMods\fgvk-stack.exe"
# bin\ : NVIDIA Streamline runtime (all seven from one Streamline release)
foreach ($f in $runtime) { Copy-Item "$root\redist\$f" "$stage\$f" }
# docs
Copy-Item "$root\README.md"  "$stage\README.md"
Copy-Item "$root\LICENSE"    "$stage\LICENSE.txt"
Copy-Item "$root\redist\STREAMLINE-LICENSE.txt" "$stage\STREAMLINE-LICENSE.txt"
@"
bg3fgvk $Version - DLSS Frame Generation for Baldur's Gate 3 (Vulkan)

Extract the CONTENTS of this folder into the game's bin\ folder (where bg3.exe is):
  bin\NativeMods\fgvk.dll, bin\NativeMods\fgvk-stack.exe   (needs Native Mod Loader)
  bin\sl.*.dll, bin\nvngx_dlssg.dll, bin\NvLowLatencyVk.dll (NVIDIA Streamline runtime)

Requirements: RTX 40 (x2) or RTX 50 (x3/x4), Vulkan mode, DLSS enabled in-game,
Hardware-accelerated GPU scheduling ON in Windows. Full guide: README.md
"@ | Set-Content -Encoding UTF8 "$stage\INSTALL.txt"

$zip = "$dist\bg3fgvk-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
Write-Host "wrote $zip"
Get-ChildItem -Recurse $stage | Where-Object { -not $_.PSIsContainer } | ForEach-Object { "{0,10}  {1}" -f $_.Length, $_.FullName.Substring($stage.Length + 1) }
