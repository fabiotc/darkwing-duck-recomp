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

F11 toggles fullscreen, F12 saves a screenshot, Esc quits.

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
| `config/` | per-game config and recorded entry points |

## Legal

This repository contains no copyrighted game code or assets. Darkwing Duck is © Disney;
the NES game is © Capcom.
