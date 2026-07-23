# Translate Mode (custom feature)

One unified tool under **System → Translate Mode** to translate a DS game's text
and produce a translated ROM.

## What it does

It reads the **real text stored in the cartridge ROM**: it parses the NDS
filesystem, finds the game's pointer-table Shift-JIS text files and shows every
string as readable, editable text (File / index / Original JP / Translation).

While the game runs it also scans main RAM and **highlights (green) the strings
the game is using right now** — the text currently loaded / on screen — and can
auto-scroll to them ("Follow"). This is how you see, in real time, which line of
the list corresponds to what is happening in the emulator.

## Controls

* **Scan ROM text** — read the loaded cartridge and list all its text.
* **Highlight active text** / **Follow** — highlight and scroll to the strings in
  use at this moment of emulation.
* **Pause emulation** — freeze to translate/inspect calmly.
* **Filter** — search by text or file.
* Type the translation in the **Translation** column.
* **Apply to RAM (live)** — write the translations of the currently-active strings
  straight into the game's RAM for an instant on-screen preview (fits the original
  length; longer text is truncated).
* **Save/Load project** — keep the work as JSON.
* **Create translated ROM...** — rebuild the text files and write a patched `.nds`.
  Untranslated strings are kept byte-for-byte; only edited ones are re-encoded.

## Notes / limits (honest)

* The patched ROM writes **in place**, so a translation must fit the space of the
  original string (Japanese is 2 bytes/char, so there is usually room). Lines that
  don't fit are listed and left untouched; a full ROM repack (to allow longer
  text) is a planned next step.
* Not yet covered: character descriptions packed inside the `.aar` (ALAR/DSTX)
  archives, and menu buttons that are graphics rather than text.

## Files

Added: `src/frontend/qt_sdl/TranslateWindow.{h,cpp}`,
`src/frontend/qt_sdl/TranslateSJIS.h` (compiled cp932 table).
Changed: `Window.{h,cpp}` (System menu entry), `src/frontend/qt_sdl/CMakeLists.txt`.

## Building the .exe

Push this source to your GitHub repo — the included **Windows** GitHub Actions
workflow builds `melonDS.exe` and offers it as the `melonDS-windows-x86_64`
artifact.
