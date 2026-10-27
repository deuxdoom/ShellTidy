param([switch]$Test)
. (Join-Path $PSScriptRoot 'environment.ps1')
& $python -B (Join-Path $PSScriptRoot 'check_site.py')
if($LASTEXITCODE -ne 0){throw 'Site verification failed'}
& $git -C $projectRoot diff --check
if($LASTEXITCODE -ne 0){throw 'Whitespace verification failed'}
$exe=Get-Item -LiteralPath (Join-Path $projectRoot 'dist\ShellTidy.exe')
if($exe.Length -gt 10MB){throw 'EXE exceeds 10 MB'}
$configuration=Get-Content -LiteralPath (Join-Path $runtime 'toolchain.json') -Raw | ConvertFrom-Json
$dumpbin=Join-Path $configuration.msvc 'bin\Hostx64\x64\dumpbin.exe'
$dependencies=& $dumpbin /DEPENDENTS $exe.FullName
if($LASTEXITCODE -ne 0){throw 'Dependency inspection failed'}
if($dependencies -match 'VCRUNTIME|MSVCP[0-9]|MSVCR[0-9]|UCRTBASE'){throw 'Unexpected external CRT dependency'}
$dependencies | Select-String -Pattern '\.dll' -CaseSensitive:$false
$hash=(Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
$sums=Join-Path $projectRoot 'dist\SHA256SUMS.txt'
if(-not (Test-Path -LiteralPath $sums)){throw 'dist\SHA256SUMS.txt is missing'}
$recorded=((Get-Content -LiteralPath $sums | Where-Object {$_ -match '\sShellTidy\.exe$'} | Select-Object -First 1) -split '\s+')[0]
if($recorded -ne $hash){throw "SHA256SUMS.txt ($recorded) does not match dist\ShellTidy.exe ($hash)"}
Write-Output "PASS SHA256SUMS.txt matches dist\ShellTidy.exe"
[pscustomobject]@{Path=$exe.FullName;Bytes=$exe.Length;Version=$exe.VersionInfo.ProductVersion;SHA256=$hash} | Format-List
Write-Output "Tools: $python | $node | $dumpbin"
if($Test){& (Join-Path $PSScriptRoot 'test.ps1')}
