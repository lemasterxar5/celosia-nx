[CmdletBinding()]
param(
    [string]$DevkitPro,
    [string]$CMake,
    [string]$Ninja,
    [ValidateRange(1,8)][int]$Jobs = 2,
    [switch]$Diagnostics,
    [switch]$ConfigureOnly,
    # Optional optimisation (defaults reproduce the previous build): separate build root,
    # link-time optimisation and profile-guided optimisation (see tools/switch/optimization.cmake).
    [string]$BuildRoot = 'build',
    [switch]$Lto,
    [ValidateSet('OFF','GENERATE','USE')][string]$Pgo = 'OFF',
    [string]$PgoDir = '',
    [ValidateSet('all','runtime')][string]$PgoScope = 'all'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$settings = Get-Content -LiteralPath (Join-Path $root 'local-tools.json') -Raw | ConvertFrom-Json
if (!$DevkitPro) {
    $environmentCompiler = if ($env:DEVKITPRO) { Join-Path $env:DEVKITPRO 'devkitA64/bin/aarch64-none-elf-g++.exe' }
    $DevkitPro = if ($environmentCompiler -and (Test-Path -LiteralPath $environmentCompiler -PathType Leaf)) { $env:DEVKITPRO } else { $settings.DevkitPro }
}
if (!$CMake) {
    if (Test-Path -LiteralPath $settings.CMake -PathType Leaf) { $CMake = $settings.CMake }
    else { $command = Get-Command cmake -ErrorAction SilentlyContinue; if ($command) { $CMake = $command.Source } }
}
if (!$Ninja) {
    if (Test-Path -LiteralPath $settings.Ninja -PathType Leaf) { $Ninja = $settings.Ninja }
    else { $command = Get-Command ninja -ErrorAction SilentlyContinue; if ($command) { $Ninja = $command.Source } }
}
$compiler = Join-Path $DevkitPro 'devkitA64/bin/aarch64-none-elf-g++.exe'
$runner = Join-Path $root 'private/generated'
$sse = Join-Path $root 'dependencies/sse2neon'
$buildBase = Join-Path $root $BuildRoot
$aotBuild = Join-Path $buildBase 'aot'
$runtimeBuild = Join-Path $buildBase 'switch'
$package = Join-Path $buildBase 'package'
$deko = if ($Diagnostics) { 'libdeko3dd.a' } else { 'libdeko3d.a' }
$required = @($CMake,$Ninja,$compiler,
    (Join-Path $DevkitPro 'libnx/include/switch.h'),
    (Join-Path $DevkitPro 'libnx/lib/libnx.a'),
    (Join-Path $DevkitPro "libnx/lib/$deko"),
    (Join-Path $DevkitPro 'portlibs/switch/lib/libz.a'),
    (Join-Path $DevkitPro 'portlibs/switch/include/zlib.h'),
    (Join-Path $DevkitPro 'tools/bin/uam.exe'),
    (Join-Path $DevkitPro 'tools/bin/nacptool.exe'),
    (Join-Path $DevkitPro 'tools/bin/elf2nro.exe'),
    (Join-Path $DevkitPro 'libnx/default_icon.jpg'),
    (Join-Path $sse 'sse2neon.h'),
    (Join-Path $runner 'ps2_recompiled_functions.h'),
    (Join-Path $runner 'register_functions.cpp'))
foreach ($file in $required) {
    if (!$file -or !(Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Missing build input: $file. Set -DevkitPro, -CMake or -Ninja for this machine."
    }
}
$cross = @('-G','Ninja','-DCMAKE_SYSTEM_NAME=Generic',
    '-DCMAKE_SYSTEM_PROCESSOR=aarch64','-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
    "-DCMAKE_CXX_COMPILER=$compiler","-DCMAKE_MAKE_PROGRAM=$Ninja",'-DCMAKE_BUILD_TYPE=Release')
$ltoValue = if ($Lto) { 'ON' } else { 'OFF' }
$cross += @("-DCELOSIA_LTO=$ltoValue","-DCELOSIA_PGO=$Pgo","-DCELOSIA_PGO_DIR=$PgoDir","-DCELOSIA_PGO_SCOPE=$PgoScope")
if ($Lto) {
    # LTO IR objects must be archived through the plugin-aware wrappers.
    $cross += @("-DCMAKE_AR=$(Join-Path $DevkitPro 'devkitA64/bin/aarch64-none-elf-gcc-ar.exe')",
        "-DCMAKE_RANLIB=$(Join-Path $DevkitPro 'devkitA64/bin/aarch64-none-elf-gcc-ranlib.exe')")
}
$priorSdk = $env:DEVKITPRO
try {
    $env:DEVKITPRO = $DevkitPro
    # Parallel LTRANS in a Windows-hosted GCC runs through make; devkitPro's msys2 provides it.
    if ($Lto) { $env:PATH = "$(Join-Path $DevkitPro 'msys2/usr/bin');$env:PATH" }
    & $CMake -S (Join-Path $root 'tools/switch/aot') -B $aotBuild @cross `
        "-DCELOSIA_RUNNER_DIR=$runner" "-DCELOSIA_SSE2NEON_DIR=$sse" -DCELOSIA_AOT_NATIVE_ONLY=OFF
    if ($LASTEXITCODE) { throw 'AOT configuration failed' }
    $diagnosticValue = if ($Diagnostics) { 'ON' } else { 'OFF' }
    & $CMake -S (Join-Path $root 'tools/switch/hybrid-runtime') -B $runtimeBuild @cross `
        "-DCELOSIA_DEVKITPRO=$DevkitPro" "-DCELOSIA_SSE2NEON_DIR=$sse" `
        "-DCELOSIA_HYBRID_GAME_ARCHIVE=$aotBuild/libcelosia_game_aot.a" `
        "-DCELOSIA_HYBRID_GAME_HEADERS=$runner" "-DCELOSIA_DEKO_DIAGNOSTICS=$diagnosticValue" `
        -DCELOSIA_HYBRID_RELOCATION_AUDIT=OFF
    if ($LASTEXITCODE) { throw 'Switch runtime configuration failed' }
    if ($ConfigureOnly) { Write-Output 'Dependency checks and both configurations passed.'; return }
    & $CMake --build $aotBuild --target celosia_game_aot --parallel $Jobs
    if ($LASTEXITCODE) { throw 'Private game AOT compilation failed' }
    & (Join-Path $root 'tools/switch/build-hybrid-game.ps1') -CMake $CMake `
        -BuildDirectory $runtimeBuild -OutputDirectory $package -DevkitPro $DevkitPro -Jobs $Jobs
    if (!$?) { throw 'NRO compilation or packaging failed' }
    Write-Output "NRO ready: $package/celosia_nx.nro"
} finally { $env:DEVKITPRO = $priorSdk }
