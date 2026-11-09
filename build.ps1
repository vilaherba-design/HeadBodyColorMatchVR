# Builds HeadBodyColorMatch (FO4VR port) in %TEMP%, zips it to C:\Users\cereBRO\Modding and deletes the build tree.
# Usage (PowerShell): .\build.ps1 -Src <folder with this project> [-CLib <existing CommonLibF4VR checkout at 9057052>]
# Without -CLib it uses lib\CommonLibF4VR (git submodule) or clones it.
param([Parameter(Mandatory)][string]$Src, [string]$Out = "C:\Users\cereBRO\Modding", [string]$CLib = "")
$ErrorActionPreference = "Stop"
$W = Join-Path $env:TEMP "hbcmvr"
if (Test-Path $W) { Remove-Item $W -Recurse -Force }
Copy-Item $Src $W -Recurse
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
$zip = Join-Path $Out "HeadBodyColorMatchVR-1.1.0-vr1.zip"
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
# GPL: the port's own source goes inside the mod as a nested zip
$srcZip = Join-Path $W "HeadBodyColorMatch_source.zip"
$sz = [System.IO.Compression.ZipFile]::Open($srcZip, "Create")
foreach ($f in @("CMakeLists.txt", "vcpkg.json", "build.ps1", "README.md", "LICENSE", "EXCEPTIONS", "src/main.cpp", "src/pch.h", "data/HeadBodyColorMatch.ini")) {
  [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($sz, (Join-Path $W $f), "HeadBodyColorMatchVR/$f")
}
$sz.Dispose()
$files["F4SE/Plugins/HeadBodyColorMatch_source.zip"] = $srcZip
foreach ($k in $files.Keys) { [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z, $files[$k], $k) }
$z.Dispose()
Get-Item $dll, $zip | Format-Table Name, Length
Get-FileHash $zip -Algorithm SHA256 | Format-List
