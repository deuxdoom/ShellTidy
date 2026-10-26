param([switch]$SkipCompiler)
$ErrorActionPreference = 'Stop'
$toolRoot = $PSScriptRoot
$projectRoot = Split-Path $toolRoot -Parent
$runtime = Join-Path $toolRoot 'runtime'
$vendor = Join-Path $toolRoot 'vendor\python'
$cache = Join-Path $toolRoot 'cache'
New-Item -ItemType Directory -Force -Path $runtime,$vendor,$cache | Out-Null
$excludeFile=Join-Path $projectRoot '.git\info\exclude'
if(Test-Path -LiteralPath $excludeFile){
    $existing=Get-Content -LiteralPath $excludeFile
    foreach($pattern in @('/tools/runtime/','/tools/vendor/','/tools/toolchain/','/tools/cache/','/tools/output/','/.vscode/settings.json','/.gitignore')){
        if($pattern -notin $existing){Add-Content -LiteralPath $excludeFile -Value $pattern}
    }
}
$env:TEMP = $cache
$env:TMP = $cache

# Copy installed software as data. No executable on C: is launched by this script.
# Runtime/compiler copies are local dependencies and must never be committed.
function Copy-Tree([string]$Source,[string]$Destination,[string[]]$ExcludeNames=@()) {
    if (-not (Test-Path -LiteralPath $Source)) { throw "Missing tool source: $Source" }
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        if ($item.Name -in $ExcludeNames) { continue }
        $target = Join-Path $Destination $item.Name
        if ($item.PSIsContainer) { Copy-Tree $item.FullName $target $ExcludeNames }
        else { Copy-Item -LiteralPath $item.FullName -Destination $target -Force }
    }
}
function Copy-File([string]$Source,[string]$Destination) {
    if([IO.Path]::GetFullPath($Source) -eq [IO.Path]::GetFullPath($Destination)){return}
    if (Test-Path -LiteralPath $Source) {
        New-Item -ItemType Directory -Force -Path (Split-Path $Destination -Parent) | Out-Null
        Copy-Item -LiteralPath $Source -Destination $Destination -Force
    }
}

$pythonSource = Split-Path (Get-Command python.exe).Source -Parent
$pythonTarget = Join-Path $runtime 'python'
if (-not (Test-Path -LiteralPath (Join-Path $pythonTarget '.ready'))) {
    New-Item -ItemType Directory -Force -Path $pythonTarget | Out-Null
    foreach ($file in Get-ChildItem -LiteralPath $pythonSource -File) {
        if ($file.Extension -in @('.exe','.dll') -or $file.Name -like '*LICENSE*') {
            Copy-File $file.FullName (Join-Path $pythonTarget $file.Name)
        }
    }
    Copy-Tree (Join-Path $pythonSource 'Lib') (Join-Path $pythonTarget 'Lib') @('site-packages','__pycache__','test','tests','idlelib','tkinter','turtledemo')
    Copy-Tree (Join-Path $pythonSource 'DLLs') (Join-Path $pythonTarget 'DLLs') @('__pycache__')
    Set-Content -LiteralPath (Join-Path $pythonTarget '.ready') -Value 'local Python copy'
}
$sitePackages = Join-Path $pythonSource 'Lib\site-packages'
foreach ($package in Get-ChildItem -LiteralPath $sitePackages -Directory -ErrorAction SilentlyContinue) {
    if ($package.Name -eq 'PIL' -or $package.Name -like 'pillow*') {
        Copy-Tree $package.FullName (Join-Path $vendor $package.Name) @('__pycache__')
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $vendor 'resvg_py\__init__.py')) -or -not (Test-Path -LiteralPath (Join-Path $vendor 'PIL\__init__.py'))) {
    $env:PYTHONHOME=$pythonTarget
    $env:PYTHONPATH=$vendor
    $env:PYTHONNOUSERSITE='1'
    $env:PYTHONDONTWRITEBYTECODE='1'
    $env:PIP_CONFIG_FILE='NUL'
    $localPython=Join-Path $pythonTarget 'python.exe'
    & $localPython -B -m ensurepip --upgrade
    if($LASTEXITCODE -ne 0){throw 'Local pip initialization failed'}
    & $localPython -B -m pip install --disable-pip-version-check --no-compile --no-cache-dir --target $vendor Pillow resvg_py==0.5.0
    if($LASTEXITCODE -ne 0){throw 'Local image dependencies could not be installed'}
}

Copy-File (Get-Command node.exe).Source (Join-Path $runtime 'node\node.exe')
Copy-File (Get-Command gh.exe).Source (Join-Path $runtime 'gh\gh.exe')
$gitSource = Split-Path (Get-Command git.exe).Source -Parent
while ($gitSource -and -not (Test-Path -LiteralPath (Join-Path $gitSource 'mingw64')) -and -not (Test-Path -LiteralPath (Join-Path $gitSource 'ucrt64'))) {
    $gitSource = Split-Path $gitSource -Parent
}
if (-not $gitSource) { throw 'Cannot locate the installed Git distribution.' }
$gitTarget = Join-Path $runtime 'git'
$gitFlavor = if(Test-Path -LiteralPath (Join-Path $gitSource 'ucrt64')){'ucrt64'}else{'mingw64'}
if (-not (Test-Path -LiteralPath (Join-Path $gitTarget '.ready'))) {
    foreach ($part in @('cmd',$gitFlavor,'usr\bin','etc')) {
        Copy-Tree (Join-Path $gitSource $part) (Join-Path $gitTarget $part) @('doc','man','__pycache__')
    }
    Copy-File (Join-Path $gitSource 'LICENSE.txt') (Join-Path $gitTarget 'LICENSE.txt')
    Set-Content -LiteralPath (Join-Path $gitTarget '.ready') -Value 'local Git copy'
}
Set-Content -LiteralPath (Join-Path $runtime 'git-flavor.txt') -Value $gitFlavor

$fontTarget = Join-Path $runtime 'fonts'
foreach ($name in @('segoeui.ttf','segoeuib.ttf','seguisb.ttf','malgun.ttf','malgunbd.ttf')) {
    Copy-File (Join-Path $env:WINDIR ('Fonts\'+$name)) (Join-Path $fontTarget $name)
}
$vsRoot = 'C:\Program Files\Microsoft Visual Studio\18\Insiders'
Copy-File (Join-Path $vsRoot 'VC\Tools\Llvm\x64\bin\clang-format.exe') (Join-Path $runtime 'format\clang-format.exe')
foreach ($file in Get-ChildItem -LiteralPath $pythonTarget -File -Filter 'vcruntime*.dll') {
    Copy-File $file.FullName (Join-Path $runtime ('format\'+$file.Name))
}

if (-not $SkipCompiler) {
    $msvcVersion = (Get-ChildItem -LiteralPath (Join-Path $vsRoot 'VC\Tools\MSVC') -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
    $msvcSource = Join-Path $vsRoot ('VC\Tools\MSVC\'+$msvcVersion)
    $msvcTarget = Join-Path $toolRoot ('toolchain\msvc\'+$msvcVersion)
    if (-not (Test-Path -LiteralPath (Join-Path $msvcTarget '.ready'))) {
        foreach ($part in @('bin\Hostx64\x64','include','lib\x64')) {
            Copy-Tree (Join-Path $msvcSource $part) (Join-Path $msvcTarget $part)
        }
        Set-Content -LiteralPath (Join-Path $msvcTarget '.ready') -Value $msvcVersion
    }
    $sdkSource = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    $sdkVersion = (Get-ChildItem -LiteralPath (Join-Path $sdkSource 'Include') -Directory | Where-Object Name -match '^10\.0\.' | Sort-Object Name -Descending | Select-Object -First 1).Name
    $sdkTarget = Join-Path $toolRoot ('toolchain\windows-sdk\'+$sdkVersion)
    if (-not (Test-Path -LiteralPath (Join-Path $sdkTarget '.ready'))) {
        Copy-Tree (Join-Path $sdkSource ('Include\'+$sdkVersion)) (Join-Path $sdkTarget 'Include')
        foreach ($part in @('um\x64','ucrt\x64')) {
            Copy-Tree (Join-Path $sdkSource ('Lib\'+$sdkVersion+'\'+$part)) (Join-Path $sdkTarget ('Lib\'+$part))
        }
        $sdkBin = Join-Path $sdkSource ('bin\'+$sdkVersion+'\x64')
        foreach ($name in @('rc.exe','rcdll.dll','mt.exe')) { Copy-File (Join-Path $sdkBin $name) (Join-Path $sdkTarget ('bin\x64\'+$name)) }
        if (Test-Path -LiteralPath (Join-Path $sdkBin '1033')) { Copy-Tree (Join-Path $sdkBin '1033') (Join-Path $sdkTarget 'bin\x64\1033') }
        Set-Content -LiteralPath (Join-Path $sdkTarget '.ready') -Value $sdkVersion
    }
    @{msvc=$msvcTarget;sdk=$sdkTarget} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runtime 'toolchain.json') -Encoding utf8
}
Write-Output "Project-local tools prepared under $toolRoot"
