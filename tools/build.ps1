param([string]$SdrSharpExe,[string]$Compiler='g++')
$ErrorActionPreference='Stop'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    New-Item -ItemType Directory -Force build/native,build/sdk | Out-Null
    if($SdrSharpExe) { & python tools/extract-sdk.py $SdrSharpExe; if($LASTEXITCODE) {throw 'SDK extraction failed'} }
    if(!(Test-Path build/sdk/SDRSharp.Common.dll)) {throw 'Supply -SdrSharpExe with your SDR# x64 executable'}
    if(!(Test-Path build/fftw/fftw3.h)) { & ./tools/setup-fftw.ps1 }
    & python tools/embed-model.py
    if($LASTEXITCODE) {throw 'Model embedding failed'}
    & gcc -c build/native/model.S -o build/native/model.o
    if($LASTEXITCODE) {throw 'Model assembly failed'}
    $codecSources=@('b25_dsp_a.c','b25_dsp_b.c','b25_decode_c.c','b25_decode_d.c','b25_tables.c','b25_golay.c','blip25.c','mbelib_compat.c','ambeplus.c','ambeplus_tables.c','ambeplus_fec.c','ambeplus4000_tables.c','ambeplus_layer2.c') | ForEach-Object {"third_party/ambe2/src/$_"}
    & gcc -std=c99 -O2 -shared -static -Ithird_party/ambe2/include -Ithird_party/ambe2/src @codecSources -o build/native/ambe2.dll
    if($LASTEXITCODE) {throw 'AMBE build failed'}
    $sources=@('native/t98.cpp','native/secret/pn.cpp','third_party/spear/apps/std_t98/receiver.cpp','third_party/spear/apps/std_t98/protocol/protocol.cpp','third_party/spear/apps/std_t98/dsp/rrc.cpp','third_party/spear/apps/std_t98/dsp/symbol_sync.cpp','third_party/spear/apps/std_t98/dsp/sync_correlator.cpp','third_party/spear/dsp/src/fir.cpp','third_party/spear/dsp/src/fftw_planner.cpp')
    $sources+=@('native/secret_api.cpp','native/secret/guided.cpp','native/secret/hybrid.cpp')
    & $Compiler -std=c++20 -O3 -shared -static -Inative -Inative/include -Ibuild/native -Ithird_party/ambe2/include -Ithird_party/spear/apps/std_t98 -Ithird_party/spear/dsp/include -Ibuild/fftw @sources build/native/model.o build/native/ambe2.dll build/fftw/libfftw3f-3.dll -o build/native/t98_native.dll
    if($LASTEXITCODE) {throw 'Receiver build failed'}
    Copy-Item build/fftw/libfftw3f-3.dll build/native/
    & dotnet build src/SDRSharp.T98 -c Release --nologo -p:NuGetAudit=false -p:UseSharedCompilation=false
    if($LASTEXITCODE) {throw 'Plugin build failed'}
} finally {Pop-Location}
