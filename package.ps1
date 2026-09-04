# Builds the release zip: bg3fgvk-<version>.zip, laid out like the game's bin\ folder so the
# user extracts it straight into bin\:
#   NativeMods\fgvk.dll                 the mod
#   NativeMods\Streamline\*.dll         NVIDIA Streamline runtime (seven files) + license + readme
#   README.md, INSTALL.txt, LICENSE.txt
# fgvk-stack.exe (developer diagnostics) is deliberately NOT packaged.
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

$dll = "$root\build\Release\fgvk.dll"
if (-not (Test-Path $dll)) { throw "missing $dll - build first (-Build)" }

$runtime = @('sl.interposer.dll','sl.common.dll','sl.dlss_g.dll','sl.pcl.dll','sl.reflex.dll','nvngx_dlssg.dll','NvLowLatencyVk.dll')
foreach ($f in $runtime) { if (-not (Test-Path "$root\redist\$f")) { throw "missing redist\$f" } }

$dist  = "$root\dist"
$stage = "$dist\bg3fgvk-$Version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\NativeMods\Streamline" | Out-Null

Copy-Item $dll "$stage\NativeMods\fgvk.dll"
foreach ($f in $runtime) { Copy-Item "$root\redist\$f" "$stage\NativeMods\Streamline\$f" }
Copy-Item "$root\redist\STREAMLINE-LICENSE.txt" "$stage\NativeMods\Streamline\STREAMLINE-LICENSE.txt"
@"
This folder is the NVIDIA Streamline runtime (DLSS Frame Generation) version 2.12.0,
unmodified from NVIDIA's release package:

  sl.interposer.dll, sl.common.dll, sl.dlss_g.dll, sl.pcl.dll, sl.reflex.dll,
  nvngx_dlssg.dll, NvLowLatencyVk.dll

Keep this folder exactly here, next to fgvk.dll:  bin\NativeMods\Streamline\
fgvk.dll loads Streamline from this folder. Nothing from here needs to be copied anywhere else.

To update Streamline later: download streamline-sdk-vX.Y.Z.zip from
https://github.com/NVIDIA-RTX/Streamline/releases and replace all seven files with the ones
from its bin\x64\ folder (not bin\x64\development\). Always replace all seven together.

License: STREAMLINE-LICENSE.txt (NVIDIA Corporation).
"@ | Set-Content -Encoding UTF8 "$stage\NativeMods\Streamline\README-STREAMLINE.txt"

Copy-Item "$root\README.md" "$stage\README.md"
Copy-Item "$root\LICENSE"   "$stage\LICENSE.txt"
@"
bg3fgvk $Version - DLSS Frame Generation for Baldur's Gate 3 (Vulkan)

INSTALL
  1. Install "Native Mod Loader": https://www.nexusmods.com/baldursgate3/mods/944
     (manual install: extract its bin folder over the game's bin folder, replacing bink2w64.dll).
     Create bin\NativeMods\ if it does not exist.
  2. Extract the CONTENTS of this zip into the game's bin\ folder (where bg3.exe is).
     If asked, merge the NativeMods folder. You end up with:
        bin\NativeMods\fgvk.dll
        bin\NativeMods\Streamline\   (NVIDIA Streamline runtime - keep it there, see its README)
  3. In the game: Vulkan renderer, DLSS on (any quality or DLAA).
     In Windows: Hardware-accelerated GPU scheduling ON.

Load a save. Frame generation starts by itself a couple of seconds into the world.
Numpad * turns it off/on. Settings: bin\NativeMods\fgvk.ini (created on first launch).

Requirements: RTX 40 (x2) or RTX 50 (x3/x4), Windows 10 20H1+, Hardware-accelerated GPU scheduling ON.
Full guide, configuration and troubleshooting: README.md
"@ | Set-Content -Encoding UTF8 "$stage\INSTALL.txt"

$zip = "$dist\bg3fgvk-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
Write-Host "wrote $zip"
Get-ChildItem -Recurse $stage | Where-Object { -not $_.PSIsContainer } | ForEach-Object { "{0,10}  {1}" -f $_.Length, $_.FullName.Substring($stage.Length + 1) }
