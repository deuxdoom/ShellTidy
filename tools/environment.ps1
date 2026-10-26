$ErrorActionPreference = 'Stop'
$toolRoot = $PSScriptRoot
$projectRoot = Split-Path $toolRoot -Parent
$runtime = Join-Path $toolRoot 'runtime'
$cache = Join-Path $toolRoot 'cache'
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$env:TEMP=$cache
$env:TMP=$cache
$env:PYTHONHOME=Join-Path $runtime 'python'
$env:PYTHONPATH=Join-Path $toolRoot 'vendor\python'
$env:PYTHONDONTWRITEBYTECODE='1'
$env:PYTHONNOUSERSITE='1'
$env:PIP_CACHE_DIR=Join-Path $cache 'pip'
$env:PIP_CONFIG_FILE='NUL'
$env:SHELLTIDY_FONT_DIR=Join-Path $runtime 'fonts'
$gitFlavor=(Get-Content -LiteralPath (Join-Path $runtime 'git-flavor.txt') -Raw).Trim()
$env:GIT_EXEC_PATH=Join-Path $runtime ('git\'+$gitFlavor+'\libexec\git-core')
$env:PATH=((@('python','node','gh','git\cmd','git\usr\bin',('git\'+$gitFlavor+'\bin'),'format') | ForEach-Object {Join-Path $runtime $_}) -join ';')+';'+$env:PATH
$python=Join-Path $runtime 'python\python.exe'
$node=Join-Path $runtime 'node\node.exe'
$git=Join-Path $runtime 'git\cmd\git.exe'
$gh=Join-Path $runtime 'gh\gh.exe'
foreach($program in @($python,$node,$git,$gh)){if(-not (Test-Path -LiteralPath $program)){throw 'Run tools/setup-local-tools.ps1 first.'}}
$gitCa=Join-Path $runtime ('git\'+$gitFlavor+'\ssl\certs\ca-bundle.crt')
if(Test-Path -LiteralPath $gitCa){$env:GIT_SSL_CAINFO=$gitCa}
# Reuse the existing account through the local CLI, never a C:-drive helper executable.
$env:GIT_CONFIG_COUNT='2'
$env:GIT_CONFIG_KEY_0='credential.helper'
$env:GIT_CONFIG_VALUE_0=''
$env:GIT_CONFIG_KEY_1='credential.helper'
$env:GIT_CONFIG_VALUE_1='!"'+$gh.Replace('\','/')+'" auth git-credential'
