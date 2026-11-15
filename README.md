# colosia nx

Experimental Nintendo Switch port of **Shadow of the Colossus**, based on the supplied Celosia and PS2Recomp sources. This repository preserves the current development progress through the **v47 hardware build**. It is a source snapshot, not a release or a finished game port.

The project targets **native 1280 × 720 rendering at 60 FPS with stock clocks**. That performance target has **not** been reached. The permitted GPU ceiling for this project is **387 MHz**; the hardware measurements below were taken at **307.2 MHz**, without writing clock settings.

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

## Current limitations and next work

- **720p/60 FPS is still a goal.** Current evidence is well below 60 FPS at the measured clocks.
- Reduce GPU fragment work, redundant generic/alpha passes and overdraw while preserving visibility and shadows.
- Move appropriate skinning work to the GPU; current mesh skinning still consumes CPU time.
- Validate sky behavior, shadows, transparency and resource reuse across more areas, camera positions and long sessions.
- Complete a repeatable v47 benchmark. The collector currently records an incomplete attempt when the transient FTP status-file error occurs; bounded retry still needs implementation.
- Soft shadow filtering and several advanced upstream effects, including parts of post-processing/reflection/refraction, are absent or disabled on this target.
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

The development v47 validation passed **14/14 host CTest targets** and **28 Python tests**. The alpha-policy regression executable evaluated **17,044,151 checks**, including comparison against shader float behavior. These tests cover selected policies, ownership, timing and tools; no project-wide coverage percentage or whole-game correctness is claimed.

The changed AArch64 renderer/runtime sources were compiled and linked for v47, and the five deko3d shaders were compiled using `uam`. The source-only public package has not been independently reproduced into a playable NRO without the private game inputs.

### Nintendo Switch prerequisites

Install external dependencies separately: **devkitA64/devkitPro, libnx, deko3d, `uam`, zlib, CMake and the appropriate build backend**. The hybrid target also expects **sse2neon v1.9.1** outside this repository. None of these libraries or SDKs are bundled here.

The hybrid CMake target exposes these inputs:

- `CELOSIA_DEVKITPRO`: SDK installation root.
- `CELOSIA_SSE2NEON_DIR`: external sse2neon checkout.
- `CELOSIA_HYBRID_GAME_ARCHIVE`: privately generated native AOT game archive.
- `CELOSIA_HYBRID_GAME_HEADERS`: matching generated game headers.

Review `tools/switch/build-hybrid-game.ps1` and `tools/switch/hybrid-runtime/CMakeLists.txt` for the platform build configuration. Default paths reflect the development workspace and must be overridden for another machine. Generating the matching game translation requires your own game data and the upstream tooling; those private inputs are intentionally absent here. **This repository does not currently provide a one-command public playable build.**

Use `CELOSIA_DEKO_DIAGNOSTICS` only when a matching diagnostic deko3d SDK build is installed. Diagnostic counters are sampled and should not be treated as an exhaustive per-frame capture.

## Credits and licensing

Based on the supplied **Celosia** source snapshot and its macOS/Vita implementations. The supplied ZIP did not identify an exact upstream commit, so none is asserted here. See the [Celosia project progress site](https://henrykun55.github.io/celosia/) and [PS2Recomp](https://github.com/ran-j/PS2Recomp) for the upstream projects.

Existing GPL-3.0 license texts and source notices are retained in `LICENSE` and `PS2Recomp/LICENSE`. External dependencies retain their own licenses and must be obtained separately. This repository does not contain or grant rights to the original game's assets. It is an unofficial development project and is not affiliated with Sony or the original developers.