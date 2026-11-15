[CmdletBinding()]
param(
    [string]$DevkitPro = 'C:/devkitPro',
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$OutputDirectory) { $OutputDirectory = Join-Path $sourceRoot 'switch-build/deko-probe' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$runtime = Join-Path $sourceRoot 'PS2Recomp/ps2xRuntime'
$backend = Join-Path $runtime 'src/lib/native/deko3d'
$gxx = Join-Path $DevkitPro 'devkitA64/bin/aarch64-none-elf-g++.exe'
$uam = Join-Path $DevkitPro 'tools/bin/uam.exe'
$elf2nro = Join-Path $DevkitPro 'tools/bin/elf2nro.exe'
$nacptool = Join-Path $DevkitPro 'tools/bin/nacptool.exe'
$icon = Join-Path $DevkitPro 'libnx/default_icon.jpg'
foreach ($tool in @($gxx, $uam, $elf2nro, $nacptool, $icon)) {
    if (!(Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Missing tool: $tool" }
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$shaderDir = Join-Path $OutputDirectory 'romfs/shaders'
New-Item -ItemType Directory -Force -Path $shaderDir | Out-Null
foreach($family in @('native','shadow')){foreach ($stage in @(@('vert','vert'), @('frag','frag'))) {
    $inputShader = Join-Path $backend "$family.$($stage[0]).glsl"
    $outputShader = Join-Path $shaderDir "celosia_$family.$($stage[0]).dksh"
    & $uam -s $stage[1] -o $outputShader $inputShader
    if ($LASTEXITCODE) { throw "Shader build failed: $inputShader" }
}
}
& $uam -s frag -o (Join-Path $shaderDir 'celosia_opaque.frag.dksh') (Join-Path $backend 'opaque.frag.glsl')
if($LASTEXITCODE){throw 'Opaque shader build failed'}
$flags = @('-std=gnu++20','-O2','-g','-Wall','-Wextra','-ffunction-sections',
    '-fdata-sections','-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft',
    '-fPIE','-D__SWITCH__','-DPS2X_NATIVE_DEKO3D=1',
    "-I$DevkitPro/libnx/include", "-I$runtime/include", "-I$runtime/src/lib/native")
$sources = @(Get-ChildItem -LiteralPath $backend -Filter '*.cpp' -File |
    Where-Object Name -NotMatch '_test\.cpp$' | Select-Object -ExpandProperty FullName)
if (!$sources.Count) { throw 'No deko3d backend implementation found' }
$sources += Join-Path $runtime 'platform/switch/deko_probe.cpp'
$objects = @()
foreach ($source in $sources) {
    $obj = Join-Path $OutputDirectory (([IO.Path]::GetFileNameWithoutExtension($source)) + '.o')
    & $gxx @flags -c $source -o $obj
    if ($LASTEXITCODE) { throw "Compilation failed: $source" }
    $objects += $obj
}
$elf = Join-Path $OutputDirectory 'celosia-deko-probe.elf'
$map = Join-Path $OutputDirectory 'celosia-deko-probe.map'
if ($env:DEVKITPRO -ne $DevkitPro) {
    # switch.specs expands this variable inside the compiler process.
    $priorDevkitPro = $env:DEVKITPRO
    $env:DEVKITPRO = $DevkitPro
}
try {
& $gxx @flags "-specs=$DevkitPro/libnx/switch.specs" @objects `
    "-L$DevkitPro/libnx/lib" '-Wl,--gc-sections' "-Wl,-Map,$map" -ldeko3d -lnx -lm -o $elf
if ($LASTEXITCODE) { throw 'Probe link failed' }
} finally {
    if (Test-Path variable:priorDevkitPro) { $env:DEVKITPRO = $priorDevkitPro }
}
$nro = Join-Path $OutputDirectory 'celosia-deko-probe.nro'
$nacp = Join-Path $OutputDirectory 'control.nacp'
& $nacptool --create 'Celosia deko3d diagnostic' 'Celosia contributors' '0.0.1' $nacp
if ($LASTEXITCODE) { throw 'NACP creation failed' }
& $elf2nro $elf $nro "--nacp=$nacp" "--icon=$icon" "--romfsdir=$(Join-Path $OutputDirectory 'romfs')"
if ($LASTEXITCODE) { throw 'NRO conversion failed' }
Get-FileHash -LiteralPath $nro -Algorithm SHA256 | Format-List
Write-Output 'Built backend diagnostic only. This NRO contains no game and proves no frame-rate target.'
