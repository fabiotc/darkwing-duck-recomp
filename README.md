# darkwing-duck-recomp

A **static recompilation** of *Darkwing Duck* (NES, Capcom 1992) to native C, in the
spirit of N64Recomp / XenonRecomp: the game's 6502 machine code is translated
ahead of time into C, compiled with a normal C compiler and linked against a small
runtime that provides the NES hardware (PPU, APU, MMC1 mapper, controllers) through SDL3.

> **No game data is included.** You must supply your own legally obtained dump of
> the cartridge. The build checks its hash.

> **This project is a "RE"comp. It is NOT a "DE"comp.**

## Recomp vs. decomp

Both turn an old game into something that runs natively on a modern computer, but
they get there in very different ways.

| | **Recompilation** (this project) | **Decompilation** |
|---|---|---|
| How | A tool **automatically translates** the game's machine code into C, instruction by instruction | People **rewrite the game by hand** as readable C that compiles back to the exact original binary |
| Resulting code | Correct but unreadable: `L_C123: c->a = RAM[0x30]; ...` | Human-friendly: `void player_jump(Player *p) { ... }` |
| Names & meaning | None: functions and variables are just addresses | Every function, variable and data structure is understood and named |
| Assets (graphics, music, levels) | Stay inside the ROM, which is read at runtime | Usually extracted into editable files (PNG, maps, music) |
| Effort | Days to weeks, mostly tooling | Months to years, usually a whole community |
| Modding | Hard: you patch at the machine-code level | Easy: change the C or the asset files and rebuild |
| Examples | N64Recomp (Zelda64Recomp), XenonRecomp (Unleashed Recompiled), SymphonyRecomp | Super Mario 64 (`sm64`), Ocarina of Time (`oot`), Pokémon Red (`pokered`) |

Recompilation is the fast route to a native, playable port. A decompilation, when
one exists, can complement it: recomp projects such as SymphonyRecomp use the
community decomp as a reference to name functions and write patches. Darkwing Duck
has no decompilation, so the generated code here only knows addresses.

In both approaches, the repository holds tools and code, never the game's data. Each
player supplies their own copy, and the build produces the game locally from it.

## Status

First playable version: the game boots, the intro, title, map and stages run
entirely from recompiled code, with sound and keyboard/gamepad input.

## Playing (release downloads)

Download the package for your system from the
[Releases](https://github.com/fabiotc/darkwing-duck-recomp/releases) page:

| System | File |
|---|---|
| Windows 10/11 (64-bit) | `DarkwingDuckRecomp-*-windows-x64.zip` |
| macOS 11+ (Apple Silicon & Intel) | `DarkwingDuckRecomp-*-macos-universal.zip` |
| Linux, Steam Deck, SteamOS | `DarkwingDuckRecomp-*-linux-x86_64.AppImage` |

On first launch the game asks for your ROM (file dialog or drag and drop). It checks
the ROM and keeps a copy in its data folder, so you only choose it once. A ROM named
`darkwing-duck.nes` placed next to the app or AppImage is also picked up automatically,
which is the easiest route on a Steam Deck. Each package includes a `README.txt` with
per-platform tips, such as adding the AppImage to Steam and the macOS "unidentified
developer" prompt.

Release downloads contain the recompiled program code but no game assets (graphics,
music, levels), so they don't work without your ROM.

## Building

Requirements: CMake ≥ 3.20, a C11 compiler, Python ≥ 3.11, SDL3
(`brew install cmake sdl3`; if SDL3 isn't installed, CMake downloads and builds it).

```sh
cp /path/to/your/darkwing-duck.nes rom/darkwing-duck.nes
cmake -S . -B build
cmake --build build -j
./build/darkwing-duck
```

Expected ROM: `Darkwing Duck (Europe)`, NES 2.0 header, SHA-1
`8c73e5f724ae8957570b0ea1b411e1ac5061bb24`.

### Controls

| NES | Keyboard | Gamepad |
|---|---|---|
| D-pad | Arrows / WASD | D-pad / left stick |
| A | X / K | South (A / ✕) |
| B | Z / J | West / East |
| Start | Enter | Start |
| Select | Right Shift / Backspace | Back |

**Esc** or **F1** (or the gamepad's Guide button) opens the options menu, which pauses the game.
On a gamepad, **Select + Start** opens it too. F11 toggles fullscreen, F12 saves a screenshot.

## Options menu & cheats

The menu has cheats, video and performance options, and volume. Your choices are saved
and restored on the next launch.

### Video and performance enhancements

| Option | What it does |
|---|---|
| Filter: Scale2x / Scale3x | Redraws pixel-art edges at 2x/3x resolution so diagonals look smooth |
| Filter: CRT scanlines | Imitates the scanlines of the TVs the art was designed for |
| Background (bezel) | Fills the space beside the picture (e.g. fullscreen on a 16:9/16:10 screen) with an original built-in pattern (default), your own image (`bezel.png` / `bezel.jpg` in the data folder), or classic black bars |
| Remove sprite flicker | Lifts the NES limit of 8 sprites per scanline, so busy scenes stop flickering. Game logic still sees the hardware's overflow flag, so gameplay is identical |
| Remove slowdown | Doubles the CPU time per frame (extra idle vblank scanlines) so busy scenes don't lag. Game speed and music stay the same because the APU is paused during the extra lines. Command line: `--overclock` |
| Frame interpolation (experimental) | Shows a generated frame between each pair of real ones (50 to 100 fps). It's rendered at 2x resolution so objects moving 1 px per frame can be drawn half a pixel along. Sprites are matched between frames by tile, and each scanline's scroll is interpolated. Adds half a frame of latency, and animation changes can't be interpolated. Needs a display of at least 100 Hz |

The game logic is tied to frames: each frame moves Darkwing a fixed distance. So simply
running more frames would speed the game up rather than make it smoother, which is
why interpolation generates extra frames instead.

| Cheat | What it does |
|---|---|
| Invincible | Enemies and projectiles can't hurt Darkwing (pits still do) |
| Infinite lives | Dying doesn't decrease the lives counter |
| Infinite special gas | Thunder, Heavy and Arrow gas shots cost nothing (pick some up first) |
| Stage select | Choose any of the 7 stages, including the final one. Then pick any location on the map |

### Save states

Save anywhere in 4 slots: **F5** save, **F9** load, **F6** next slot, or use the menu
(the only option on a gamepad). States are stored in the data folder and only work
with the same build of the game.

Cheats are **code patches**, not memory pokes. They're declared in
`config/darkwing_duck.toml` and compiled into the game as runtime switches:

```toml
[[cheats]]
id = "invincible"
name = "Invincible"
return = ["07:F9C2"]   # the damage routine returns immediately when enabled
```

* `return = ["BANK:ADDR"]` makes the instruction at that address act as `RTS`.
* `skip = ["BANK:ADDR"]` skips that instruction.
* `poke = [{ at = "BANK:ADDR", ram = ["0031"] }]` with `choices`/`values` stores the
  value chosen in a menu dropdown into RAM just before that instruction runs (how
  stage select works).

New cheats show up in the menu automatically after a rebuild. They can also be enabled
from the command line with `--cheat invincible`.

### Finding addresses for new cheats

Two debug options help locate game variables and the code that changes them:

* `--ram-log FILE` dumps the 2 KiB of RAM every frame, so you can compare memory
  before and after an event, like getting hit.
* `--watch ADDR` (with `--interpret`) prints every instruction that changes a RAM byte:
  ```
  watch $05B4: 80 -> 82 by 07:F9F3 (frame 4474)
  ```

Things found so far:

| What | Where |
|---|---|
| Health | HUD heart tiles `$05B2`–`$05B8`, one per quarter heart; damage routine `$F9C2` |
| Lives | digit tiles `$05E4`/`$05E6` (`$70` = "0"); decremented at `$CB15` |
| Special gas | digit tiles `$05BE`/`$05C0`; bank 6 `$8982` adds a signed amount (shots −2/−3, pickups +1/+10) |
| Stage | `$31` (0–2, then 3–5, then 6 = final); `$38` = stages cleared; `$6C` = map cursor; bank 4 `$8000` loads a stage |
| Game state | `$50` (e.g. `$06` hurt, `$11` stage clear) |

Public Game Genie codes are a good shortcut. Each one is a ROM address plus a value, and
two of them pointed at exactly the instructions found above.

## How it works

```
 ROM ──► tools/nesrecomp (Python) ──► build/generated/*.c ──┐
         • decodes 6502 from entry points                    ├─► C compiler ─► darkwing-duck
         • control-flow analysis per PRG bank                │
 runtime/ (C): PPU, APU, MMC1, dispatcher, interpreter ──────┘
```

* **Windows.** Each 16 KiB PRG bank, as seen at `$8000` or `$C000`, becomes one C
  function. Every discovered instruction is C code; branches and jumps inside the
  bank are `goto`s, and a `switch` at the top lets the runtime enter at any block start.
* **Bank switching.** After any store that could hit the mapper, the generated code
  checks whether its own bank was swapped out and returns to the dispatcher if so.
* **Stack-faithful calls.** `JSR`/`RTS` use the real 6502 stack in emulated RAM, so
  tricks like pushing addresses and `RTS`-jumping, or jump tables that pop their return
  address, behave as on hardware.
* **Interrupts and timing.** Each instruction adds its exact cycle count (including
  page-cross and branch penalties). The PPU and APU are caught up lazily on register
  access and at control-transfer "poll points", where NMI/IRQ are delivered.
* **Fallback interpreter.** Code the analysis didn't find (or code running from RAM)
  runs in an interpreter. It is *generated from the same instruction semantics* as the
  recompiler (`tools/nesrecomp/semantics.py`), so both paths are cycle-identical.
* **Coverage from traces.** `config/darkwing_duck.entries` lists the block entry
  points (`bank:address`, addresses only, no game data) the interpreter had to run.
  The recompiler uses them as extra seeds.

### Making release packages

```sh
./scripts/package-all.sh     # or package-macos.sh / package-windows.sh / package-linux.sh
```

| Package | Built with |
|---|---|
| macOS universal `.app` | Xcode command line tools (ad-hoc signed) |
| Windows x64 `.exe` | mingw-w64 cross-compiler (`brew install mingw-w64`) |
| Linux x86_64 AppImage | Docker, inside an Ubuntu 22.04 container (glibc 2.35+, including SteamOS) |

SDL3 is built from source and linked statically (`-DDWD_BUNDLED_SDL=ON`), so the packages
have no dependencies. Output goes to `dist/`, together with `SHA256SUMS.txt`. The ROM is
only read locally during the build and is never uploaded.

### Growing coverage

On exit the game prints how many blocks the interpreter ran. To recompile more of
the game, play with tracing on, then rebuild:

```sh
./build/darkwing-duck --trace config/darkwing_duck.entries
cmake --build build
```

### Verifying correctness

Recompiled and interpreted execution must produce identical frames:

```sh
./build/darkwing-duck --headless --frames 3000 --input inputs.txt --screenshot a.bmp
./build/darkwing-duck --headless --frames 3000 --input inputs.txt --screenshot b.bmp --interpret
cmp a.bmp b.bmp
```

`--input` takes lines of `<frame> <buttons>` using `ABsSUDLR` (`-` releases all).

## Layout

| Path | What |
|---|---|
| `tools/nesrecomp/` | the recompiler (opcode table, semantics, analysis, codegen) |
| `runtime/` | `nesrt`: CPU glue/dispatcher, PPU, APU, cartridge + mappers |
| `src/main.c` | SDL3 frontend |
| `src/ui.cpp` | options menu and first-launch ROM screen (Dear ImGui) |
| `src/rom_select.c` | finding, checking and remembering the player's ROM |
| `packaging/`, `scripts/` | icons, platform metadata and release packaging |
| `config/` | per-game config and recorded entry points |

## Credits

* [Dear ImGui](https://github.com/ocornut/imgui) (MIT) for the menu
* [SDL3](https://libsdl.org) (zlib) for video, audio and input
* [stb_image](https://github.com/nothings/stb) (MIT / public domain), vendored in `third_party/`, for loading custom bezel images

## Legal

This repository contains no copyrighted game code or assets. Darkwing Duck is © Disney;
the NES game is © Capcom.
