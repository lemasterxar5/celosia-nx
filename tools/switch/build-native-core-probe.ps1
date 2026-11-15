[CmdletBinding()]
param(
    [string]$DevkitPro = 'C:/devkitPro',
    [Parameter(Mandatory)][string]$GeneratedRunner,
    [Parameter(Mandatory)][string]$Sse2NeonDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$runtime = Join-Path $sourceRoot 'PS2Recomp/ps2xRuntime'
$platform = Join-Path $runtime 'platform/switch'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$GeneratedRunner = (Resolve-Path -LiteralPath $GeneratedRunner).Path
$Sse2NeonDirectory = (Resolve-Path -LiteralPath $Sse2NeonDirectory).Path
$generated = Join-Path $GeneratedRunner 'iosAbsVector_0x1be898.cpp'
$bootEntry = Join-Path $GeneratedRunner 'sub_00100010_0x100010.cpp'
$libraryInputs=@('ps2__InitSys_0x10fdd8.cpp','sub_0010FB88_0x10fb88.cpp','CreateSema_0x106fe0.cpp') |
    ForEach-Object { Join-Path $GeneratedRunner $_ }
$gxx = Join-Path $DevkitPro 'devkitA64/bin/aarch64-none-elf-g++.exe'
$elf2nro = Join-Path $DevkitPro 'tools/bin/elf2nro.exe'
$nacptool = Join-Path $DevkitPro 'tools/bin/nacptool.exe'
foreach ($file in @($generated,$bootEntry,$gxx,$elf2nro,$nacptool,(Join-Path $Sse2NeonDirectory 'sse2neon.h'))+$libraryInputs) {
    if (!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing input: $file" }
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$flags = @('-std=gnu++20','-O2','-g','-ffunction-sections','-fdata-sections',
    '-march=armv8-a+crc+crypto','-mtune=cortex-a57','-mtp=soft','-fPIE',
    '-D__SWITCH__','-DUSE_SSE2NEON','-DPS2X_SWITCH_NATIVE_ONLY=1',
    '-DPS2X_GUEST_READ_WATCH=0','-DPS2X_GUEST_WRITE_TRACE=0',
    "-I$DevkitPro/libnx/include","-I$platform","-I$runtime/include", "-I$runtime/src/lib/Kernel",
    "-I$GeneratedRunner","-I$Sse2NeonDirectory")
$objects = @()
$sources = @('native_memory.cpp','native_aot_runtime.cpp','native_aot_memory.cpp','native_heap.cpp',
    'native_boot_services.cpp','native_semaphore.cpp','native_sync.cpp','native_clock.cpp','native_library.cpp',
    'native_cache.cpp','native_threads.cpp','native_audio_output.cpp','native_core_probe.cpp') |
    ForEach-Object { Join-Path $platform $_ }
$sources += $generated
$sources += $bootEntry
$sources += $libraryInputs
foreach ($source in $sources) {
    $obj = Join-Path $OutputDirectory (([IO.Path]::GetFileNameWithoutExtension($source))+'.o')
    & $gxx @flags -c $source -o $obj
    if ($LASTEXITCODE) { throw "Native core compilation failed: $source" }
    $objects += $obj
}
$elf = Join-Path $OutputDirectory 'celosia-native-core-probe.elf'
$priorDevkitPro = $env:DEVKITPRO
try {
    $env:DEVKITPRO = $DevkitPro
    & $gxx @flags "-specs=$DevkitPro/libnx/switch.specs" @objects "-L$DevkitPro/libnx/lib" `
        '-Wl,--gc-sections' "-Wl,-Map,$OutputDirectory/celosia-native-core-probe.map" -lnx -lm -o $elf
    if ($LASTEXITCODE) { throw 'Native core probe link failed' }
} finally { $env:DEVKITPRO = $priorDevkitPro }
$nacp = Join-Path $OutputDirectory 'control.nacp'
& $nacptool --create 'Celosia native core diagnostic' 'Celosia contributors' '0.0.1' $nacp
if ($LASTEXITCODE) { throw 'Native core metadata failed' }
$nro = Join-Path $OutputDirectory 'celosia-native-core-probe.nro'
& $elf2nro $elf $nro "--nacp=$nacp" "--icon=$DevkitPro/libnx/default_icon.jpg"
if ($LASTEXITCODE) { throw 'Native core packaging failed' }
Get-FileHash -LiteralPath $nro -Algorithm SHA256
