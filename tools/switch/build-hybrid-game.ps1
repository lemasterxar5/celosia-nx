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
# Lean variants (v105/v116): bit 0 alpha test, 1 background fade, 2 mips, 3 zero-add discard, 4 early tests,
# 5 near cull, 6 untextured, 7 opaque (no discards).
$lean=Get-Content -LiteralPath (Join-Path $backend 'lean.frag.glsl')
if($lean[0] -notmatch '^#version'){throw 'lean.frag.glsl must start with #version'}
$leanNames=@('LEAN_ALPHA','LEAN_BGFOG','LEAN_MIP','LEAN_ZEROADD','LEAN_EARLY','LEAN_NEARCULL','LEAN_UNTEXTURED','LEAN_OPAQUE')
for($v=0;$v -lt 256;++$v){
    $defines=@();for($b=0;$b -lt 8;++$b){if($v -band (1 -shl $b)){$defines+="#define $($leanNames[$b]) 1"}}
    $variantSource=Join-Path $shaderDir ('lean{0:D3}.frag.glsl' -f $v)
    Set-Content -LiteralPath $variantSource -Encoding ascii -Value (@($lean[0])+$defines+$lean[1..($lean.Count-1)])
    & "$DevkitPro/tools/bin/uam.exe" -s frag -o (Join-Path $shaderDir ('celosia_lean{0:D3}.frag.dksh' -f $v)) $variantSource
    if($LASTEXITCODE){throw "Switch lean shader variant $v compilation failed"}
    Remove-Item -LiteralPath $variantSource
}
Remove-Item -LiteralPath (Join-Path $shaderDir 'celosia_lean.frag.dksh') -ErrorAction SilentlyContinue
& "$DevkitPro/tools/bin/uam.exe" -s frag -o (Join-Path $shaderDir 'celosia_depth.frag.dksh') (Join-Path $backend 'depth.frag.glsl')
if($LASTEXITCODE){throw 'Switch depth pre-pass shader compilation failed'}
& "$DevkitPro/tools/bin/uam.exe" -s frag -o (Join-Path $shaderDir 'celosia_sky.frag.dksh') (Join-Path $backend 'sky.frag.glsl')
if($LASTEXITCODE){throw 'Switch background image shader compilation failed'}
& "$DevkitPro/tools/bin/uam.exe" -s frag -o (Join-Path $shaderDir 'celosia_bloom.frag.dksh') (Join-Path $backend 'bloom.frag.glsl')
if($LASTEXITCODE){throw 'Switch bloom shader compilation failed'}
# Early-depth-test variant of the generic shader, generated from the same source.
$earlySource=Join-Path $shaderDir 'native_early.frag.glsl'
$generic=Get-Content -LiteralPath (Join-Path $backend 'native.frag.glsl')
if($generic[0] -notmatch '^#version'){throw 'native.frag.glsl must start with #version'}
Set-Content -LiteralPath $earlySource -Encoding ascii -Value (@($generic[0],'layout(early_fragment_tests) in;')+$generic[1..($generic.Count-1)])
& "$DevkitPro/tools/bin/uam.exe" -s frag -o (Join-Path $shaderDir 'celosia_native_early.frag.dksh') $earlySource
if($LASTEXITCODE){throw 'Switch early-test shader compilation failed'}
Remove-Item -LiteralPath $earlySource
$elf=Join-Path $OutputDirectory 'celosia_switch_game.elf'
Copy-Item -LiteralPath (Join-Path $BuildDirectory 'celosia_switch_game.elf') -Destination $elf
$nacp=Join-Path $OutputDirectory 'control.nacp'
& "$DevkitPro/tools/bin/nacptool.exe" --create 'Celosia Switch development' 'Celosia contributors' '0.1.0' $nacp
if($LASTEXITCODE){throw 'Switch NACP generation failed'}
$nro=Join-Path $OutputDirectory 'celosia_switch_game.nro'
& "$DevkitPro/tools/bin/elf2nro.exe" $elf $nro "--nacp=$nacp" "--icon=$DevkitPro/libnx/default_icon.jpg" "--romfsdir=$(Join-Path $OutputDirectory 'romfs')"
if($LASTEXITCODE){throw 'Switch NRO packaging failed'}
Get-FileHash -LiteralPath $nro -Algorithm SHA256
