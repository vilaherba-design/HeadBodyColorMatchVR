# Builds HeadBodyColorMatch (FO4VR port) in %TEMP%, zips it to C:\Users\cereBRO\Modding and deletes the build tree.
# Usage (PowerShell): .\build.ps1 -Src <folder with this project> [-CLib <existing CommonLibF4VR checkout at 9057052>]
# Without -CLib it uses lib\CommonLibF4VR (git submodule) or clones it.
param([Parameter(Mandatory)][string]$Src, [string]$Out = "C:\Users\cereBRO\Modding", [string]$CLib = "", [switch]$CleanSrc)
$ErrorActionPreference = "Stop"
$W = Join-Path $env:TEMP "hbcmvr"
if (Test-Path $W) { Remove-Item $W -Recurse -Force }
Copy-Item $Src $W -Recurse
if ($CleanSrc) { Remove-Item $Src -Recurse -Force }  # the source now lives only in %TEMP%\hbcmvr
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = "E:\General\Lifestyle\Modding\Fallout 4\VR Version\Herramientas\vcpkg" }
if (-not $CLib -and (Test-Path "$W\lib\CommonLibF4VR\CommonLibF4\include\REL\Relocation.h")) {
  # the submodule was checked out with the source (git clone --recursive): already copied with it
} elseif ($CLib) {
  # an existing CommonLibF4VR checkout (copied to a short path: cl.exe cannot open 260+ character header paths)
  New-Item -ItemType Directory "$W\lib" -Force | Out-Null
  robocopy $CLib "$W\lib\CommonLibF4VR" /E /XD .git /NFL /NDL /NJH /NJS | Out-Null
} else {
  git clone -q https://github.com/ArthurHub/CommonLibF4VR.git "$W\lib\CommonLibF4VR"
  git -C "$W\lib\CommonLibF4VR" checkout -q 9057052
}
cmake -S $W -B "$W\build" -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE) { throw "configure failed" }
cmake --build "$W\build" --config Release -- /m /v:m
if ($LASTEXITCODE) { throw "build failed" }
$dll = "$W\build\Release\HeadBodyColorMatch.dll"
$zip = Join-Path $Out "HeadBodyColorMatchVR-1.1.0-vr6.zip"
if (Test-Path $zip) { Remove-Item $zip }
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$z = [System.IO.Compression.ZipFile]::Open($zip, "Create")
$files = @{
  "F4SE/Plugins/HeadBodyColorMatch.dll" = $dll
  "F4SE/Plugins/HeadBodyColorMatch.pdb" = "$W\build\Release\HeadBodyColorMatch.pdb"
  "F4SE/Plugins/HeadBodyColorMatch.ini" = "$W\data\HeadBodyColorMatch.ini"
  "F4SE/Plugins/HeadBodyColorMatch_LICENSE.txt" = "$W\LICENSE"
  "F4SE/Plugins/HeadBodyColorMatch_EXCEPTIONS.txt" = "$W\EXCEPTIONS"
  "F4SE/Plugins/HeadBodyColorMatch_README.txt" = "$W\README.md"
}
# GPL: no nested zip (Nexus quarantines archives inside archives). A text file points to the public source of this exact build.
$srcTxt = Join-Path $W "HeadBodyColorMatch_SOURCE.txt"
$commit = if ($env:BUILD_COMMIT) { $env:BUILD_COMMIT } else { "see the tag or commit matching this version" }
@("HeadBodyColorMatch is free software under the GPL-3.0 (see the LICENSE in the repository).", "Complete corresponding source of this build, including the CommonLibF4VR submodule:", "https://github.com/vilaherba-design/HeadBodyColorMatchVR", "Commit: $commit") | Set-Content -Path $srcTxt -Encoding UTF8
$files["F4SE/Plugins/HeadBodyColorMatch_SOURCE.txt"] = $srcTxt
foreach ($k in $files.Keys) { [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $files[$k], $k) }
$z.Dispose()
Get-Item $dll, $zip | Format-Table Name, Length
Get-FileHash $zip -Algorithm SHA256 | Format-List
Set-Location $env:TEMP
Remove-Item $W -Recurse -Force  # leave no build tree behind
