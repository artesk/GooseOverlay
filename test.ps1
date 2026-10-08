$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'MSVC x86/x64 tools are required.' }
$devcmd = Join-Path $vsRoot 'Common7\Tools\VsDevCmd.bat'
Push-Location $PSScriptRoot
try {
    foreach ($arch in @('x64','x86')) {
        $compile = 'call "{0}" -arch={1} -host_arch=x64 && cl /nologo /std:c++17 /EHsc /utf-8 /W4 /MT /DUNICODE /D_UNICODE /Ithird_party\ts3client-pluginsdk\include tests\whisper_tests.cpp /Fodist\obj\{1}\whisper_tests.obj /Fedist\whisper_tests_{1}.exe /link user32.lib gdi32.lib gdiplus.lib ole32.lib advapi32.lib' -f $devcmd, $arch
        & cmd.exe /d /s /c $compile
        if ($LASTEXITCODE -ne 0) { throw "Test build failed: $arch" }
        & ".\dist\whisper_tests_$arch.exe"
        if ($LASTEXITCODE -ne 0) { throw "Tests failed: $arch" }
    }
} finally {
    Pop-Location
}
