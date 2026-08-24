param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Build Tools 2022 with Desktop development with C++ is required.'
}

$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) {
    throw 'MSVC x64 build tools were not found.'
}

$outputDir = Join-Path $projectRoot 'dist'
$packageDir = Join-Path $outputDir 'package'
$pluginDir = Join-Path $packageDir 'plugins'
if (Test-Path -LiteralPath $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null

$optimization = if ($Configuration -eq 'Release') { '/O2 /DNDEBUG' } else { '/Od /Zi' }
$source = Join-Path $projectRoot 'src\plugin.cpp'
$include = Join-Path $projectRoot 'third_party\ts3client-pluginsdk\include'
$dll = Join-Path $pluginDir 'bfg_overlay_win64.dll'
$pdb = Join-Path $outputDir 'bfg_overlay_win64.pdb'
$vsDevCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'

$compile = 'call "{0}" -arch=x64 -host_arch=x64 && cl.exe /nologo /std:c++17 /EHsc /utf-8 /W4 {1} /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"{2}" /LD "{3}" user32.lib gdi32.lib /link /OUT:"{4}" /PDB:"{5}"' -f $vsDevCmd, $optimization, $include, $source, $dll, $pdb
& cmd.exe /d /s /c $compile
if ($LASTEXITCODE -ne 0) {
    throw "Compilation failed with exit code $LASTEXITCODE."
}

Copy-Item -LiteralPath (Join-Path $projectRoot 'package.ini') -Destination $packageDir -Force

$zipPath = Join-Path $outputDir 'GooseOverlay.zip'
$packagePath = Join-Path $outputDir 'GooseOverlay.ts3_plugin'
$legacyPackagePath = Join-Path $outputDir 'bfg-overlay.ts3_plugin'
Remove-Item -LiteralPath $zipPath, $packagePath, $legacyPackagePath -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $packageDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
Move-Item -LiteralPath $zipPath -Destination $packagePath

Write-Host "Built: $dll"
Write-Host "Installer: $packagePath"
