[CmdletBinding()]
param(
    [string]$DevkitPro = 'C:/devkitPro',
    [Parameter(Mandatory)][string]$Python,
    [Parameter(Mandatory)][string]$GameArchive,
    [Parameter(Mandatory)][string]$NativeCoreArchive,
    [Parameter(Mandatory)][string]$NativeDataArchive,
    [string]$NativeServicesArchive,
    [Parameter(Mandatory)][string]$Sse2NeonDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
# DIAGNOSTIC ONLY. No assets are embedded and unavailable APIs always throw.
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$platform = Join-Path $sourceRoot 'PS2Recomp/ps2xRuntime/platform/switch'
$runtime = Join-Path $sourceRoot 'PS2Recomp/ps2xRuntime'
$backend = Join-Path $runtime 'src/lib/native/deko3d'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$GameArchive = (Resolve-Path -LiteralPath $GameArchive).Path
$NativeCoreArchive = (Resolve-Path -LiteralPath $NativeCoreArchive).Path
$NativeDataArchive = (Resolve-Path -LiteralPath $NativeDataArchive).Path
if(!$NativeServicesArchive) { $NativeServicesArchive=Join-Path (Split-Path $NativeCoreArchive) 'native-services/libps2_iop.a' }
$NativeServicesArchive=(Resolve-Path -LiteralPath $NativeServicesArchive).Path
$Sse2NeonDirectory = (Resolve-Path -LiteralPath $Sse2NeonDirectory).Path
$gxx = Join-Path $DevkitPro 'devkitA64/bin/aarch64-none-elf-g++.exe'
$nm = Join-Path $DevkitPro 'devkitA64/bin/aarch64-none-elf-nm.exe'
$elf2nro = Join-Path $DevkitPro 'tools/bin/elf2nro.exe'
$nacptool = Join-Path $DevkitPro 'tools/bin/nacptool.exe'
$uam = Join-Path $DevkitPro 'tools/bin/uam.exe'
$generator = Join-Path $PSScriptRoot 'generate-native-unavailable.py'
foreach($file in @($Python,$gxx,$nm,$elf2nro,$nacptool,$uam,$generator,(Join-Path $Sse2NeonDirectory 'sse2neon.h'))) {
    if(!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing input: $file" }
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$shaderDir = Join-Path $OutputDirectory 'romfs/shaders'
New-Item -ItemType Directory -Path $shaderDir -Force | Out-Null
foreach($family in @('native','shadow')){foreach($stage in @('vert','frag')) {
    & $uam -s $stage -o (Join-Path $shaderDir "celosia_$family.$stage.dksh") (Join-Path $backend "$family.$stage.glsl")
    if($LASTEXITCODE) { throw "Native shader compilation failed: $family.$stage" }
}
}
& $uam -s frag -o (Join-Path $shaderDir 'celosia_opaque.frag.dksh') (Join-Path $backend 'opaque.frag.glsl')
if($LASTEXITCODE){throw 'Native opaque shader compilation failed'}
$unavailable = Join-Path $OutputDirectory 'native_unavailable.cpp'
& $Python $generator --nm $nm --game-archive $GameArchive --native-archive $NativeCoreArchive `
    --native-archive $NativeDataArchive --native-archive $NativeServicesArchive --output $unavailable
if($LASTEXITCODE) { throw 'Diagnostic provider generation failed' }
$flags = @('-std=gnu++20','-O2','-g','-ffunction-sections','-fdata-sections',
    '-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-fPIE',
    '-D__SWITCH__','-DUSE_SSE2NEON','-DPS2X_SWITCH_NATIVE_ONLY=1','-DPS2X_NATIVE_DEKO3D=1','-DPS2X_IOP_NATIVE_ONLY=1',
    "-I$DevkitPro/libnx/include","-I$platform","-I$Sse2NeonDirectory",
    "-I$runtime/include","-I$runtime/src/lib/native","-I$sourceRoot/PS2Recomp/ps2xIOP/include")
$objects = @()
$sources = @((Join-Path $platform 'native_boot_probe.cpp'),(Join-Path $platform 'native_display.cpp'),
    (Join-Path $platform 'native_audio_output.cpp'),(Join-Path $platform 'native_lifecycle.cpp'),$unavailable)
$sources += Get-ChildItem -LiteralPath $backend -Filter '*.cpp' -File | Select-Object -ExpandProperty FullName
foreach($source in $sources) {
    $obj = Join-Path $OutputDirectory (([IO.Path]::GetFileNameWithoutExtension($source))+'.o')
    & $gxx @flags -c $source -o $obj
    if($LASTEXITCODE) { throw "Native boot compilation failed: $source" }
    $objects += $obj
}
$elf = Join-Path $OutputDirectory 'celosia-native-boot.elf'
$priorDevkitPro = $env:DEVKITPRO
try {
    $env:DEVKITPRO = $DevkitPro
    & $gxx @flags "-specs=$DevkitPro/libnx/switch.specs" @objects '-Wl,--start-group' `
        $GameArchive $NativeCoreArchive $NativeDataArchive $NativeServicesArchive '-Wl,--end-group' "-L$DevkitPro/libnx/lib" `
        '-Wl,--gc-sections' "-Wl,-Map,$OutputDirectory/celosia-native-boot.map" -ldeko3d -lnx -lm -o $elf
    if($LASTEXITCODE) { throw 'Full AOT native boot diagnostic link failed' }
} finally { $env:DEVKITPRO = $priorDevkitPro }
$nacp = Join-Path $OutputDirectory 'control.nacp'
& $nacptool --create 'Celosia native boot diagnostic' 'Celosia contributors' '0.0.1' $nacp
if($LASTEXITCODE) { throw 'Native boot metadata failed' }
$nro = Join-Path $OutputDirectory 'celosia-native-boot.nro'
& $elf2nro $elf $nro "--nacp=$nacp" "--icon=$DevkitPro/libnx/default_icon.jpg" "--romfsdir=$(Join-Path $OutputDirectory 'romfs')"
if($LASTEXITCODE) { throw 'Native boot packaging failed' }
Get-FileHash -LiteralPath $nro -Algorithm SHA256
