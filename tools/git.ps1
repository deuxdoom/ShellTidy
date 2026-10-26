param([Parameter(ValueFromRemainingArguments=$true)][string[]]$Arguments)
. (Join-Path $PSScriptRoot 'environment.ps1')
& $git -C $projectRoot @Arguments
exit $LASTEXITCODE
