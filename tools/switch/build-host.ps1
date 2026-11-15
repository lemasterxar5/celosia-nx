[CmdletBinding()]
param(
    [string]$CMake = 'cmake.exe',
    [string]$Ninja = 'ninja.exe',
    [string]$Git = 'git.exe',
    [string]$Clang = 'clang.exe',
    [string]$ClangXX = 'clang++.exe',
    [string]$BuildDirectory,
    [ValidateRange(1,64)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
if (!$BuildDirectory) { $BuildDirectory = Join-Path $sourceRoot 'build/host-tools' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
function Find-Tool([string]$Name) {
    $resolved = (Get-Command $Name -ErrorAction Stop).Source
    if ($resolved.Replace('\','/') -match '/msys2/usr/bin/') {
        throw "Use a native Windows executable instead of MSYS: $Name"
    }
    return $resolved.Replace('\','/')
}
$cm = Find-Tool $CMake
$nj = Find-Tool $Ninja
$gt = Find-Tool $Git
$cc = Find-Tool $Clang
$cxx = Find-Tool $ClangXX
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null
$configureLog = Join-Path $BuildDirectory 'configure.log'
& $cm -S (Join-Path $sourceRoot 'PS2Recomp') -B $BuildDirectory -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$nj" "-DGIT_EXECUTABLE=$gt" `
    "-DCMAKE_C_COMPILER=$cc" "-DCMAKE_CXX_COMPILER=$cxx" -DCMAKE_BUILD_TYPE=Release `
    -DPS2X_BUILD_RECOMP=ON -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_ANALYZER=OFF `
    -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF *> $configureLog
if ($LASTEXITCODE) { Get-Content $configureLog -Tail 16; throw 'Recompiler configuration failed' }
$buildLog = Join-Path $BuildDirectory 'build.log'
& $cm --build $BuildDirectory --target ps2_recomp -j $Jobs *> $buildLog
if ($LASTEXITCODE) { Get-Content $buildLog -Tail 16; throw 'Recompiler compilation failed' }
Write-Output (Join-Path $BuildDirectory 'ps2xRecomp/ps2_recomp.exe')
Write-Output 'Host tool only; generating the boot executable does not generate the complete game.'
