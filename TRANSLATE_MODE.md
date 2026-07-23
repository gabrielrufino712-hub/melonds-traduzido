# Translate Mode (custom feature) — live on-screen text

A tool under **System → Translate Mode** that reads the text a DS game is drawing
**right now** and lets you translate it.

## Why it reads tiles, not RAM

DS games don't keep their on-screen text as normal Shift-JIS/Unicode in memory —
they draw it from a font as background **tiles**. So instead of scanning raw RAM
(which is mostly noise), Translate Mode walks the **background tilemaps** of both
2D engines every ~0.3s and turns the visible tile rows into text lines. You only
ever see what is on screen at this moment, in real time.

## Two tables

The window has two tables:

* **Top screen (tela de cima)**
* **Bottom screen (tela de baixo)**

Each line shows its layer (BG0-3 or **OBJ** for sprite text), its row, the
on-screen text and an editable **Translation** column. Both background tiles and
**sprite (OBJ) text** are read.

## Building the table automatically (Teach)

Instead of writing the `.tbl` by hand, use **Teach reading**: select a line, click
it and type exactly what that line says. The tool maps each tile to the character
you typed (tile = char) and instantly decodes every matching line. **Save table...**
writes the `.tbl` so you can reuse it; **Load table...** loads one back.

## Direct translation editing (live)

Type your translation in the **Translation** column, then **Apply to screen
(live)** writes it back onto the running game using the table — both BG lines and
sprite lines. Static text (menus, names) stays changed; text the game redraws
every frame may revert.

## Auto-OCR (best-effort)

**Auto-OCR glyphs** tries to guess each tile's character by matching its shape to
a system font. It works reasonably for kana and latin letters, but **kanji at 8x8
is unreliable** and often stays as tile codes — treat OCR as a head start and fix
the wrong guesses with **Teach reading**. No internet needed.

## Real-time translation (online)

Tick **Auto-translate (online)** (and set the target language, e.g. `pt`) to have
decoded lines translated in real time by an online service; **Translate now** does
it once on demand. Requirements and limits: needs **internet**; only works on
lines that are already **real text** (via table / Teach / Auto-OCR), not raw tile
codes; and it uses an unofficial endpoint that may rate-limit or change.

## Seeing the actual characters

The on-screen text is drawn from the game's own font, so each line first appears
as tile codes (e.g. `15c 15d 15e`) — those numbers **are** the text, in the game's
internal font. Two ways to read them:

* **Show glyphs** (on by default) — draws each tile's real pixels next to the
  codes, so you literally SEE the kanji/kana/latin characters as images, with no
  table needed.
* **Tile table** — to get them as selectable/typable letters, map tile codes to
  characters (see below).

```
15c=A
15d=B
8140=(space)
```

Load it with **Load table...** and the lines become readable text. Tick **Show
tile codes** to switch back to raw numbers while building the table.

## Controls

* **Pause emulation** — freeze the frame to inspect/translate calmly.
* **Live (auto-refresh)** — keep the tables updating in real time (on by default).
* **Min length** — ignore tile runs shorter than this (cuts stray single tiles).
* **Inspect (click screen)** — arm it, then click a piece of text on the **bottom
  (touch)** screen; the matching line is highlighted (green) in the bottom table.
* **Guide** — shows this workflow in-app.
* **Export/Import .txt**, **Save/Load project** — translate in bulk / keep your
  work (translations are remembered per line across scene changes).

## Limits (honest)

* Only **background-layer** text is read. Text drawn as sprites (OBJ) is not
  covered.
* **Click-inspect** works on the **bottom** screen only (the top screen has no
  click coordinates on the DS).
* Turning tile text into a translated ROM means editing the game's font/tilemaps,
  which is game-specific and not automated here — this tool is the reading,
  inspecting and translation-drafting stage.

## Files

Added: `src/frontend/qt_sdl/TranslateWindow.{h,cpp}`.
Changed: `Window.{h,cpp}` (System menu entry), `Screen.cpp` (inspect click hook),
`src/frontend/qt_sdl/CMakeLists.txt`.

## Building the .exe

Push this source to your GitHub repo — the included **Windows** GitHub Actions
workflow builds `melonDS.exe` and offers it as the `melonDS-windows-x86_64`
artifact. (Local Windows: `cmake --preset release-windows-x86_64` then
`cmake --build --preset release-windows-x86_64`.)
