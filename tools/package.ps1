param([string]$Output)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
Push-Location $projectRoot
try {
    if(!$Output) {$Output=Join-Path $projectRoot ('artifacts/release-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))}
    if(Test-Path -LiteralPath $Output) {throw 'Package destination already exists'}
    $plugin=Join-Path $Output 'Plugins/SDRSharp.T98'
    if(!(Test-Path build/native/t98_native.dll)) {throw 'Build the native receiver first'}
    $version=& ./tools/verify-version.ps1
    $assemblyPath=(Resolve-Path -LiteralPath 'src/SDRSharp.T98/bin/Release/net8.0-windows/SDRSharp.Plugin.T98.dll').Path
    $builtVersion=[Diagnostics.FileVersionInfo]::GetVersionInfo($assemblyPath).ProductVersion.Split('+')[0]
    if($builtVersion -ne $version) {throw "Rebuild the plugin: DLL version $builtVersion does not match $version"}
    New-Item -ItemType Directory -Force $plugin | Out-Null
    Copy-Item src/SDRSharp.T98/bin/Release/net8.0-windows/SDRSharp.Plugin.T98.dll $plugin
    foreach($name in @('t98_native.dll','ambe2.dll','libfftw3f-3.dll')) {Copy-Item (Join-Path 'build/native' $name) $plugin}
    Copy-Item -Recurse licenses (Join-Path $Output 'licenses')
    Copy-Item README.md,LICENSE,FORMAT.md,THIRD_PARTY_NOTICES.md $Output
    Set-Content -LiteralPath (Join-Path $Output 'VERSION.txt') -Value $version -Encoding ascii
    # Source allowlist: no development references, binaries, caches or captures.
    & python tools/source-archive.py (Join-Path $Output 'std-t98-receiver-sharp-source.zip')
    if($LASTEXITCODE) {throw 'Source archive failed'}
    $binaryContents=@('Plugins','licenses','README.md','LICENSE','FORMAT.md','THIRD_PARTY_NOTICES.md','VERSION.txt') | ForEach-Object {Join-Path $Output $_}
    Compress-Archive -LiteralPath $binaryContents -DestinationPath (Join-Path $Output 'STD-T98-Receiver.zip')
    $checksums=@()
    foreach($name in @('STD-T98-Receiver.zip','std-t98-receiver-sharp-source.zip')) {
        $hash=(Get-FileHash -LiteralPath (Join-Path $Output $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        $checksums+="$hash  $name"
    }
    foreach($file in Get-ChildItem -LiteralPath $plugin -File | Sort-Object Name) {
        $hash=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $checksums+="$hash  Plugins/SDRSharp.T98/$($file.Name)"
    }
    Set-Content -LiteralPath (Join-Path $Output 'SHA256SUMS.txt') -Value $checksums -Encoding ascii
    Write-Host "Ready: $Output"
} finally {Pop-Location}
