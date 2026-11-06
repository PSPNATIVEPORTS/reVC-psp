# GTA Vice City for PSP (reVC PSP) — V5.0

A native port of GTA Vice City to the Sony PSP: **PSP-2000, PSP-3000, PSP Go and PSP Street (E1000)**.

Follow the project on **Instagram** and **YouTube**: **@PSPNATIVEPORTS**
It is not an emulator: the game code is compiled for the PSP's CPU and draws with the PSP's
graphics chip (the GE) through a new PSP backend for librw (`vendor/librw/src/psp`). Sound
effects, radio, cutscene music and mission speech play through the PSP's sound hardware, with
the MP3 streams decoded by its Media Engine.

**This port is based on the source code of reVC, the reverse-engineered GTA Vice City by the
re3/reVC team. Many thanks to them: without their
work this port would not exist.**

**No game files are included.** You need your own copy of GTA Vice City for PC.

## What you need

- A PSP-2000, PSP-3000, PSP Go or PSP Street (E1000) that runs homebrew (custom firmware such
  as ARK-4/ARK-5, PRO or ME).
- **PSP-1000:** not supported yet. We are collecting donations to buy a PSP-1000 to test the
  game on it; if you'd like to help, contact us on Instagram or YouTube (@PSPNATIVEPORTS).
- Your GTA Vice City PC folder (any PC release).
- From the `release/V5.0` folder of this repository:
  - `EBOOT.PBP` (the game for the PSP)
  - `pspconv.exe` (the converter that turns the PC game data into PSP data; Windows)
- About 450 MB free on the Memory Stick, or about 1.9 GB with the full sound.

## Step 1 — Make a working copy of the PC game

Copy your whole GTA Vice City folder somewhere else, for example `C:\VC-PSP`. Everything below
is done on that copy, so your PC game stays as it is.

## Step 2 — Add the reVC files

Copy everything inside the `gamefiles` folder of this repository into `C:\VC-PSP`, and say
**yes** to overwrite. These are reVC's own extra files: the menu texts (`TEXT\*.gxt`), the
button pictures and fonts (`models\*.txd`) the port uses.

## Step 3 — Convert the game data with pspconv.exe

Open a command prompt (Windows key, type `cmd`, Enter) in the folder where `pspconv.exe` is and run:

```
pspconv.exe "C:\VC-PSP" "C:\VC-PSP-out"
```

- The first folder is your working copy, the second one is created for the converted files.
- It takes a few minutes and prints every file it converts, then
  `done: N textures converted, M failed` (a few failed ones are normal).
- It writes PSP versions of `models\gta3.img`, `models\gta3.dir`, the loose texture files in
  `models\` (fonts, HUD, menus, particles, button icons, `generic\wheels.TXD`) and the loading
  screens in `txd\`. The textures are shrunk (128 px for the world), turned into
  256-colour palettes and arranged the way the PSP's GE reads them fastest.

**Options** (a third word after the two folders):

| Command | What it does |
|---|---|
| `pspconv.exe "C:\VC-PSP" "C:\VC-PSP-out"` | Normal conversion (recommended). |
| `pspconv.exe "C:\VC-PSP" "C:\VC-PSP-out" low` | World textures with 16 colours instead of 256: half the texture memory, looks almost the same. People, vehicles, weapons, menus and fonts are unchanged. |
| `pspconv.exe "C:\VC-PSP" "C:\VC-PSP-out" 64` | Smaller world textures (64 px): less memory, blurrier. `256` makes them sharper but they may not fit in memory. |

`low` also works on data that is already converted (for example the game folder on your
Memory Stick), so you can try it later without starting over.

## Step 4 — Put the game on the Memory Stick

1. Create a folder for the game in `PSP/GAME/` on the Memory Stick, for example
   `PSP/GAME/Vice City/` (PSP Go: on the internal storage or the Memory Stick Micro).
2. Copy `EBOOT.PBP` into it.
3. From `C:\VC-PSP` copy these folders into it: `anim`, `data`, `models`, `TEXT`.
4. From `C:\VC-PSP-out` copy `models` and `txd` into it and say **yes** to overwrite.
   The converted `gta3.img`, `gta3.dir` and textures must replace the PC ones.
5. Sound (optional), from `C:\VC-PSP\Audio` into an `Audio` folder next to `EBOOT.PBP`:
   - sound effects only: `SFX.RAW` and `SFX.SDT`;
   - everything (radio, cutscene music, mission speech): the whole `Audio` folder (about 1.4 GB).

   Without the `Audio` folder the game runs silent.

Do **not** copy `movies` or `mp3` (not used).

The folder should look like this:

```
PSP/GAME/Vice City/
    EBOOT.PBP
    anim/
    Audio/          (optional)
    data/
    models/         (with the converted gta3.img, gta3.dir and .txd files)
    TEXT/
    txd/            (converted)
```

Start the game from the XMB (Game > Memory Stick). Plugins (cheat devices, screen recorders,
music players) slow the game down: start it with plugins off. The game sets the CPU to 333 MHz
itself.

## Controls

The layout follows GTA Vice City Stories on the PSP. Options > Controller Setup shows a PSP
with every button, and the game's help texts name the PSP buttons.

| PSP | On foot | In a vehicle |
|---|---|---|
| Analog nub | Move | Steer (bikes: lean) |
| Cross | Sprint | Accelerate (helicopter: up) |
| Square | Jump | Brake / reverse (helicopter: down) |
| Circle | Attack / fire | Vehicle weapon, drive-by, helicopter guns |
| Triangle | Enter vehicle | Exit vehicle |
| R | Target / aim (hold) | Handbrake (helicopter: turn right) |
| R + d-pad down | Free aim (pistols, SMGs) | - |
| L | Camera behind the player | Helicopter: turn left |
| L + analog | Look around | Look left / right / behind |
| L + R | Look behind | - |
| D-pad left / right | Previous / next weapon (sniper rifle, rifles, rocket launcher: fine aim) | Radio station (tank, fire truck: turret) |
| D-pad up | - | Sub-mission (taxi, vigilante...) |
| D-pad down | Crouch | Horn / siren |
| Select | Camera mode | Camera mode |
| Start | Pause menu | Pause menu |

In the menus the d-pad moves, Cross selects, Triangle goes back (on the pause menu: back to
the game). HOME quits.

**Map and GPS:** in the pause menu map, the d-pad / nub moves the map, R zooms in, L zooms out,
Cross sets or removes a waypoint, Square shows or hides the legend. While you drive, the road
route to the waypoint is drawn in purple on the radar and the map. During a mission, without a
waypoint of your own, the GPS leads to the mission's blip.

**Cheat menu:** hold L + R and press Select (in game, not during cutscenes). Up / down chooses,
left / right changes the weapon set or the vehicle (L / R: 10 vehicles at a time), Cross applies,
Circle closes. It has health + armour, remove wanted level, three weapon sets, money and a
spawner with every car, bike and helicopter.

## Saves

- Saves go into the PSP's **Saved Data Utility** (`PSP/SAVEDATA/REVC00001SLOT1` to `SLOT8`), with
  an icon and the mission name, so you can copy, back up or delete them from the XMB.
- The game starts straight into the save you made or loaded last, like Vice City Stories.
  Hold **L** while it starts to get the main menu instead.
- **Using your PC saves:** the PC game keeps them in `Documents\GTA Vice City User Files\`
  (`GTAVCsf1.b` to `GTAVCsf8.b`). Make a folder `pc_saves` next to `EBOOT.PBP` and copy them into
  it. At the next start, `GTAVCsfN.b` goes into slot N (it replaces the PSP save in that slot) and
  is renamed `GTAVCsfN.b.imported`. Then load it from the menu.

## Settings (`psp_settings.txt`)

At its first start the game writes `psp_settings.txt` into its folder. Edit it on the PC (or
delete it to get the defaults back) and restart the game.

| Setting | Default | Meaning |
|---|---|---|
| `draw_distance` | 450 | The furthest the world is drawn, in metres. With `auto_quality` on it's the upper limit. |
| `min_draw_distance` | 400 | The shortest draw distance `auto_quality` may use. |
| `auto_quality` | 1 | Holds the frame rate: when frames get slow, detail switches to low models sooner, then the draw distance comes in, then people and cars are thinned out (never below 80%). 0 = fixed settings. |
| `lod_scale` | 0.60 | How far detailed models are kept before the low detail ones. |
| `ped_density` | 1.00 | Amount of people (0 - 2, 1 = like the PC game). |
| `car_density` | 0.90 | Amount of traffic (0 - 2). |
| `shadows` | 1 | 0 off, 1 player and cars, 2 everything. |
| `particles` | 0 | 0 half the smoke and sparks, 1 all. |
| `detail_water` | 0 | 1 = the PC's wavy water near the camera. |
| `car_reflections` | 0 | 1 = shiny reflections on cars. |
| `colour_filter` | 2 | 0 off, 1 PC, 2 VCS style, 3 PS2. |
| `motion_blur` | 1 | Trails when driving fast, like VCS. 2 = stronger, 0 = off. |
| `fps_target` | 30 | 30 or 20. |
| `fast_hud` | 1 | Builds the HUD every other frame (saves 2-3 ms per frame). |
| `audio` | 1 | 0 = no sound. |
| `music` | 1 | 0 = sound effects only (no radio, cutscene music or speech). |
| `audio_rate` | 16000 | Sound output rate: 11025, 16000 (like LCS/VCS), 22050 or 44100. |
| `sfx_cache_kb` | 3072 | Memory for sound effects. |
| `near_clip` | 1.25 | Camera near plane in metres (higher = less flicker far away). |
| `road_bias`, `decal_bias` | 6, 3 | Keep distant roads and ground details from flickering (0 = off). |
| `ground_fix` | 0 | Leave at 0. |
| `show_fps` | 1 | Timing lines in the log (only written while testing). |

If the game is slow, lower `min_draw_distance` first (300, then 200), then the densities.

## Building from source

```
# PSP toolchain: https://github.com/pspdev/pspdev
psp-cmake -S . -B build-psp -DCMAKE_BUILD_TYPE=Release
cmake --build build-psp                 # -> build-psp/src/EBOOT.PBP

# the converter (PC; for Windows build it with MinGW or Visual Studio)
cmake -S utils/pspconv -B build-pspconv
cmake --build build-pspconv --config Release
```

The PSP code is mostly in `src/skel/psp` (platform, input, saves, cheat menu, GPS),
`src/audio/sampman_psp.cpp` (sound) and `vendor/librw/src/psp` (graphics). The tuning limits
are in `src/core/config.h`, section "Sony PSP". The XMB pictures are `res/psp/ICON0.PNG` and
`res/psp/PIC1.PNG` (you can put your own there).

## Known limitations

- Textures must be converted with `pspconv.exe`: unconverted PC data loads slowly and runs out
  of memory.
- Objects pop in instead of fading in (saves a lot of CPU).
- No full-motion videos.

## Follow the project

Instagram and YouTube: **@PSPNATIVEPORTS**: videos, news and the next ports.

## Credits

- **reVC / re3 team** — the reverse-engineered GTA Vice City source this port is built on.
- **librw** by aap — the RenderWare replacement, extended here with a PSP backend.
- **pspdev** — the PSP toolchain and SDK.
- GTA Vice City © Rockstar Games. This project is not affiliated with Rockstar Games or
  Take-Two Interactive. It contains no game data; you need to own the game.
