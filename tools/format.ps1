. (Join-Path $PSScriptRoot 'environment.ps1')
$format=Join-Path $runtime 'format\clang-format.exe'
$files=@(Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src') -File | Where-Object Extension -in @('.cpp','.h') | ForEach-Object FullName)
$files+=Join-Path $PSScriptRoot 'native_tests.cpp'
& $format -i @files
if($LASTEXITCODE -ne 0){throw 'Source formatting failed'}
