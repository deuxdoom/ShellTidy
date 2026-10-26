param([string]$ExePath,[string]$Report)
. (Join-Path $PSScriptRoot 'environment.ps1')
if(-not $ExePath){$ExePath=Join-Path $projectRoot 'dist\ShellTidy.exe'}
if(-not $Report){$Report=Join-Path $PSScriptRoot 'output\test-results.txt'}
New-Item -ItemType Directory -Force -Path (Split-Path $Report -Parent) | Out-Null
$env:SHELLTIDY_BACKUP_DIR=Join-Path $PSScriptRoot 'output\test-backups'
$test=Start-Process -FilePath $ExePath -ArgumentList @('--self-test',('"'+$Report+'"')) -PassThru -Wait -WindowStyle Hidden
Get-Content -LiteralPath $Report
if($test.ExitCode -ne 0){throw "Isolated registry tests failed: $($test.ExitCode)"}
