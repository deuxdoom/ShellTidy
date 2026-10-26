param([ValidateSet('Social','Screenshots','Icons','All')][string]$Asset='All')
. (Join-Path $PSScriptRoot 'environment.ps1')
$scripts=@()
if($Asset -in @('Icons','All')){$scripts+='generate_icons.py'}
if($Asset -in @('Social','All')){$scripts+='generate_social_preview.py'}
if($Asset -in @('Screenshots','All')){$scripts+='generate_ui_previews.py'}
foreach($script in $scripts){& $python -B (Join-Path $PSScriptRoot $script);if($LASTEXITCODE -ne 0){throw "Asset generation failed: $script"}}
