$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$folder=Join-Path $projectRoot 'build/fftw'
New-Item -ItemType Directory -Force $folder | Out-Null
$zipPath=Join-Path $folder 'fftw.zip'
Invoke-WebRequest 'https://www.fftw.org/pub/fftw/fftw-3.3.5-dll64.zip' -OutFile $zipPath
if((Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash -ne 'CFD88DC0E8D7001115EA79E069A2C695D52C8947F5B4F3B7AC54A192756F439F') {throw 'FFTW checksum mismatch'}
Expand-Archive -LiteralPath $zipPath -DestinationPath $folder -Force
