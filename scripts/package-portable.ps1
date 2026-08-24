# Package the portable Windows ZIP (action plan §5 Phase 5).
# Usage: powershell -File scripts\package-portable.ps1 [-Version 2.1.0]
#        [-DnglabBin <path>] [-ExiftoolBin <path>]
# Builds nothing; run scripts\build-windows.bat first. Stages:
#   rawimport-portable-v<Version>-win-x64.zip
#     rawimport.exe, .env.example, README.txt, tools\dnglab.exe, tools\exiftool.exe
param(
    [string]$Version = "2.1.0",
    [string]$DnglabBin = "",
    [string]$ExiftoolBin = ""
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot

$exe = Join-Path $repo "build-win\Release\rawimport-pipeline.exe"
if (!(Test-Path $exe)) {
    throw "rawimport-pipeline.exe not found at $exe — run scripts\build-windows.bat first."
}

$stageName = "rawimport-portable-v$Version-win-x64"
$distDir = Join-Path $repo "dist"
$stage = Join-Path $distDir $stageName
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $stage "tools") -Force | Out-Null

Copy-Item $exe (Join-Path $stage "rawimport.exe")
Copy-Item (Join-Path $repo ".env.example") $stage
Copy-Item (Join-Path $repo "README.txt") $stage

# Bundle dnglab/exiftool if their locations are known; otherwise leave the
# tools/ folder for a manual one-time drop (the binary never downloads).
$toolSpecs = @(
    @{ Name = "dnglab";    Given = $DnglabBin },
    @{ Name = "exiftool";  Given = $ExiftoolBin }
)
foreach ($t in $toolSpecs) {
    $dest = Join-Path $stage ("tools\" + $t.Name + ".exe")
    if ($t.Given -and (Test-Path $t.Given)) {
        Copy-Item $t.Given $dest
        Write-Host "bundled $($t.Name) from $($t.Given)"
        continue
    }
    $candidates = @(
        (Join-Path $repo ("tools\" + $t.Name + ".exe")),
        (Join-Path $repo ("tools\" + $t.Name))
    )
    $found = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($found) {
        Copy-Item $found $dest
        Write-Host "bundled $($t.Name) from $found"
    } else {
        Write-Warning "$($t.Name).exe not found — place it manually in:"
        Write-Warning "  $dest"
    }
}

$zip = Join-Path $distDir "$stageName.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -Force
Write-Host "Created $zip"
