# Local source bundle — colosia nx v47

This private ZIP extends the public source snapshot with the complete local generated game C++ source (9,077 `.cpp` files and three headers), the MIT-licensed sse2neon header and a build entry point. No ISO, original game assets, prebuilt game archive, NRO or SDK binaries are bundled. Keep this private generated material out of GitHub; the public README describes the public repository, which omits these local inputs.

## Build on this PC

Extract the entire ZIP to a normal writable directory. Open PowerShell in the extracted `colosia-nx` folder and run:

```powershell
./BUILD-NRO.ps1
```

The script uses the devkitPro SDK already installed at `C:/devkitPro`. It prefers this PC's verified Windows CMake/Ninja paths in `local-tools.json`, falling back to `PATH` when those files are absent. An invalid `DEVKITPRO` environment path is ignored in favor of the saved Windows SDK path. It configures and compiles the complete native game AOT archive, builds the Switch runtime, compiles the five deko3d shaders and packages the NRO. It does not change clock settings or deploy to the Switch.

Output: **`build/package/celosia_switch_game.nro`**.

The full first build is substantial; two compiler jobs are used by default. Use `./BUILD-NRO.ps1 -Jobs 1` for lower memory usage. Later builds reuse CMake/Ninja output. Do not move an existing `build` directory between extracted locations; start from a fresh extraction when relocating.

To check all dependency paths and configure without compiling:

```powershell
./BUILD-NRO.ps1 -ConfigureOnly
```

For another Windows PC, install compatible devkitA64/libnx/deko3d/zlib and Switch SDK tools separately, plus CMake and Ninja, then provide their actual paths:

```powershell
./BUILD-NRO.ps1 -DevkitPro 'D:/devkitPro' -CMake 'C:/tools/cmake/bin/cmake.exe' -Ninja 'C:/tools/ninja.exe'
```

`-Diagnostics` selects `libdeko3dd.a` instead of the normal deko3d library. The default is the normal library. A complete SDK distribution is not embedded in this source ZIP.

## Running the result

This is the existing development port, with the performance and compatibility limitations described in `README.md`; it is not a new completed 720p/60 build. Preserve your working forwarder and private SD game-data installation under `sdmc:/switch/celosia-native-boot`. Building the NRO does not supply the original game data required to play. This bundle does not modify saves or install anything on the Switch.

The generated input and source were copied from the current local project. Dependency checks and both CMake configurations are verified before packaging; a fresh full AOT compilation is not claimed as part of creating this ZIP. The source snapshot's 14 C++ host tests passed; three pending Python FTP-retry tests remain documented in the public README.
