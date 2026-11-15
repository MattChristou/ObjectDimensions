# Builds the Object Dimensions beta-tester package:  dist\ObjectDimensions_<version>.zip
#
#   1. Windows Release build into build\win\ (not bin\Release, so it works while
#      LightWave has the installed copy loaded)
#   2. Checks: the version in the plugin-info record (src\od_module.c) is in
#      README.txt and RELEASE_NOTES.txt, and both the Windows .p and the macOS
#      binary carry it
#   3. macOS build: dist\mac\ObjectDimensions-mac.zip, the CI artifact of the
#      same commit (GitHub Actions "ObjectDimensions-mac")
#   4. Stages Plugins\Windows\ObjectDimensions.p, Plugins\macOS\ObjectDimensions-mac.zip,
#      README.txt, RELEASE_NOTES.txt, and zips them
#   5. Keeps the .pdb in dist\symbols\<version>\ (NOT shipped) for crash dumps

$ErrorActionPreference = 'Stop'
$root    = $PSScriptRoot
$msbuild = 'E:\Microsoft\Visual Studio IDE\MSBuild\Current\Bin\MSBuild.exe'

function Fail([string]$msg) { Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

# A plugin binary carries its record as plain text; check the version is in it.
function Test-PluginVersion([string]$file, [string]$ver) {
    $bytes = [System.IO.File]::ReadAllBytes($file)
    $text  = [System.Text.Encoding]::ASCII.GetString($bytes)
    return $text.Contains("ObjectDimensions") -and $text.Contains($ver)
}

# --- version, from the plugin-info record so the package and the plugin agree ---
$src = Get-Content -Raw (Join-Path $root 'src\od_module.c')
if ($src -notmatch 'LWF_PLUGIN_INFO\("ObjectDimensions",\s*"([^"]+)"\)') { Fail 'LWF_PLUGIN_INFO not found in src\od_module.c' }
$version = $Matches[1]
Write-Host "Object Dimensions $version"

# --- Windows build ---
$winOut = Join-Path $root 'build\win\'
& $msbuild (Join-Path $root 'ObjectDimensions.vcxproj') -p:Configuration=Release -p:Platform=x64 `
    "-p:OutDir=$winOut" ("-p:IntDir=" + (Join-Path $root 'build\win\obj\')) -v:minimal -nologo
if ($LASTEXITCODE -ne 0) { Fail 'Windows build failed.' }
$p = Join-Path $winOut 'ObjectDimensions.p'
if (-not (Test-PluginVersion $p $version)) { Fail "ObjectDimensions.p does not carry version $version." }

# --- docs ---
$readme = Join-Path $root 'dist_src\README.txt'
$notes  = Join-Path $root 'dist_src\RELEASE_NOTES.txt'
foreach ($f in $readme, $notes) {
    if ((Get-Content -Raw $f) -notmatch [regex]::Escape($version)) { Fail "$(Split-Path -Leaf $f) does not mention version $version." }
}

# --- macOS build (CI artifact) ---
$macZip = Join-Path $root 'dist\mac\ObjectDimensions-mac.zip'
if (-not (Test-Path $macZip)) { Fail "Missing $macZip - download the ObjectDimensions-mac artifact of this commit from GitHub Actions." }
$macTmp = Join-Path $root 'build\macpkg'
if (Test-Path $macTmp) { Remove-Item -Recurse -Force $macTmp }
Expand-Archive $macZip -DestinationPath $macTmp
$macBin = Join-Path $macTmp 'ObjectDimensions.plugin\Contents\MacOS\ObjectDimensions'
if (-not (Test-Path $macBin)) { Fail 'The macOS zip has no ObjectDimensions.plugin.' }
if (-not (Test-PluginVersion $macBin $version)) { Fail "The macOS build is not version $version - download the artifact of this commit." }

# --- stage ---
$name  = "ObjectDimensions_$version"
$dist  = Join-Path $root 'dist'
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force (Join-Path $stage 'Plugins\Windows') | Out-Null
New-Item -ItemType Directory -Force (Join-Path $stage 'Plugins\macOS')   | Out-Null
Copy-Item $p (Join-Path $stage 'Plugins\Windows')
# The Mac build goes in as the zip made on the Mac (ditto): Windows zipping
# would drop the executable bit and symlinks, so the bundle stays inside it.
Copy-Item $macZip (Join-Path $stage 'Plugins\macOS\ObjectDimensions-mac.zip')
Copy-Item $readme $stage
Copy-Item $notes  $stage

# --- symbols (kept, not shipped) ---
$sym = Join-Path $dist "symbols\$version"
New-Item -ItemType Directory -Force $sym | Out-Null
Copy-Item (Join-Path $winOut 'ObjectDimensions.pdb') $sym -Force
Copy-Item $p $sym -Force

# --- zip ---
$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
$hash = (Get-FileHash -Algorithm SHA256 $zip).Hash

Write-Host ''
Write-Host "Package : $zip" -ForegroundColor Green
Write-Host "SHA-256 : $hash"
Get-ChildItem -Recurse $stage -File | ForEach-Object { '  ' + $_.FullName.Substring($stage.Length + 1) + "  ($($_.Length) bytes)" }
