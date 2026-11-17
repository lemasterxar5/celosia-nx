# celosia nx

Experimental Nintendo Switch port of **Shadow of the Colossus**, named after and based on the **Celosia** project and the PS2Recomp sources. This repository preserves the current development progress through the **v125 sources** (v124 is the latest build measured on hardware; v125 was built but not yet measured when this snapshot was taken). It is a source snapshot, not a release or a finished game port.

The project targets **native 1280 × 720 rendering at a 60 FPS minimum** (every frame, not an average), with the CPU and GPU each under 14 ms per frame. That target has **not** been reached yet. Clocks are never raised to hide performance problems: the permitted GPU ceiling is **387 MHz**; v43–v47 measurements used **307.2 MHz**; v57 onward used **384 MHz** with the CPU at **1020 MHz** and memory at **1331 MHz** (the console's per-title clock profile, verified in the on-screen overlay). Progress comes from rewriting the heaviest CPU and GPU paths natively with the same output.

## What is included

- Project C/C++ source, native Switch platform code, deko3d shaders, build scripts, hardware measurement tools and host regression tests.
- The project's own runtime and rendering modules, including directories named `src/lib`. These are project implementation source, not bundled external libraries.
- Existing copyright notices and GPL-3.0 license texts.

**No external libraries, SDK files, prebuilt archives, NRO/ELF executables, compiled shaders, game disc images, extracted game assets, memory cards, save files or releases are included.** The private generated game translation and its compiled AOT archive are also excluded. A playable build therefore cannot be produced from this public snapshot alone.

## Architecture and native execution

The Switch rendering backend uses **deko3d directly**. Switch presentation does not go through an OpenGL, Vulkan or Metal translation layer. Geometry is submitted as native GPU draws at 1280 × 720.

Game functions use ahead-of-time translated native ARM code together with handwritten native overrides. The public source includes the runtime and the overrides, but not the private generated translation. CPU skinning is still present; GPU skinning is not implemented.

This is a **hybrid port**. Secondary PS2 runtime behavior, IOP/peripheral services and compatibility code remain. The repository contains interpreter and compatibility implementations inherited from upstream. It would be inaccurate to describe the whole project as containing no emulation or compatibility code. The current direction prioritizes native rendering and performance-critical paths while retaining necessary secondary services.

The macOS and Vita implementations supplied a reference for rendering behavior, overrides and optimization. Their feature completeness and performance claims do not describe the Switch build.

## Verified progress on physical Switch hardware

The following have been observed during development; they do not establish full-game compatibility:

- Launch through the installed homebrew forwarder, title/menu, new-game startup and the opening sequence.
- Wander movement, jumping, map interaction, outdoor scenery and Agro.
- Native controller input and a native 48 kHz stereo audio output path.
- Native 720p presentation, GPU timing and frame-rate telemetry.
- Hard character/body shadow silhouettes for Wander and Agro in the tested shrine scene.

### Recent implementation milestones

| Build / area | Progress |
| --- | --- |
| v42: character pose | Fixed the observed head/body separation. Draws now resolve the final mesh identity after skin-at-kick updates, while submitted frames retain their resource ownership. The user confirmed that the original defect no longer appeared. |
| Sky rendering | Matched the direct upstream background depth policy: painter order without depth writes/testing unless explicitly requested. The previously observed horizontal sky artifacts improved in the checked outdoor view. Whole-game sky correctness remains unverified. |
| Native shadows | Added indexed stencil-volume rendering with depth clamping, front/back wrapping stencil operations, a depth/stencil attachment and an ordered nonzero-stencil black resolve. Existing game volume geometry supplies the silhouettes. Soft shadow blur is not implemented. |
| v44: shadow shaders | Added a dedicated position-only shadow vertex shader and an empty fragment shader, removing unnecessary texture/alpha work from shadow volume draws. |
| GPU resource management | Added pooled transient vertex/index arenas and small-buffer page allocation, retained indexed draws, and tied GPU resource lifetimes to completed fences. Small buffers use 4 MiB pages with 4 KiB slots; image/code allocations remain separate. |
| Driver allocation pressure | Increased nvdrv transfer memory from 8 to 32 MiB and reduced allocation churn. Earlier nvMap/address-space failures informed the pooling work. A later run exceeded eight minutes without the same failures; this is not a long-term stability guarantee. |
| CPU/GPU scheduling | Added bounded immutable frame snapshots, an owning rendering executor and a dedicated game worker with an explicit 2 MiB stack. Core placement is used only after checking the process's allowed mask. |
| Native presentation | Removed the desktop framebuffer-readback/legacy frame-upload route from Switch native presentation. |
| Telemetry | Corrected elapsed application time, separated CPU and GPU timing, used the GPU timestamp scale of 625/384 ns, and read completed timestamp queries after their owning fences. |
| v45: opaque policy | Added conservative texture/vertex alpha metadata so proven opaque draws can use a fragment shader without discard. Unknown and translucent cases retain the generic path. This change alone did not produce a meaningful measured gain. |
| v46/v47: diagnostics | Added sampled vertex/fragment/primitive counters and opaque/generic/shadow draw counts, including alpha-mode statistics. |
| v52: depth pre-pass | Depth-only pre-pass for opaque/alpha-tested geometry, then colour with early depth tests. A/B on the same build: GPU −9.2 ms stationary, −12.3 ms moving; fragments 20.3M→12.9M per sampled frame. |
| v54: GPU segments | GPU time by draw category on sampled frames (pre-pass, backdrop, opaque, generic, alpha-fail, shadow, screen) in `hardware-status.txt`. |
| v59: sky clear | Frames with background (sky) layers clear to the fog colour, as the GL renderer does; reduced the purple cloud tint in the checked view. |
| v60: async frame submission | The game thread hands each frame's batch to the render worker without waiting (at most one frame in flight; GPU fences unchanged). The wait fell from ~11 ms to ~0.5 ms per frame. `CELOSIA_DIAG_SYNC_RENDER=1` restores the old behaviour. |
| Tools | Frame-time log (`CELOSIA_DIAG_FRAME_LOG=1`), optional LTO/PGO (not adopted: worse moving minimums), SaltyNX NX-FPS bridge (not yet working on hardware: the SaltySD port is busy). |
| v85–v89 | The game's GS bloom chain ported to deko3d; texture-cost telemetry; CD and model-hook workers on by default; an optional 30 FPS cap. |
| v98: save states | Emulator-style full save states (the whole game state, not a position) on the SD card, usable after closing and reopening the game; memory-card saves are never touched. Verified on hardware: save, close, reopen, load, keep playing. |
| v104–v116: lean shaders | The generic and opaque scene shaders replicate the whole GS feature set in one program. 256 compiled variants now hold only the paths each draw's state turns on (alpha test, background fade, mips, zero-add discard, early tests, near cull, untextured, opaque): same arithmetic, same picture. Measured per full-screen pass: 0.23 ms instead of 0.76–0.87 ms. Opening-cinematic GPU 24.3 → 20.2 ms. |
| v106–v111 | Status/benchmark files written by a libnx background thread (std::thread terminates on this target); texture and large mesh storage from pooled 4 MiB pages instead of one GPU allocation per resource; recycled strip storage in the model builder. |
| v120: one-channel masks | The bloom and soft-shadow masks hold one channel (every reader takes `.r`): the same values in a quarter of the bytes. Shadow resolve 3.6 → 1.5 ms. |
| v121: background alpha merge | A background draw's FB_ONLY alpha test in the main pass writes passing and failing pixels alike (no depth writes either half), so it is drawn once without the test, in primitive order as on the GS. Alpha-fail pass 3.1 → 1.5 ms in the opening cinematic. |
| v122: render command queue | The game thread hands the render worker typed command records in storage kept between frames, instead of one heap-allocated `std::function` per draw and two copies of every per-frame mesh. Largest gameplay gain so far (altar 45.7 → 52.4 FPS). |
| v123–v125: render thread | Draw/upload lists keep their storage between frames; only the fragment stage is rebound between shader variants; the frame's draw sort moves each draw once (key sort) instead of `stable_sort` moving ~300-byte draws n log n times (2 ms of the 4.2 ms record time at ~1000 draws). Per-window render-thread timing in the present log. |
| v117/v118 (not adopted) | Enabling the desktop/Vita offload workers on the Switch made gameplay worse (the three application cores are already busy); they stay off. |
| v47: alpha pass policy | Added conservative proofs for passing alpha comparisons and skipping an empty alpha-fail pass. Generic handling remains when the proof is insufficient. The build ran on hardware, but its benchmark was interrupted before collecting the intended complete sample. |

## Hardware performance evidence

The reported values are **native presentations per second**, calculated as the sum of presented frames divided by the sum of complete measurement intervals. Each window is approximately five seconds. These are not 1% low figures, and they are not a claim that the entire game runs at the same speed.

All rows below used **1280 × 720**, hard shadows enabled, **CPU 1020 MHz / GPU 307.2 MHz / memory 1331.2 MHz**, in handheld operation mode. No overclock was applied by the measurement tools.

| Build | Scene / action | Complete windows | Frames / elapsed seconds | FPS |
| --- | --- | ---: | ---: | ---: |
| v43 | Exterior, stationary | 6 | 469 / 30.22346 | 15.52 |
| v43 | Exterior, camera orbit | 3 | 231 / 15.10078 | 15.30 |
| v43 | Running and jumping | 4 | 394 / 20.13279 | 19.57 |
| v43 | Initial shrine, stationary | 7 | 595 / 35.06727 | 16.97 |
| v44 | Same shrine view | 7 | 623 / 35.03565 | 17.78 |
| v45 | Same shrine view | 7 | 623 / 35.10319 | 17.75 |

The matched shrine comparison showed approximately **4.8%** more presentations per second from v43 to v44. Idle animation still changes between runs. v45 was effectively unchanged within these observations.

**v47 preliminary observation, not a completed benchmark:** one accepted window contained 124 presentations in 5.04014 seconds, or **24.60 FPS**. The collection attempt then failed on a transient FTP `450` response while a status file was being written. One window is insufficient to establish a repeatable improvement; no completed v47 result is claimed. A later status sample reported about 40.7 ms of GPU time and roughly 20.3 million fragment invocations per sampled frame in that shrine view, indicating substantial GPU workload remains.

### v57–v60 (shrine route, 1280 × 720, CPU 1020 / GPU 384 / memory 1331 MHz)

Static view and scripted movement; "window min" is the slowest ~5 s window, plus worst frame and 1% low.

| Build | Static: window min / avg / worst / 1% low | Moving: window min / avg / worst / 1% low |
| --- | --- | --- |
| v57 | 27.27 / 27.70 / 64.0 ms / 17.45 | 25.04 / 25.09 / 89.1 ms / 14.24 |
| v59 | 26.39 / 26.82 / 101.9 ms / 13.07 | 24.95 / 26.34 / 102.0 ms / 10.81 |
| v60 | 33.87 / 34.31 / 53.0 ms / 20.57 | 30.87 / 35.26 / 112.3 ms / 11.41 |

At 384 MHz the GPU needs ~21.8 ms per static frame. After v60 the game thread spends ~19.5 ms in game code and ~9 ms in render hooks per frame. Both limits, and 45–65 ms spikes, are above the 16.7 ms needed for 60 FPS.

### v116–v124 (1280 × 720, CPU 1020 / GPU 384 / memory 1331 MHz)

Gameplay route (static altar view, then scripted movement) and the opening cinematic used as a benchmark (start to title menu). "Shown at 60" is the share of cinematic frames presented within one 60 Hz refresh.

| Build | Altar avg / window min FPS | Moving avg / window min FPS | Gameplay GPU (altar / moving) | Cinematic GPU avg | Cinematic frames shown at 60 |
| --- | --- | --- | --- | --- | --- |
| v116 | 44.1 / 43.9 | 37.9 / 29.0 | 17.0 / 18.8 ms | 20.2 ms | 45.4 % |
| v120 | 45.4 / 44.3 | 42.6 / 35.7 | 13.1 / 12.1 ms | 17.8 ms | 49.9 % |
| v121 | 45.7 / 45.3 | 40.9 / 37.1 | 13.1 / 14.5 ms | 15.2 ms | 63.2 % |
| v122 | 52.4 / 51.9 | 47.6 / 40.7 | 13.1 / 13.9 ms | 15.1 ms | 68.5 % |
| v124 | 53.6 / 52.6 | 52.0 / 48.2 | 13.1 / 11.9 ms | 15.3 ms | 68.1 % |

Gameplay GPU time is now under the 14 ms goal; gameplay is limited by the CPU (the game thread and the render thread's command recording). Loading spikes at scene changes (100–150 ms in the cinematic) are still present.

## Current limitations and next work

- **720p/60 FPS minimum is still a goal.** v124 averages ~52–54 FPS in the tested gameplay route; the cinematic presents ~68 % of its frames at 60. Next: the render thread's recording and draw handling, the heaviest game-thread functions (scripts and AI are left as they are unless the rewrite is provably identical), the cinematic's haze layers and loading spikes; then retraining PGO on the final code.
- The SaltyNX/Status Monitor FPS counter is not yet working.
- Reduce GPU fragment work, redundant generic/alpha passes and overdraw while preserving visibility and shadows.
- Move appropriate skinning work to the GPU; current mesh skinning still consumes CPU time.
- Validate sky behavior, shadows, transparency and resource reuse across more areas, camera positions and long sessions.
- Complete a repeatable v47 benchmark. The collector currently records an incomplete attempt when the transient FTP status-file error occurs; bounded retry still needs implementation.
- Several advanced upstream effects, including parts of post-processing/reflection/refraction, are absent or disabled on this target (soft shadows and the GS bloom are implemented).
- The Switch target does not provide the upstream FFmpeg decoder (`PS2X_HAS_FFMPEG=0`). Do not assume complete movie compatibility from the tested startup sequence.
- Colossus encounters, full progression, all saves and whole-game stability have not been comprehensively validated.

## Repository layout

| Path | Contents |
| --- | --- |
| `PS2Recomp/ps2xRuntime/platform/switch/` | Native platform services, input/audio, worker infrastructure and host test stubs. |
| `PS2Recomp/ps2xRuntime/src/lib/native/deko3d/` | deko3d renderer, allocation, shaders, draw policies and regression tests. |
| `PS2Recomp/ps2xRuntime/src/lib/native/` | Shared native rendering interface and upstream rendering references. |
| `PS2Recomp/ps2xIOP/` | Secondary IOP/peripheral runtime source. |
| `PS2Recomp/ps2xRecomp/` | Recompiler source retained from the supplied project. |
| `game/overrides/` | Game-specific native overrides and reconstructed routines. |
| `tools/switch/hybrid-runtime/` | Switch runtime CMake target. |
| `tools/switch/hybrid-tests/` | Host-side regression test target. |
| `tools/switch/hardware/` | PC-side control, wireless transfer and benchmark tooling. Supply your own local hardware configuration; none is published. |

This is a scoped source snapshot. Upstream analyzer/studio tools, their generated symbol database, platform packaging, documentation media and external dependencies are omitted. The retained upstream top-level build files describe broader configurations and are not a standalone complete upstream checkout. Use the Switch/host targets below, and obtain upstream tooling separately if needed.

## Building and validation

### Host regression tests

Requires CMake 3.21+, a C++20 compiler and host threading support:

```sh
cmake -S tools/switch/hybrid-tests -B build/host-tests
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
python -m unittest discover -s tools/switch/hardware -p 'test_*.py'
```

These host tests were last run on the v60 sources and have not been re-run for this snapshot. Validation of the v60 sources passed **17/17 host CTest targets** and **32/32 Python tests** (`tools/switch/hardware`). The native executor test (asynchronous post/drain included) is built separately from `PS2Recomp/ps2xRuntime/platform/switch/tests/native_executor_test.cpp` and passed 1,070 checks (1,076 with `CELOSIA_EXECUTOR_TEST_WAIT_FAILURE`).

The alpha-policy regression executable evaluated **17,044,151 checks**, including comparison against shader float behavior. These tests cover selected policies, ownership, timing and tools; no project-wide coverage percentage or whole-game correctness is claimed.

The AArch64 renderer/runtime sources were compiled and linked for v60 and run on hardware, and the deko3d shaders were compiled using `uam`. The source-only public package has not been independently reproduced into a playable NRO without the private game inputs.

### Nintendo Switch prerequisites

Install external dependencies separately: **devkitA64/devkitPro, libnx, deko3d, `uam`, zlib, CMake and the appropriate build backend**. The hybrid target also expects **sse2neon v1.9.1** outside this repository. None of these libraries or SDKs are bundled here.

The hybrid CMake target exposes these inputs:

- `CELOSIA_DEVKITPRO`: SDK installation root.
- `CELOSIA_SSE2NEON_DIR`: external sse2neon checkout.
- `CELOSIA_HYBRID_GAME_ARCHIVE`: privately generated native AOT game archive.
- `CELOSIA_HYBRID_GAME_HEADERS`: matching generated game headers.

Review `tools/switch/build-hybrid-game.ps1` and `tools/switch/hybrid-runtime/CMakeLists.txt` for the platform build configuration. Default paths reflect the development workspace and must be overridden for another machine. Generating the matching game translation requires your own game data and the upstream tooling; those private inputs are intentionally absent here. **This repository does not currently provide a one-command public playable build.**

Use `CELOSIA_DEKO_DIAGNOSTICS` only when a matching diagnostic deko3d SDK build is installed. Diagnostic counters are sampled and should not be treated as an exhaustive per-frame capture.

### Build the NRO (Windows / PowerShell)

Run these commands from the repository root in PowerShell, with `cmake` and Ninja on `PATH`. The existing packaging script uses Windows SDK executable names (`uam.exe`, `nacptool.exe`, `elf2nro.exe`); the commands below describe that supported host environment, not a tested Linux packaging procedure.

**Before starting:** obtain a complete, compatible private generated runner from your own SCUS-97472 game data and the upstream recompilation workflow. It must contain `ps2_recompiled_functions.h`, `register_functions.cpp` and the generated `.cpp` functions matching this runtime and its overrides. Merely extracting an ISO does not produce these files. The public snapshot omits the complete generator setup, module-capture inputs and generated output; it does not currently reproduce this private generation stage end to end. Do not substitute an unrelated generated archive or enable the separate native-only ABI for this hybrid target.

1. Set your local dependency and private input paths. Replace the example directories with real absolute paths; do not commit their contents.

   ```powershell
   $sdk = 'C:/devkitPro'
   $sse = 'C:/deps/sse2neon'          # v1.9.1; contains sse2neon.h
   $runner = 'C:/private/celosia/generated' # matching complete generated runner
   $env:DEVKITPRO = $sdk
   $compiler = "$sdk/devkitA64/bin/aarch64-none-elf-g++.exe"
   $aotBuild = "$PWD/build/aot"
   $runtimeBuild = "$PWD/build/switch"
   $package = "$PWD/build/package"
   $cross = @(
       '-G', 'Ninja',
       '-DCMAKE_SYSTEM_NAME=Generic',
       '-DCMAKE_SYSTEM_PROCESSOR=aarch64',
       '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
       "-DCMAKE_CXX_COMPILER=$compiler",
       '-DCMAKE_BUILD_TYPE=Release'
   )
   ```

2. Build the private native game archive. `CELOSIA_AOT_NATIVE_ONLY=OFF` selects the ABI used by the current hybrid game build. Start with a small parallel job count because the complete translation is large.

   ```powershell
   cmake -S tools/switch/aot -B $aotBuild @cross `
       "-DCELOSIA_RUNNER_DIR=$runner" `
       "-DCELOSIA_SSE2NEON_DIR=$sse" `
       -DCELOSIA_AOT_NATIVE_ONLY=OFF
   if ($LASTEXITCODE) { throw 'AOT configuration failed' }
   cmake --build $aotBuild --target celosia_game_aot --parallel 2
   if ($LASTEXITCODE) { throw 'AOT compilation failed' }
   ```

   The expected output is `build/aot/libcelosia_game_aot.a`. If you already have a compatible archive, skip this step and point the next configuration at it.

3. Configure the Switch runtime and its game executable. The normal SDK path uses `deko3d`; diagnostics require the separately installed `deko3dd` library. Keep relocation-audit mode disabled when producing a playable package.

   ```powershell
   cmake -S tools/switch/hybrid-runtime -B $runtimeBuild @cross `
       "-DCELOSIA_DEVKITPRO=$sdk" `
       "-DCELOSIA_SSE2NEON_DIR=$sse" `
       "-DCELOSIA_HYBRID_GAME_ARCHIVE=$aotBuild/libcelosia_game_aot.a" `
       "-DCELOSIA_HYBRID_GAME_HEADERS=$runner" `
       -DCELOSIA_DEKO_DIAGNOSTICS=OFF `
       -DCELOSIA_HYBRID_RELOCATION_AUDIT=OFF
   if ($LASTEXITCODE) { throw 'Switch configuration failed' }
   ```

4. Build and package using the supplied script. It builds `celosia_switch_game.elf`, compiles the deko3d shaders with `uam` (including the 256 lean scene-shader variants generated from `lean.frag.glsl`), generates NACP metadata, embeds the shader RomFS and invokes `elf2nro`.

   ```powershell
   ./tools/switch/build-hybrid-game.ps1 `
       -CMake (Get-Command cmake).Source `
       -BuildDirectory $runtimeBuild `
       -OutputDirectory $package `
       -DevkitPro $sdk -Jobs 2
   if (-not $?) { throw 'NRO build or packaging failed' }
   Get-FileHash "$package/celosia_switch_game.nro" -Algorithm SHA256
   ```

   The final NRO is **`build/package/celosia_switch_game.nro`**. The package directory also contains the ELF, `control.nacp`, shader staging files and `build.log`. The NRO embeds the shaders, **not the original game data**. `-PackageOnly` may be used to repackage an already successfully linked ELF; it does not compile changed C++ source. An ELF configured with relocation-audit mode is deliberately rejected by the packaging script.

5. For local execution, the current application expects its working directory at **`sdmc:/switch/celosia-native-boot`** and loads the original game ELF at **`sdmc:/switch/celosia-native-boot/assets/SCUS_974.72`**. Place the NRO at the path configured in your existing homebrew forwarder and preserve the matching private extracted game-data layout. The original game ELF alone is not sufficient to play. This snapshot does not include a complete game-data deployment package or create/install a forwarder. Use the working forwarder setup rather than assuming an applet launch has enough memory.

   The optional `tools/switch/prepare-disc.mjs` tool can inspect/extract your local disc, but its default extraction only selects recompilation inputs; it does not generate native game code or prepare a complete playable SD installation. Do not publish extracted or generated game material.

**Troubleshooting:** missing `switch.h`, `sse2neon.h` or `zlib.h` indicates an SDK/dependency path problem. Missing `ps2_recompiled_functions.h` means the private runner is absent or incorrectly selected. Undefined game functions or duplicate overrides require a compatible complete translation, not placeholder stubs. Relocation/link failures must be resolved with matching Switch compiler/SDK inputs; do not turn on audit mode to disguise a failed playable link. No clock changes are made by these build commands.

These commands are derived from the checked-in CMake targets and packaging script. Both the AOT and hybrid-runtime CMake configurations were freshly checked against the local SDK and private inputs; configuration and generation succeeded. This documentation update did not rebuild or run a new NRO. Host tests were verified for the published snapshot; a clean end-to-end playable rebuild using only public inputs remains blocked by the missing private generation stage described above.

## Credits and licensing

The project is named after and based on the supplied **Celosia** source snapshot and its macOS/Vita implementations. The supplied ZIP did not identify an exact upstream commit, so none is asserted here. See the [Celosia project progress site](https://henrykun55.github.io/celosia/) and [PS2Recomp](https://github.com/ran-j/PS2Recomp) for the upstream projects.

Existing GPL-3.0 license texts and source notices are retained in `LICENSE` and `PS2Recomp/LICENSE`. External dependencies retain their own licenses and must be obtained separately. This repository does not contain or grant rights to the original game's assets. It is an unofficial development project and is not affiliated with Sony or the original developers.
