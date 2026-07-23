# Translate Mode (custom feature)

A new tool added to melonDS under **System → Translate Mode**. It is built for
extracting, logging and translating the text of a running DS game and producing
a patched (translated) `.nds` ROM.

## What it does

When you open **System → Translate Mode** a second window appears. While a game
is running it analyses the console's memory in real time and logs every unique
readable string it finds. It is a general-purpose text tool for ANY DS game.

## Encodings (works on any game)

Pick the **Encoding** in the toolbar:

* **ASCII** – Latin text.
* **Shift-JIS (JP)** – cp932 Japanese (a full table is compiled in).
* **UTF-16LE** – 2-byte little-endian Unicode, used by many DS games.
* **UTF-8**.
* **Custom table (.tbl)** – for games with their own encoding: load a Thingy-style
  table (`Load table...`, lines like `41=A` or `8140= `) and the tool decodes and
  re-encodes text with it.

Check **Japanese only** to keep just strings containing real kana/kanji (cuts the
random-byte noise on JP games).

## Where it looks

* **Scan RAM** / **Auto-scan RAM** – reads the running game's main RAM.
* **Scan ROM (full)** – reads the whole cartridge ROM directly. Text captured this
  way carries its exact ROM offset, so patching it is 100% precise.

## Finding text that isn't obvious

* **Relative search** – type a short piece of text you can see on screen; the tool
  finds where a matching single-byte sequence is stored in RAM even without a
  table. Use the results to figure out the game's character codes and build a
  `.tbl`.
* **Highlight on-screen** – when checked, the strings that just appeared in memory
  on each scan are highlighted and scrolled to, so you see what is on screen now.
* **Inspect (click screen)** – EXPERIMENTAL. Arm it, then click text on the bottom
  (touch) screen; it reads the background tiles under the cursor (both 2D engines,
  BG0/BG1) and, if a tile `.tbl` is loaded, decodes them. Useful to identify which
  layer and tiles a piece of on-screen text uses.
* **Guide** – an in-app button that shows this step-by-step workflow.

## Workflow

1. **Boot the game** and open **System → Translate Mode**.
2. Text appears automatically as the game runs (toggle **Auto-scan**, or press
   **Scan now**). Use **Min length** to cut noise and the **Encoding** selector
   to focus on Shift-JIS or ASCII.
3. **Pause emulation** (button inside the window) to freeze the game on the exact
   dialogue on screen; the log is re-scanned on pause so you see precisely what
   is being displayed at that moment. Press it again to resume.
4. Use the **Filter** box to find a specific string.
5. Double-click the **Translation** cell and type your translation.
6. **Apply translation to RAM (live preview)** writes your text straight into the
   running game's memory so you can immediately see it on screen (great for
   confirming you matched the right string).
7. **Save project…/Load project…** stores all your translations as a JSON file so
   you can continue later.
8. **Create patched ROM…** searches the loaded ROM for every original string and
   replaces it with your translation, then writes a new `translated.nds`.

### Patching notes (important for real games)

* Replacements are written **at the original byte length** – the translation is
  truncated or zero-padded to fit, so ROM offsets never shift and the game stays
  bootable.
* If a string is stored **compressed/packed** in the ROM it will not be found by
  the ROM search; the tool reports which strings were not found. For those you
  can still use **Apply translation to RAM (live preview)**, or the string has to
  be decompressed with a game-specific tool first. This is a fundamental limit of
  any generic tool – DS games have no standard text format.

## Files added / changed

Added:
* `src/frontend/qt_sdl/TranslateWindow.h`
* `src/frontend/qt_sdl/TranslateWindow.cpp`
* `src/frontend/qt_sdl/TranslateSJIS.h` (auto-generated cp932 table)

Changed:
* `src/frontend/qt_sdl/Window.h` – action + slot declarations
* `src/frontend/qt_sdl/Window.cpp` – menu item, slot, include
* `src/frontend/qt_sdl/CMakeLists.txt` – build the new source file

## Building the executable

melonDS needs Qt6, SDL2, OpenGL and several vcpkg dependencies, so it must be
compiled on a machine with those toolchains. Three easy options:

### A) Get a Windows .exe with zero setup (recommended)
This repo already contains a GitHub Actions workflow that builds a Windows
`melonDS.exe`.
1. Create a GitHub account, make a new repository and push this source to it
   (the default branch must be `master`).
2. Open the repo's **Actions** tab → the **Windows** workflow runs
   automatically → when it finishes, download the **melonDS-windows-x86_64**
   artifact. It contains the ready-to-run `melonDS.exe` with Translate Mode.

### B) Build locally on Windows
Install Visual Studio 2022 (C++), Git and CMake, then:
```
git clone <this repo>
cd melonDS
cmake --preset release-windows-x86_64
cmake --build --preset release-windows-x86_64
```
The exe ends up in `build\release-windows-x86_64\melonDS.exe`.
(vcpkg fetches Qt6/SDL2/etc. automatically the first time – it takes a while.)

### C) Build on Linux
```
sudo apt install cmake ninja-build qt6-base-dev qt6-multimedia-dev \
    libsdl2-dev libslirp-dev libarchive-dev libzstd-dev libepoxy-dev
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
