# celosia nx

Native Nintendo Switch port of **Shadow of the Colossus** (PS2, NTSC-U), recompiled from the PS2 executable and drawn
with deko3d.

## Thanks to the original Celosia team

celosia nx exists because of the work of the **[Celosia team](https://henrykun55.github.io/celosia/)**. Their
recompilation of Shadow of the Colossus is the base this port is built on, and its name is a tribute to theirs. Thank
you for making this possible.

## The in-game menu

Open it at any time by pressing **− and +** together. Every option is in English, Spanish and Brazilian Portuguese.
Settings are saved to the SD card when the menu closes and kept across restarts.

### Graphics

- **Preset**: sets every graphics option at once.
  - *Performance*: PS2 resolution (720p docked) with the extras off. Helps reach 60 FPS.
  - *Balanced*: 540p (900p docked), hard shadows, the sliders at their minimum. For 45 FPS.
  - *Quality*: maximum resolution, the other options at their middle, detail distance 1x. For 30 FPS.
  - *PS3 quality*: everything at its maximum plus the Panda Venom HD textures. For 30 FPS.
  - *Custom*: your own settings.
- **Frame cap**: 30, 45 or 60 real frames per second.
- **Frame generation (LSFG)**: Lossless Scaling frame generation, using your own copy of `Lossless.dll`.
  - *60 FPS (30 x2)* or *90 FPS (45 x2)*. When the game falls below the target rate, it doubles the real FPS.
  - 90 FPS only works if the display is set up for a 90 Hz output.
- **Advanced graphics**: shows or hides the options below. The presets set them for you.
  - **Resolution (handheld)**: Original (the PS2's 448 lines), 540p or 720p. Changes at once.
  - **Resolution (docked)**: 720p, 900p or 1080p. Above 720p the game is drawn sharper and scaled to the output.
  - **Dynamic resolution**: lowers the resolution only while the GPU is above 87 %.
    - The lowest it goes depends on the mode: Quality 80 %, Balanced 70 %, Performance 60 %.
  - **Upscaling filter**: bilinear or bicubic (sharper). Used when the picture is drawn below the output resolution.
  - **Anti-aliasing**: FXAA, MSAA 2x, MSAA 4x or MSAA 4x + FXAA.
    - FXAA is light.
    - MSAA costs more GPU, especially at high resolutions.
  - **Sharpening**: low, medium or high. Adaptive sharpening of the final picture, without halos.
  - **Frame generation quality**: performance, balanced or quality. Sets the resolution of the motion estimate: higher
    looks better and costs more GPU.
  - **Frame generation performance mode**: Lossless Scaling's lighter network. Less GPU, slightly less precise. On by
    default.
  - **Anisotropic filtering**: sharper ground and walls seen at an angle. Off is as on PS2.
  - **Texture detail**: high or maximum. Sharper textures in the distance (finer mipmaps). May shimmer a little more.
  - **Detail distance**: models keep their full detail farther away. 1x is as on PS2. Costs more GPU and CPU.
  - **HD textures (Panda Venom)**: loads the Panda Venom pack from `sd:/switch/celosia_nx/textures/pandavenom`. Uses
    more memory.
  - **Sky bloom**: the glow of the sky and the distance (PS2 effect).
  - **Motion blur**: the camera's blur when it turns fast (PS2 effect). The scenes' own effects stay.
  - **Heat haze and water ripple**: the air's shimmer in the fights and the ripple under water (PS2 effect).
  - **Glow**: the soft glow of the sigils, the lights and the fallen colossi (PS2 effect).
  - **Depth of field**: the blur of what lies far away, in the scenes that use it (PS2 effect).
  - **Weak points as on PCSX2**: the colossi's weak points glow at the brightness PCSX2 shows.
  - **Water reflections**: the world mirrored on the water, rippling (PS2 effect).
  - **Water refraction**: what lies under the water, seen through it (PS2 effect).
  - **Shadows**: soft (as on PS2, costs more GPU) or hard.
  - **Character shadows**: Wander, Agro and the colossi on the ground.
  - **Sky and background fog**: the PS2's background image behind the fog.
  - **Reduce colour banding**: a very fine dither for smooth gradients in the sky and the fog.
  - **Skip empty additive light**: same final colour, saves GPU on glows.

### Cheats

- **Infinite health**: Wander's health stays full.
- **Infinite grip**: the grip gauge never runs out.
- **All items**: every item unlocked and equipped (masks, cloaks, arrows, maps).
- **Story skip to colossus 15**: with it on, press **R1 + Down** in the game. The story jumps ahead and colossus 15
  becomes the next one; the earlier colossi count as beaten.
- **Story skip to colossus 16**: the same with **R1 + Left**, for colossus 16.

### States

Full save states, kept on the SD card.

- **Slot**: picks the slot.
- **Save state**: saves the whole game to the SD. Your memory card saves are not touched.
- **Load state**: returns to the state saved in that slot.

States saved with older builds still load in newer ones.

### Menu

- **Photo mode**: freezes the game without the HUD and gives you a free camera.
  - Left stick moves, right stick looks.
  - A goes up, B goes down.
  - ZR is faster, ZL is slower.
  - L and R tilt the camera.
  - X resets the camera.
  - Y takes a photo, saved to `/switch/celosia_nx/Modo foto`.
  - \+ goes back to the game.
- **Language**: Console (the Switch's language), English, Spanish or Portuguese.
- **Switch button layout**: swaps A with B and X with Y in the game, so you confirm with A as on the Switch. The menu
  and photo mode keep their own buttons.
- **4-core boost**: moves the helper threads (models, textures, disc, audio, logs, presentation) to the fourth core.
  Only usable if your forwarder was created with 4 cores.
- **Save settings**: writes every setting to the SD now. This also happens whenever the menu closes.

## Installer

**[Open the celosia nx installer](https://lemasterxar5.github.io/celosia-nx/)**

The installer is a web page that prepares everything you need into a single ZIP. It runs entirely in your browser:
nothing is uploaded anywhere.

1. Open the page in **Chrome or Edge** on a computer. Other browsers can't save a file this large straight to disk.
2. Add the pieces you have. Each one is optional and independent:
   - **Game disc**: your Shadow of the Colossus ISO (NTSC-U, SCUS-97472). It is verified and its files are
     extracted.
   - **Frame generation**: your own `Lossless.dll` from Lossless Scaling. The LSFG shaders are prepared for the
     Switch right on the page, with the same tools the port uses.
   - **Panda Venom textures**: the folder of the extracted pack. If it contains the optional PlayStation or Xbox
     button sets, you can choose one.
3. Press **Create ZIP** and choose where to save it. The page writes the ZIP to that place; it does not appear as a
   browser download.
4. Extract the ZIP to the root of your microSD. Everything goes into `switch/celosia_nx/`. If the folder already
   exists, accept merging.
5. Launch `celosia_nx.nro` from the homebrew menu through a game (application mode), not from the album.

The port itself (`celosia_nx.nro`) is always included in the ZIP. A full ZIP with the game is about 3.7 GB.

## Performance

celosia nx never raises the clocks on its own to hide performance problems. The only clock it requests is the GPU's:
384 MHz normally, and 460.8 MHz while frame generation, the Panda Venom textures or the PS3 quality preset is on.

The CPU clock is up to you, through your overclocking tool:

| Target | CPU clock |
|---|---|
| 45 FPS | 1224 MHz |
| 60 FPS | at least 1581 MHz |

- **HD textures**: running the RAM at 1600 MHz helps when the Panda Venom textures are on.
- **Frame generation at 90 FPS (45 x2)**: raise everything to the maximum of Horizon OC's default profile.

## Credits

celosia nx is built on these projects. Thank you to all of their authors.

- **[Celosia](https://henrykun55.github.io/celosia/)**: the Shadow of the Colossus recompilation this port is based on.
- **[PS2Recomp](https://github.com/ran-j/PS2Recomp)**: the PS2 static recompiler and runtime.
- **[devkitPro](https://devkitpro.org/)**: the Switch toolchain, [libnx](https://github.com/switchbrew/libnx),
  [deko3d](https://github.com/devkitPro/deko3d) and [uam](https://github.com/devkitPro/uam), the shader compiler.
- **lsfg-deko3d** by **[NaGaa95](https://github.com/NaGaa95)**, from [Cemu-nx](https://github.com/NaGaa95/Cemu-nx): Lossless Scaling frame generation on
  deko3d.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**: shader translation for the LSFG preparation.
- **[Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/)**: the frame generation itself.
  You need your own copy.
- **Panda Venom**: the HD texture pack.
- **[xxHash](https://github.com/Cyan4973/xxHash)**: fast hashing.
- **[Emscripten](https://emscripten.org/)**: runs the LSFG preparation inside the installer page.
- **alexyt2021es**: the design idea for the installer page.
- **Kow Otani**: the Shadow of the Colossus soundtrack on the installer page.

Shadow of the Colossus is a trademark of Sony Interactive Entertainment. celosia nx is an unaffiliated fan project.
You need your own copy of the game.

Made by [lemasterxar5](https://github.com/lemasterxar5). If you have problems with the installation, contact me on
[Discord](https://discord.com/users/1108525615793045564).
