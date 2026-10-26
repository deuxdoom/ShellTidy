param([switch]$Test,[string]$OutputDirectory)
. (Join-Path $PSScriptRoot 'environment.ps1')
$configuration=Get-Content -LiteralPath (Join-Path $runtime 'toolchain.json') -Raw | ConvertFrom-Json
$msvc=$configuration.msvc
$sdk=$configuration.sdk
$compiler=Join-Path $msvc 'bin\Hostx64\x64\cl.exe'
$resourceCompiler=Join-Path $sdk 'bin\x64\rc.exe'
if(-not $OutputDirectory){$OutputDirectory=Join-Path $projectRoot 'dist'}
$intermediate=Join-Path $PSScriptRoot 'output\build'
New-Item -ItemType Directory -Force -Path $OutputDirectory,$intermediate | Out-Null
$env:INCLUDE=((@((Join-Path $msvc 'include')))+(@('ucrt','shared','um','winrt','cppwinrt') | ForEach-Object {Join-Path $sdk ('Include\'+$_)})) -join ';'
$env:LIB=@((Join-Path $msvc 'lib\x64'),(Join-Path $sdk 'Lib\ucrt\x64'),(Join-Path $sdk 'Lib\um\x64')) -join ';'
$env:PATH=(Join-Path $msvc 'bin\Hostx64\x64')+';'+(Join-Path $sdk 'bin\x64')+';'+$env:PATH
$resource=Join-Path $intermediate 'app.res'
Push-Location (Join-Path $projectRoot 'resources')
try{& $resourceCompiler /nologo /fo $resource app.rc;if($LASTEXITCODE -ne 0){throw 'Resource compilation failed'}}finally{Pop-Location}
$sources=@(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src') -Filter '*.cpp' | Sort-Object Name | ForEach-Object FullName)
$sources+=Join-Path $PSScriptRoot 'native_tests.cpp'
$exe=Join-Path $OutputDirectory 'ShellTidy.exe'
$options=@('/nologo','/std:c++20','/utf-8','/EHsc','/O2','/MT','/W4','/permissive-','/DUNICODE','/D_UNICODE','/D_WIN32_WINNT=0x0A00','/DWINVER=0x0A00','/DNTDDI_VERSION=0x0A000000')
$libraries=@('user32.lib','gdi32.lib','advapi32.lib','shell32.lib','shlwapi.lib','comctl32.lib','ole32.lib','uxtheme.lib','dwmapi.lib','rstrtmgr.lib')
Push-Location $intermediate
try{& $compiler @options @sources $resource "/Fe:$exe" /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /MANIFEST:NO /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA @libraries;if($LASTEXITCODE -ne 0){throw 'Native compilation failed'}}finally{Pop-Location}
$result=Get-Item -LiteralPath $exe
if($result.Length -gt 10MB){throw 'EXE exceeds 10 MB'}
Write-Output "Built with project-local MSVC: $($result.FullName) ($($result.Length) bytes)"
if($Test){& (Join-Path $PSScriptRoot 'test.ps1') -ExePath $exe}
