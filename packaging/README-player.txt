Darkwing Duck Recomp
====================

A native PC/Mac/Linux version of the NES game Darkwing Duck (Capcom, 1992),
made by statically recompiling the original game code.

This download contains NO game data. You need your own copy of the game as
a ROM file (.nes), dumped from your cartridge. Supported version:
  Darkwing Duck (Europe), SHA-1 8c73e5f724ae8957570b0ea1b411e1ac5061bb24
Other versions run in a slower compatibility mode without cheats.

FIRST LAUNCH
  The game asks for your ROM: click "Choose ROM file..." or drag the file
  onto the window. It is copied into the game's data folder, so you only do
  this once.
  You can also place the ROM next to the game, named darkwing-duck.nes.

CONTROLS
  D-pad    Arrow keys / WASD          Gamepad: D-pad or left stick
  A        X or K                     Gamepad: A (south button)
  B        Z or J                     Gamepad: X or B
  Start    Enter                      Gamepad: Start
  Select   Right Shift / Backspace    Gamepad: Back / Select
  Menu     Esc or F1                  Gamepad: Select + Start together
  Save state F5, load state F9, next slot F6 (also in the menu)
  Quick skip Tab / gamepad R1 (turn it on in the menu: Convenience)
  F11 fullscreen, F12 screenshot

  Custom background: put an image named bezel.png or bezel.jpg in the data
  folder (see below) and choose Background > Custom image in the menu.

  Quick skip races through the intro, briefings and dialogue boxes
  and stops at the map or when the stage starts.

  The menu has save states (4 slots), cheats (invincible, infinite lives,
  infinite special gas, stage select), video filters, sprite flicker
  removal, slowdown removal, frame interpolation and volume. Settings are
  remembered.

macOS
  The app isn't signed with an Apple developer account, so the first time
  macOS says it "cannot be opened". Right-click (or Control-click) the app,
  choose Open, then Open again. Or: System Settings > Privacy & Security >
  "Open Anyway".

WINDOWS
  Run DarkwingDuck.exe. If SmartScreen warns about an unknown publisher,
  click "More info" > "Run anyway".

LINUX / STEAM DECK / STEAMOS
  Both the AppImage and the .tar.gz work in Steam Game Mode without any
  launch options (the AppImage switches to extract-and-run mode by itself
  when Steam doesn't allow it to mount). For the .tar.gz: extract it and
  add the "darkwing-duck" file inside as a Non-Steam Game.

  1. In Desktop Mode, make the AppImage executable
     (right-click > Properties > Permissions > "Is executable",
     or: chmod +x DarkwingDuckRecomp-*.AppImage).
  2. Put darkwing-duck.nes in the same folder as the AppImage (easiest),
     or start it once in Desktop Mode and choose the ROM.
  3. Steam > Add a Game > Add a Non-Steam Game > select the AppImage.
     It then appears in Game Mode with full controller support.
  Open the menu with Select + Start (Steam reserves the Steam button).
  If it closes right away in Game Mode: in Steam, open the game's
  Properties > Compatibility and make sure "Force the use of a specific
  Steam Play compatibility tool" is OFF (Proton is for Windows games).

TROUBLESHOOTING
  When started without a terminal, the game writes darkwing-duck.log to
  its data folder (see below). It says which video system was used and
  any errors.

Data folder (ROM copy and settings):
  Windows: %APPDATA%\darkwing-duck-recomp\darkwing-duck\
  macOS:   ~/Library/Application Support/darkwing-duck-recomp/darkwing-duck/
  Linux:   ~/.local/share/darkwing-duck-recomp/darkwing-duck/

Default background: "Pixel night city" by fridaruiz (CC0),
https://opengameart.org/content/pixel-night-city

Darkwing Duck is (c) Disney. The NES game is (c) Capcom. This project is not
affiliated with or endorsed by either.
