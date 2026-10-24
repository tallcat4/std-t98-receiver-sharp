param([string]$Tag)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
[xml]$project=Get-Content -LiteralPath (Join-Path $projectRoot 'src/SDRSharp.T98/SDRSharp.T98.csproj')
$version=[string]$project.Project.PropertyGroup.Version
if($version -notmatch '^\d+\.\d+\.\d+$') {throw "Invalid release version: $version"}
if($Tag -and $Tag -cne "v$version") {throw "Tag $Tag does not match project version v$version"}
Write-Output $version
