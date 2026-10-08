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
    throw 'MSVC x86/x64 build tools were not found.'
}

$outputDir = Join-Path $projectRoot 'dist'
$packageDir = Join-Path $outputDir 'package'
$pluginDir = Join-Path $packageDir 'plugins'
if (Test-Path -LiteralPath $packageDir) {
    Remove-Item -LiteralPath $packageDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null

$optimization = if ($Configuration -eq 'Release') { '/O2 /DNDEBUG /MT' } else { '/Od /Zi /MTd' }
$vsDevCmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
$source = Join-Path $projectRoot 'src\plugin.cpp'
$moduleDefinition = Join-Path $projectRoot 'src\plugin.def'
$include = Join-Path $projectRoot 'third_party\ts3client-pluginsdk\include'

function Invoke-MsvcBuild {
    param(
        [Parameter(Mandatory)] [ValidateSet('x86', 'x64')] [string]$Architecture,
        [Parameter(Mandatory)] [string]$DllName
    )

    $baseName = [IO.Path]::GetFileNameWithoutExtension($DllName)
    $objectDir = Join-Path $outputDir "obj\$Architecture"
    if (Test-Path -LiteralPath $objectDir) {
        Remove-Item -LiteralPath $objectDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $objectDir | Out-Null

    $dll = Join-Path $pluginDir $DllName
    $object = Join-Path $objectDir 'plugin.obj'
    $compilerPdb = Join-Path $objectDir 'compiler.pdb'
    $linkerPdb = Join-Path $outputDir "$baseName.pdb"
    $importLibrary = Join-Path $objectDir "$baseName.lib"
    $resource = Join-Path $objectDir 'portraits.res'
    $resourceSource = Join-Path $projectRoot 'src\portraits.rc'
    $resourceInclude = Join-Path $projectRoot 'src'
    $compile = 'call "{0}" -arch={1} -host_arch=x64 && rc.exe /nologo /i "{13}" /fo "{11}" "{12}" && cl.exe /nologo /std:c++17 /EHsc /utf-8 /W4 {2} /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"{3}" /Fo"{4}" /Fd"{5}" /LD "{6}" "{11}" user32.lib gdi32.lib gdiplus.lib ole32.lib advapi32.lib /link /DEF:"{7}" /OUT:"{8}" /PDB:"{9}" /IMPLIB:"{10}"' -f $vsDevCmd, $Architecture, $optimization, $include, $object, $compilerPdb, $source, $moduleDefinition, $dll, $linkerPdb, $importLibrary, $resource, $resourceSource, $resourceInclude
    & cmd.exe /d /s /c $compile
    if ($LASTEXITCODE -ne 0) {
        throw "$Architecture compilation failed with exit code $LASTEXITCODE."
    }
    Write-Host "Built: $dll"
}

Invoke-MsvcBuild -Architecture x64 -DllName 'bfg_overlay_win64.dll'
Invoke-MsvcBuild -Architecture x86 -DllName 'bfg_overlay_win32.dll'

Copy-Item -LiteralPath (Join-Path $projectRoot 'package.ini') -Destination $packageDir -Force

$zipPath = Join-Path $outputDir 'GooseOverlay.zip'
$packagePath = Join-Path $outputDir 'GooseOverlay.ts3_plugin'
$legacyPackagePath = Join-Path $outputDir 'bfg-overlay.ts3_plugin'
Remove-Item -LiteralPath $zipPath, $packagePath, $legacyPackagePath -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $packageDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
Move-Item -LiteralPath $zipPath -Destination $packagePath

Write-Host "Installer: $packagePath"
