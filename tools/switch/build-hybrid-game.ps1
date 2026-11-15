[CmdletBinding()]
param(
    [string]$CMake='cmake.exe',
    [string]$BuildDirectory,
    [switch]$PackageOnly,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$DevkitPro='C:/devkitPro',
    [ValidateRange(1,8)][int]$Jobs=4
)
$ErrorActionPreference='Stop'
$sourceRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if(!$BuildDirectory){$BuildDirectory=Join-Path ([IO.Path]::GetFullPath((Join-Path $sourceRoot '../..'))) 'hybrid-runtime-arm'}
$BuildDirectory=(Resolve-Path -LiteralPath $BuildDirectory).Path
if((Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Raw) -match 'CELOSIA_HYBRID_RELOCATION_AUDIT:BOOL=ON') {
    throw 'Audit-only ELF cannot be packaged as a game NRO'
}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$priorDevkitPro=$env:DEVKITPRO
try {
    $env:DEVKITPRO=$DevkitPro
    if(!$PackageOnly) {
        & $CMake --build $BuildDirectory --target celosia_switch_game -j $Jobs *> (Join-Path $OutputDirectory 'build.log')
        if($LASTEXITCODE){Get-Content (Join-Path $OutputDirectory 'build.log') -Tail 16;throw 'Switch game build failed'}
    }
} finally {$env:DEVKITPRO=$priorDevkitPro}
$shaderDir=Join-Path $OutputDirectory 'romfs/shaders'
New-Item -ItemType Directory -Force -Path $shaderDir | Out-Null
$backend=Join-Path $sourceRoot 'PS2Recomp/ps2xRuntime/src/lib/native/deko3d'
foreach($family in @('native','shadow')){foreach($stage in @('vert','frag')){
    & "$DevkitPro/tools/bin/uam.exe" -s $stage -o (Join-Path $shaderDir "celosia_$family.$stage.dksh") (Join-Path $backend "$family.$stage.glsl")
    if($LASTEXITCODE){throw "Switch shader compilation failed: $family.$stage"}
}
}
& "$DevkitPro/tools/bin/uam.exe" -s frag -o (Join-Path $shaderDir 'celosia_opaque.frag.dksh') (Join-Path $backend 'opaque.frag.glsl')
if($LASTEXITCODE){throw 'Switch opaque shader compilation failed'}
$elf=Join-Path $OutputDirectory 'celosia_switch_game.elf'
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'celosia_switch_game.elf') -Destination $elf
$nacp=Join-Path $OutputDirectory 'control.nacp'
& "$DevkitPro/tools/bin/nacptool.exe" --create 'Celosia Switch development' 'Celosia contributors' '0.1.0' $nacp
if($LASTEXITCODE){throw 'Switch NACP generation failed'}
$nro=Join-Path $OutputDirectory 'celosia_switch_game.nro'
& "$DevkitPro/tools/bin/elf2nro.exe" $elf $nro "--nacp=$nacp" "--icon=$DevkitPro/libnx/default_icon.jpg" "--romfsdir=$(Join-Path $OutputDirectory 'romfs')"
if($LASTEXITCODE){throw 'Switch NRO packaging failed'}
Get-FileHash -LiteralPath $nro -Algorithm SHA256
