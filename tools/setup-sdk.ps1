# Build references only: SDR# and its reference assemblies are never packaged.
# CI repository variables SDRSHARP_DOWNLOAD_URL / SDRSHARP_SHA256 can pin a
# compatible official archive when Airspy changes its current download.
param(
    [string]$Archive,
    [string]$Url='https://airspy.com/downloads/sdrsharp-x64.zip',
    [string]$Sha256
)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$downloadRoot=Join-Path $projectRoot 'build/sdrsharp-sdk'
New-Item -ItemType Directory -Force $downloadRoot | Out-Null
if(!$Archive) {
    $Archive=Join-Path $downloadRoot 'sdrsharp.zip'
    Invoke-WebRequest -Uri $Url -OutFile $Archive
}
if($Sha256 -and (Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ine $Sha256) {throw 'SDR# archive checksum mismatch'}
# A fresh directory prevents an earlier extraction from masking an incompatible archive.
$expanded=Join-Path $downloadRoot ([Guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $Archive -DestinationPath $expanded
$executables=@(Get-ChildItem -LiteralPath $expanded -Recurse -File -Filter 'SDRSharp.dotnet8.exe')
if($executables.Count -ne 1) {throw 'Expected one SDRSharp.dotnet8.exe in the x64 archive. Set SDRSHARP_DOWNLOAD_URL to a compatible official archive.'}
& python (Join-Path $PSScriptRoot 'extract-sdk.py') $executables[0].FullName --output (Join-Path $projectRoot 'build/sdk')
if($LASTEXITCODE) {throw 'SDR# reference extraction failed'}
