$ErrorActionPreference='Stop'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    & python tools/synthetic-check.py
    if($LASTEXITCODE) {throw 'Synthetic IQ check failed'}
    & python tools/check-native-model.py
    if($LASTEXITCODE) {throw 'Native model equivalence failed'}
    & python tools/privacy-check.py
    if($LASTEXITCODE) {throw 'Privacy check failed'}
    & dotnet build src/T98.Smoke -c Release --nologo -p:NuGetAudit=false -p:UseSharedCompilation=false
    if($LASTEXITCODE) {throw 'Smoke build failed'}
    Copy-Item build/native/*.dll src/T98.Smoke/bin/Release/net8.0-windows/
    & src/T98.Smoke/bin/Release/net8.0-windows/T98.Smoke.exe .
    if($LASTEXITCODE) {throw 'Host integration check failed'}
} finally {Pop-Location}
