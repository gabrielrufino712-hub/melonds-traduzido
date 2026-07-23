/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "TranslateWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QPushButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QInputDialog>
#include <QFile>
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QColor>
#include <QBrush>
#include <QImage>
#include <QPixmap>
#include <QIcon>
#include <QPainter>
#include <QFont>
#include <QFontDatabase>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>
#include <QStringConverter>
#include <vector>
#include <unordered_map>
#include <cstdlib>
#include <bitset>

#include "main.h"
#include "EmuInstance.h"
#include "EmuThread.h"
#include "Window.h"
#include "Screen.h"
#include "NDS.h"
#include "GPU.h"

using namespace melonDS;

enum { COL_BG, COL_POS, COL_TEXT, COL_TRANS, COL_COUNT };

// visible DS screen = 32x24 tiles (256x192)
static const int kTileCols = 32;
static const int kTileRows = 24;

TranslateWindow* TranslateWindow::currentDlg = nullptr;

TranslateWindow::TranslateWindow(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Translate Mode - live on-screen text");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1040, 620);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();
    net = new QNetworkAccessManager(this);

    auto* root = new QVBoxLayout(this);

    // ---- toolbar ----
    auto* bar = new QHBoxLayout();

    btnPause = new QPushButton("Pause emulation", this);
    connect(btnPause, &QPushButton::clicked, this, &TranslateWindow::onTogglePause);
    bar->addWidget(btnPause);

    auto* btnRefresh = new QPushButton("Refresh now", this);
    connect(btnRefresh, &QPushButton::clicked, this, &TranslateWindow::onRefreshNow);
    bar->addWidget(btnRefresh);

    chkAuto = new QCheckBox("Live (auto-refresh)", this);
    chkAuto->setChecked(true);
    bar->addWidget(chkAuto);

    chkHex = new QCheckBox("Show tile codes", this);
    chkHex->setToolTip("Show raw tile numbers instead of decoded text.");
    bar->addWidget(chkHex);

    chkGlyph = new QCheckBox("Show glyphs", this);
    chkGlyph->setChecked(true);
    chkGlyph->setToolTip("Draw each tile's actual pixels, so you SEE the Japanese "
                         "characters (as images) even without a table.");
    bar->addWidget(chkGlyph);

    bar->addWidget(new QLabel("Min length:", this));
    spnMinRun = new QSpinBox(this);
    spnMinRun->setRange(1, 20);
    spnMinRun->setValue(2);
    bar->addWidget(spnMinRun);

    auto* btnTable = new QPushButton("Load table...", this);
    connect(btnTable, &QPushButton::clicked, this, &TranslateWindow::onLoadTable);
    bar->addWidget(btnTable);

    auto* btnSaveTable = new QPushButton("Save table...", this);
    connect(btnSaveTable, &QPushButton::clicked, this, &TranslateWindow::onSaveTable);
    bar->addWidget(btnSaveTable);

    auto* btnTeach = new QPushButton("Teach reading", this);
    btnTeach->setToolTip("Select a line, click this and type what it actually says. "
                         "It builds the tile table (tile = character) automatically.");
    connect(btnTeach, &QPushButton::clicked, this, &TranslateWindow::onTeach);
    bar->addWidget(btnTeach);

    lblTable = new QLabel("(no table)", this);
    bar->addWidget(lblTable);

    btnInspect = new QPushButton("Inspect (click screen)", this);
    btnInspect->setCheckable(true);
    btnInspect->setToolTip("Arm, then click text on the bottom (touch) screen to "
                           "highlight its line in the bottom table.");
    connect(btnInspect, &QPushButton::toggled, this, &TranslateWindow::onToggleInspect);
    bar->addWidget(btnInspect);

    auto* btnGuide = new QPushButton("Guide", this);
    connect(btnGuide, &QPushButton::clicked, this, &TranslateWindow::onGuide);
    bar->addWidget(btnGuide);

    bar->addStretch();
    root->addLayout(bar);

    // ---- second row: OCR + translation ----
    auto* bar2 = new QHBoxLayout();

    auto* btnOCR = new QPushButton("Auto-OCR glyphs", this);
    btnOCR->setToolTip("Best-effort: guess characters by matching each tile's shape to "
                       "a system font (kana/latin work best; kanji is unreliable).");
    connect(btnOCR, &QPushButton::clicked, this, &TranslateWindow::onAutoOCR);
    bar2->addWidget(btnOCR);

    chkTranslate = new QCheckBox("Auto-translate (online)", this);
    chkTranslate->setToolTip("Translate decoded lines in real time via an online service "
                             "(needs internet; only works on lines that are real text).");
    connect(chkTranslate, &QCheckBox::toggled, this, &TranslateWindow::onAutoTranslateToggled);
    bar2->addWidget(chkTranslate);

    bar2->addWidget(new QLabel("to:", this));
    txtLang = new QLineEdit("pt", this);
    txtLang->setFixedWidth(44);
    txtLang->setToolTip("Target language code, e.g. pt, en, es.");
    bar2->addWidget(txtLang);

    auto* btnTr = new QPushButton("Translate now", this);
    connect(btnTr, &QPushButton::clicked, this, &TranslateWindow::onTranslateNow);
    bar2->addWidget(btnTr);

    chkOverlay = new QCheckBox("Overlay on game", this);
    chkOverlay->setToolTip("Show the translations as a subtitle over the emulator screen.");
    bar2->addWidget(chkOverlay);

    bar2->addStretch();
    root->addLayout(bar2);

    // ---- two tables side by side ----
    auto* tables = new QHBoxLayout();

    auto makeTable = [&](const QString& title) -> QTableWidget*
    {
        auto* box = new QVBoxLayout();
        auto* lbl = new QLabel(title, this);
        lbl->setStyleSheet("font-weight: bold;");
        box->addWidget(lbl);
        auto* t = new QTableWidget(this);
        t->setColumnCount(COL_COUNT);
        t->setHorizontalHeaderLabels({ "BG", "Pos", "On-screen text", "Translation" });
        t->horizontalHeader()->setSectionResizeMode(COL_TEXT, QHeaderView::Stretch);
        t->horizontalHeader()->setSectionResizeMode(COL_TRANS, QHeaderView::Stretch);
        t->setColumnWidth(COL_BG, 36);
        t->setColumnWidth(COL_POS, 54);
        t->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->setIconSize(QSize(360, 20));
        box->addWidget(t);
        tables->addLayout(box);
        return t;
    };

    tblTop = makeTable("Top screen (tela de cima)");
    tblBottom = makeTable("Bottom screen (tela de baixo)");
    connect(tblTop, &QTableWidget::cellChanged, this, &TranslateWindow::onTopCellChanged);
    connect(tblBottom, &QTableWidget::cellChanged, this, &TranslateWindow::onBottomCellChanged);

    root->addLayout(tables);

    // ---- bottom action row ----
    auto* actions = new QHBoxLayout();
    auto* btnApply = new QPushButton("Apply to screen (live)", this);
    btnApply->setToolTip("Write your translations back onto the running game using the "
                         "tile table (needs a table). Best on static text.");
    connect(btnApply, &QPushButton::clicked, this, &TranslateWindow::onApplyLive);
    actions->addWidget(btnApply);
    auto* btnExport = new QPushButton("Export .txt", this);
    connect(btnExport, &QPushButton::clicked, this, &TranslateWindow::onExportTxt);
    actions->addWidget(btnExport);
    auto* btnImport = new QPushButton("Import .txt", this);
    connect(btnImport, &QPushButton::clicked, this, &TranslateWindow::onImportTxt);
    actions->addWidget(btnImport);
    auto* btnLoad = new QPushButton("Load project", this);
    connect(btnLoad, &QPushButton::clicked, this, &TranslateWindow::onLoadProject);
    actions->addWidget(btnLoad);
    auto* btnSave = new QPushButton("Save project", this);
    connect(btnSave, &QPushButton::clicked, this, &TranslateWindow::onSaveProject);
    actions->addWidget(btnSave);
    actions->addStretch();
    root->addLayout(actions);

    lblStatus = new QLabel("Live view. Start a game - the on-screen text appears here.", this);
    root->addWidget(lblStatus);

    refreshTimer = new QTimer(this);
    refreshTimer->setInterval(300);
    connect(refreshTimer, &QTimer::timeout, this, &TranslateWindow::onTick);
    refreshTimer->start();

    refreshPauseButton();
}

TranslateWindow::~TranslateWindow()
{
    TranslateWindow::closeDlg();
}

// ---------------------------------------------------------------------------
// tilemap reading
// ---------------------------------------------------------------------------

int TranslateWindow::readTileIndex(GPU2D& eng, bool engineA, int bg, int dsx, int dsy)
{
    u16 bgcnt = eng.BGCnt[bg];
    u32 dispcnt = eng.DispCnt;
    u32 screenBaseOfs = engineA ? (((dispcnt >> 27) & 0x7) * 0x10000u) : 0u;
    u32 mapBase = (((bgcnt >> 8) & 0x1F) * 0x800u) + screenBaseOfs;
    int scsize = (bgcnt >> 14) & 0x3;
    int mapW = (scsize == 1 || scsize == 3) ? 512 : 256;
    int mapH = (scsize == 2 || scsize == 3) ? 512 : 256;

    u8* vram = nullptr; u32 mask = 0;
    eng.GetBGVRAM(vram, mask);
    if (!vram || mask == 0) return -1;

    int mx = (dsx + eng.BGXPos[bg]) & (mapW - 1);
    int my = (dsy + eng.BGYPos[bg]) & (mapH - 1);
    int ctx = mx / 8, cty = my / 8;
    int scx = ctx >> 5, scy = cty >> 5, scIndex = 0;
    if (mapW == 512 && mapH == 512) scIndex = scx + scy * 2;
    else if (mapW == 512)           scIndex = scx;
    else if (mapH == 512)           scIndex = scy;

    u32 addr = mapBase + scIndex * 0x800u + (((cty & 31) * 32 + (ctx & 31)) * 2);
    u16 ent = (u16)(vram[addr & mask] | (vram[(addr + 1) & mask] << 8));
    return ent & 0x3FF;
}

// A tile "looks like text" if it is drawn in very few colours (font glyphs are
// mono: 1 ink colour, maybe an outline) with a sane ink density. Background art
// and gradients use many colours / are solid, so they are rejected. This is what
// separates real dialogue/HUD text from decorative graphics.
bool TranslateWindow::isTextTile(int engineNum, int kind, int bg, int tile)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return false;
    GPU2D& eng = engineNum ? nds->GPU.GPU2D_B : nds->GPU.GPU2D_A;

    u8* vram = nullptr; u32 mask = 0; bool bpp8 = false; u32 charBase = 0;
    if (kind == 0)
    {
        u16 bgcnt = eng.BGCnt[bg]; u32 d = eng.DispCnt;
        bpp8 = bgcnt & 0x80;
        charBase = ((bgcnt >> 2) & 0x3) * 0x4000u + ((engineNum == 0) ? (((d >> 24) & 0x7) * 0x10000u) : 0u);
        eng.GetBGVRAM(vram, mask);
    }
    else eng.GetOBJVRAM(vram, mask);
    if (!vram || mask == 0) return false;

    u32 tb = bpp8 ? 64u : 32u;
    u32 base = charBase + (u32)tile * tb;
    int ink = 0;
    int colors[8]; int nc = 0; // distinct non-zero colour values (capped)
    for (int p = 0; p < 64; p++)
    {
        int v;
        if (bpp8) v = vram[(base + p) & mask];
        else { u8 b = vram[(base + (p >> 1)) & mask]; v = (p & 1) ? (b >> 4) : (b & 0x0F); }
        if (v == 0) continue;
        ink++;
        bool seen = false;
        for (int i = 0; i < nc; i++) if (colors[i] == v) { seen = true; break; }
        if (!seen && nc < 8) colors[nc++] = v;
    }
    if (ink < 3) return false;                 // basically empty
    if (ink > 46) return false;                // ~72% filled -> solid/background
    if (nc > 3) return false;                  // many colours -> art, not text
    return true;
}

// which BGs are readable as text for a given engine
static bool isTextBg(u32 dispcnt, int bg)
{
    int mode = dispcnt & 0x7;
    switch (bg)
    {
    case 0: return !(dispcnt & (1 << 3));  // BG0 unless it's the 3D layer
    case 1: return true;
    case 2: return mode == 0 || mode == 1 || mode == 3;
    case 3: return mode == 0 || mode == 2 || mode == 4 || mode == 5;
    }
    return false;
}

void TranslateWindow::readScreen(GPU2D& eng, bool engineA, QVector<ScreenLine>& out)
{
    out.clear();
    if (eng.ForcedBlank) return;
    u32 dispcnt = eng.DispCnt;
    const int minRun = spnMinRun->value();

    for (int bg = 0; bg < 4; bg++)
    {
        if (!(eng.LayerEnable & (1 << bg))) continue;
        if (!isTextBg(dispcnt, bg)) continue;

        // read the visible tile grid + classify each tile as text-like or not
        int engineNum = engineA ? 0 : 1;
        std::vector<std::vector<int>> grid(kTileRows, std::vector<int>(kTileCols, 0));
        std::vector<std::vector<char>> isText(kTileRows, std::vector<char>(kTileCols, 0));
        std::unordered_map<int, char> textCache;
        for (int vy = 0; vy < kTileRows; vy++)
            for (int vx = 0; vx < kTileCols; vx++)
            {
                int t = readTileIndex(eng, engineA, bg, vx * 8, vy * 8);
                if (t < 0) t = 0;
                grid[vy][vx] = t;
                auto ci = textCache.find(t);
                if (ci == textCache.end())
                {
                    char tv = isTextTile(engineNum, 0, bg, t) ? 1 : 0;
                    textCache[t] = tv;
                    isText[vy][vx] = tv;
                }
                else isText[vy][vx] = ci->second;
            }

        // group each row into runs of consecutive text tiles only
        for (int vy = 0; vy < kTileRows; vy++)
        {
            int vx = 0;
            while (vx < kTileCols)
            {
                if (!isText[vy][vx]) { vx++; continue; }
                int s = vx;
                QVector<int> tiles;
                while (vx < kTileCols && isText[vy][vx]) { tiles.append(grid[vy][vx]); vx++; }
                // reject a run that is one tile repeated (flat fill, not text)
                bool varied = false;
                for (int q = 1; q < tiles.size(); q++) if (tiles[q] != tiles[0]) { varied = true; break; }
                if (tiles.size() >= minRun && varied)
                {
                    ScreenLine ln;
                    ln.kind = 0; ln.engineNum = engineNum;
                    ln.bg = bg; ln.ty = vy; ln.c0 = s; ln.c1 = vx - 1;
                    ln.tiles = tiles;
                    ln.sig = lineSignature(0, bg, tiles);
                    ln.text = decodeLine(tiles);
                    ln.translation = transBySig.value(ln.sig);
                    out.append(ln);
                }
            }
        }
    }
}

QString TranslateWindow::lineSignature(int kind, int bg, const QVector<int>& tiles)
{
    QString s = QString::number(kind) + "/" + QString::number(bg) + ":";
    for (int t : tiles) s += QString::number(t) + ",";
    return s;
}

QString TranslateWindow::decodeLine(const QVector<int>& tiles)
{
    if (!chkHex->isChecked() && Table.isLoaded())
    {
        QString s;
        for (int t : tiles)
        {
            auto it = Table.byCode.find((u32)t);
            s += (it != Table.byCode.end()) ? it.value() : QString(QChar(char16_t(0x00B7)));
        }
        return s;
    }
    // raw tile codes
    QString s;
    for (int t : tiles) s += QString("%1 ").arg(t, 3, 16, QChar('0'));
    return s.trimmed();
}

// Draw the actual pixels of each tile so the user SEES the glyphs (monochrome:
// ink = any non-zero pixel). Works without a table. Fully bounds-checked.
QImage TranslateWindow::renderLineImage(const ScreenLine& ln)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    int n = ln.tiles.size();
    QImage img(qMax(1, n * 8), 8, QImage::Format_ARGB32);
    img.fill(qRgba(255, 255, 255, 255));
    if (!nds || n == 0) return img;

    GPU2D& eng = ln.engineNum ? nds->GPU.GPU2D_B : nds->GPU.GPU2D_A;

    u8* vram = nullptr; u32 mask = 0;
    bool bpp8 = false;
    u32 charBase = 0;
    if (ln.kind == 0)
    {
        u16 bgcnt = eng.BGCnt[ln.bg];
        u32 dispcnt = eng.DispCnt;
        bpp8 = bgcnt & 0x80;
        charBase = ((bgcnt >> 2) & 0x3) * 0x4000u
                 + ((ln.engineNum == 0) ? (((dispcnt >> 24) & 0x7) * 0x10000u) : 0u);
        eng.GetBGVRAM(vram, mask);
    }
    else
    {
        bpp8 = false; // OBJ text is almost always 4bpp
        charBase = 0;
        eng.GetOBJVRAM(vram, mask);
    }
    if (!vram || mask == 0) return img;

    const u32 tileBytes = bpp8 ? 64u : 32u;
    for (int t = 0; t < n; t++)
    {
        u32 base = charBase + (u32)ln.tiles[t] * tileBytes;
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
            {
                int px;
                if (bpp8)
                    px = vram[(base + row * 8 + col) & mask];
                else
                {
                    u8 b = vram[(base + row * 4 + col / 2) & mask];
                    px = (col & 1) ? (b >> 4) : (b & 0x0F);
                }
                if (px != 0)
                    img.setPixel(t * 8 + col, row, qRgba(0, 0, 0, 255));
            }
    }
    return img;
}

// ---------------------------------------------------------------------------
// sprites (OBJ)
// ---------------------------------------------------------------------------

void TranslateWindow::readSprites(GPU2D& eng, int engineNum, QVector<ScreenLine>& out)
{
    (void)eng;
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return;
    const u8* oam = &nds->GPU.OAM[engineNum ? 0x400 : 0];

    struct Spr { int x, y, tile, idx; };
    std::vector<Spr> sprs;
    for (int i = 0; i < 128; i++)
    {
        const u8* e = oam + i * 8;
        u16 a0 = (u16)(e[0] | (e[1] << 8));
        u16 a1 = (u16)(e[2] | (e[3] << 8));
        u16 a2 = (u16)(e[4] | (e[5] << 8));
        bool rotscale = a0 & 0x0100;
        if (!rotscale && (a0 & 0x0200)) continue;   // hidden sprite
        int y = a0 & 0xFF;
        int x = a1 & 0x1FF; if (x & 0x100) x -= 0x200;
        if (y >= 192 || x <= -32 || x >= 256) continue;
        int tile = a2 & 0x3FF;
        if (!isTextTile(engineNum, 1, 0, tile)) continue;   // only glyph-like sprites
        sprs.push_back({ x, y, tile, i });
    }
    if (sprs.empty()) return;

    std::sort(sprs.begin(), sprs.end(), [](const Spr& a, const Spr& b)
              { return (a.y != b.y) ? a.y < b.y : a.x < b.x; });

    const int minRun = spnMinRun->value();
    size_t k = 0;
    while (k < sprs.size())
    {
        int rowY = sprs[k].y;
        QVector<int> tiles; QVector<int> oamIdx;
        int lastX = sprs[k].x - 8;
        int c0 = sprs[k].x / 8;
        while (k < sprs.size() && std::abs(sprs[k].y - rowY) <= 4 && (sprs[k].x - lastX) <= 24)
        {
            tiles.append(sprs[k].tile);
            oamIdx.append(sprs[k].idx);
            lastX = sprs[k].x;
            k++;
        }
        if (tiles.size() >= minRun)
        {
            ScreenLine ln;
            ln.kind = 1; ln.engineNum = engineNum;
            ln.ty = rowY / 8; ln.c0 = c0; ln.c1 = lastX / 8;
            ln.tiles = tiles; ln.oam = oamIdx;
            ln.sig = lineSignature(1, engineNum, tiles);
            ln.text = decodeLine(tiles);
            ln.translation = transBySig.value(ln.sig);
            out.append(ln);
        }
    }
}

// ---------------------------------------------------------------------------
// table encode + tile writing (direct/live translation editing)
// ---------------------------------------------------------------------------

void CharTable::buildEnc()
{
    enc.clear();
    for (auto it = byCode.constBegin(); it != byCode.constEnd(); ++it)
        if (!it.value().isEmpty())
            enc.append({ it.value(), it.key() });
    std::sort(enc.begin(), enc.end(),
              [](const QPair<QString, melonDS::u32>& a, const QPair<QString, melonDS::u32>& b)
              { return a.first.size() > b.first.size(); });
}

QVector<int> TranslateWindow::encodeTiles(const QString& text)
{
    QVector<int> out;
    int pos = 0;
    while (pos < text.size())
    {
        bool matched = false;
        for (const auto& p : Table.enc)
        {
            if (!p.first.isEmpty() && text.mid(pos, p.first.size()) == p.first)
            {
                out.append((int)p.second);
                pos += p.first.size();
                matched = true;
                break;
            }
        }
        if (!matched) { out.append(-1); pos++; } // unknown char
    }
    return out;
}

void TranslateWindow::writeTileBG(int engineNum, int bg, int dsx, int dsy, int tileIndex)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return;
    GPU2D& eng = engineNum ? nds->GPU.GPU2D_B : nds->GPU.GPU2D_A;

    u16 bgcnt = eng.BGCnt[bg];
    u32 dispcnt = eng.DispCnt;
    u32 screenBaseOfs = (engineNum == 0) ? (((dispcnt >> 27) & 0x7) * 0x10000u) : 0u;
    u32 mapBase = (((bgcnt >> 8) & 0x1F) * 0x800u) + screenBaseOfs;
    int scsize = (bgcnt >> 14) & 0x3;
    int mapW = (scsize == 1 || scsize == 3) ? 512 : 256;
    int mapH = (scsize == 2 || scsize == 3) ? 512 : 256;

    int mx = (dsx + eng.BGXPos[bg]) & (mapW - 1);
    int my = (dsy + eng.BGYPos[bg]) & (mapH - 1);
    int ctx = mx / 8, cty = my / 8;
    int scx = ctx >> 5, scy = cty >> 5, scIndex = 0;
    if (mapW == 512 && mapH == 512) scIndex = scx + scy * 2;
    else if (mapW == 512)           scIndex = scx;
    else if (mapH == 512)           scIndex = scy;
    u32 addr = mapBase + scIndex * 0x800u + (((cty & 31) * 32 + (ctx & 31)) * 2);

    u8* vram = nullptr; u32 mask = 0;
    eng.GetBGVRAM(vram, mask);
    if (!vram || mask == 0) return;
    u16 cur = (u16)(vram[addr & mask] | (vram[(addr + 1) & mask] << 8));
    u16 ent = (u16)((cur & 0xFC00) | (tileIndex & 0x3FF)); // keep palette/flip bits

    if (engineNum == 0) nds->GPU.WriteVRAM_ABG<u16>(addr & 0x7FFFF, ent);
    else                nds->GPU.WriteVRAM_BBG<u16>(addr & 0x1FFFF, ent);
}

// ---------------------------------------------------------------------------
// refresh
// ---------------------------------------------------------------------------

void TranslateWindow::onTick()
{
    refreshPauseButton();
    if (chkAuto->isChecked())
        refresh(false);
}

void TranslateWindow::onRefreshNow() { refresh(true); }

void TranslateWindow::refresh(bool force)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return;
    EmuThread* thread = emuInstance->getEmuThread();
    if (thread && !thread->emuIsActive()) return;

    // engine -> screen mapping (POWCNT screen swap)
    bool swap = nds->GPU.ScreenSwap;
    GPU2D& topEng = swap ? nds->GPU.GPU2D_A : nds->GPU.GPU2D_B;
    GPU2D& botEng = swap ? nds->GPU.GPU2D_B : nds->GPU.GPU2D_A;

    QVector<ScreenLine> nt, nb;
    readScreen(topEng, &topEng == &nds->GPU.GPU2D_A, nt);
    readSprites(topEng, (&topEng == &nds->GPU.GPU2D_A) ? 0 : 1, nt);
    readScreen(botEng, &botEng == &nds->GPU.GPU2D_A, nb);
    readSprites(botEng, (&botEng == &nds->GPU.GPU2D_A) ? 0 : 1, nb);

    auto sigOf = [](const QVector<ScreenLine>& v) {
        QString s; for (auto& l : v) s += l.sig + "|"; return s;
    };
    QString ts = sigOf(nt), bs = sigOf(nb);

    if (force || ts != lastTopSig)
    {
        topLines = nt; lastTopSig = ts;
        rebuildTable(tblTop, topLines);
    }
    if (force || bs != lastBotSig)
    {
        botLines = nb; lastBotSig = bs;
        highlightedRow = -1;
        rebuildTable(tblBottom, botLines);
    }

    lblStatus->setText(QString("On screen now - top: %1 line(s), bottom: %2 line(s)%3")
                       .arg(topLines.size()).arg(botLines.size())
                       .arg(Table.isLoaded() ? "" : "   (load a tile .tbl to read letters)"));

    if (chkTranslate && chkTranslate->isChecked())
        doAutoTranslate();

    // push the translation subtitle overlay onto the game screen
    if (chkOverlay)
    {
        QString ov;
        if (chkOverlay->isChecked())
        {
            // the OSD font is ASCII-only, so approximate accents (tradução -> traducao)
            auto ascii = [](QString s)
            {
                s = s.normalized(QString::NormalizationForm_KD);
                QString o;
                for (QChar c : s)
                {
                    if (c.category() == QChar::Mark_NonSpacing) continue;
                    ushort u = c.unicode();
                    o += (u >= 0x20 && u <= 0x7E) ? c : QChar('?');
                }
                return o;
            };
            int lines = 0;
            auto add = [&](const QVector<ScreenLine>& v)
            {
                for (const auto& l : v)
                    if (!l.translation.isEmpty() && lines < 4)
                    { ov += (ov.isEmpty() ? QString() : QString(" / ")) + ascii(l.translation); lines++; }
            };
            add(topLines); add(botLines);
        }
        MainWindow* mw = emuInstance ? emuInstance->getMainWindow() : nullptr;
        if (mw && mw->panel) mw->panel->setTransOverlay(ov);
    }
}

void TranslateWindow::rebuildTable(QTableWidget* tbl, const QVector<ScreenLine>& lines)
{
    updatingTable = true;
    tbl->setRowCount(lines.size());
    for (int r = 0; r < lines.size(); r++)
    {
        const ScreenLine& e = lines[r];
        auto setCell = [&](int col, const QString& txt, bool editable)
        {
            QTableWidgetItem* it = new QTableWidgetItem(txt);
            it->setData(Qt::UserRole, r);
            if (editable) it->setFlags(it->flags() | Qt::ItemIsEditable);
            else          it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            tbl->setItem(r, col, it);
        };
        setCell(COL_BG, e.kind == 1 ? QString("OBJ") : QString("BG%1").arg(e.bg), false);
        setCell(COL_POS, QString("r%1").arg(e.ty), false);
        setCell(COL_TEXT, e.text, false);
        setCell(COL_TRANS, e.translation, true);

        if (chkGlyph && chkGlyph->isChecked())
        {
            QImage img = renderLineImage(e);
            QPixmap pm = QPixmap::fromImage(img.scaled(img.width() * 2, img.height() * 2));
            if (auto* it = tbl->item(r, COL_TEXT))
                it->setData(Qt::DecorationRole, pm);
        }
    }
    updatingTable = false;
}

// ---------------------------------------------------------------------------
// editing / persistence
// ---------------------------------------------------------------------------

void TranslateWindow::onTopCellChanged(int row, int col)
{
    if (updatingTable || col != COL_TRANS) return;
    if (row < 0 || row >= topLines.size()) return;
    QString txt = tblTop->item(row, col)->text();
    topLines[row].translation = txt;
    transBySig[topLines[row].sig] = txt;
}

void TranslateWindow::onBottomCellChanged(int row, int col)
{
    if (updatingTable || col != COL_TRANS) return;
    if (row < 0 || row >= botLines.size()) return;
    QString txt = tblBottom->item(row, col)->text();
    botLines[row].translation = txt;
    transBySig[botLines[row].sig] = txt;
}

// ---------------------------------------------------------------------------
// pause
// ---------------------------------------------------------------------------

void TranslateWindow::refreshPauseButton()
{
    EmuThread* thread = emuInstance ? emuInstance->getEmuThread() : nullptr;
    if (!thread || !thread->emuIsActive())
    {
        btnPause->setEnabled(false);
        btnPause->setText("Pause emulation");
        return;
    }
    btnPause->setEnabled(true);
    btnPause->setText(thread->emuIsRunning() ? "Pause emulation" : "Resume emulation");
}

void TranslateWindow::onTogglePause()
{
    EmuThread* thread = emuInstance ? emuInstance->getEmuThread() : nullptr;
    if (!thread || !thread->emuIsActive()) return;
    thread->emuTogglePause();
    refreshPauseButton();
    refresh(true);
}

// ---------------------------------------------------------------------------
// inspect (click screen -> highlight matching bottom line)
// ---------------------------------------------------------------------------

void TranslateWindow::onToggleInspect(bool on)
{
    inspectArmed = on;
    lblStatus->setText(on
        ? "Inspect armed: click the text on the bottom (touch) screen."
        : "Inspect off.");
}

void TranslateWindow::highlightBottomLine(int lineIndex)
{
    updatingTable = true;
    if (highlightedRow >= 0 && highlightedRow < tblBottom->rowCount())
        for (int c = 0; c < tblBottom->columnCount(); c++)
            if (auto* it = tblBottom->item(highlightedRow, c)) it->setBackground(QBrush());
    highlightedRow = lineIndex;
    if (lineIndex >= 0 && lineIndex < tblBottom->rowCount())
    {
        QColor hi(120, 220, 120);
        for (int c = 0; c < tblBottom->columnCount(); c++)
            if (auto* it = tblBottom->item(lineIndex, c)) it->setBackground(hi);
        if (auto* it = tblBottom->item(lineIndex, COL_TEXT))
            tblBottom->scrollToItem(it, QAbstractItemView::PositionAtCenter);
    }
    updatingTable = false;
}

void TranslateWindow::screenPick(int dsx, int dsy)
{
    // make sure the bottom table matches the current frame
    refresh(true);

    int ptx = dsx / 8, pty = dsy / 8;
    int found = -1;
    // prefer the last (usually front-most) matching line
    for (int i = 0; i < botLines.size(); i++)
    {
        const ScreenLine& l = botLines[i];
        if (l.ty == pty && ptx >= l.c0 && ptx <= l.c1)
            found = i;
    }

    if (found < 0)
    {
        lblStatus->setText(QString("Inspect: no text line at bottom-screen tile (col %1, row %2).")
                           .arg(ptx).arg(pty));
        return;
    }
    highlightBottomLine(found);
    tblBottom->selectRow(found);
    lblStatus->setText(QString("Inspect: selected bottom line at row %1 (BG%2).")
                       .arg(pty).arg(botLines[found].bg));
}

// ---------------------------------------------------------------------------
// tile table (.tbl)
// ---------------------------------------------------------------------------

void TranslateWindow::onLoadTable()
{
    QString fn = QFileDialog::getOpenFileName(this, "Load tile/character table",
                                              "", "Table (*.tbl *.txt);;All files (*)");
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    { QMessageBox::critical(this, "Translate Mode", "Could not open the table file."); return; }

    Table.clear();
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    int n = 0;
    while (!in.atEnd())
    {
        QString line = in.readLine();
        if (line.isEmpty() || line.startsWith(';') || line.startsWith('#')) continue;
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        QString hex = line.left(eq).trimmed();
        QString val = line.mid(eq + 1);
        bool ok = false;
        u32 code = hex.toUInt(&ok, 16);
        if (ok) { Table.byCode[code] = val; n++; }
    }
    f.close();

    if (n == 0)
    { QMessageBox::warning(this, "Translate Mode", "No valid entries (expected lines like \"15c=A\")."); return; }

    Table.loaded = true;
    Table.buildEnc();
    lblTable->setText(QString("table: %1").arg(n));
    lastTopSig.clear(); lastBotSig.clear(); // force redecode
    refresh(true);
    lblStatus->setText(QString("Loaded tile table with %1 entries.").arg(n));
}

// ---------------------------------------------------------------------------
// tile table generator (teach) + save + live apply
// ---------------------------------------------------------------------------

QTableWidget* TranslateWindow::activeTable()
{
    if (tblBottom->hasFocus()) return tblBottom;
    if (tblTop->hasFocus()) return tblTop;
    if (tblBottom->currentRow() >= 0) return tblBottom;
    if (tblTop->currentRow() >= 0) return tblTop;
    return nullptr;
}

void TranslateWindow::onTeach()
{
    QTableWidget* t = activeTable();
    if (!t) { QMessageBox::information(this, "Teach reading",
        "Click a line in one of the tables first, then press Teach reading."); return; }
    int row = t->currentRow();
    QVector<ScreenLine>& lines = (t == tblTop) ? topLines : botLines;
    if (row < 0 || row >= lines.size()) return;
    const ScreenLine& ln = lines[row];

    bool ok = false;
    QString reading = QInputDialog::getText(this, "Teach reading",
        QString("This line has %1 tile(s). Type exactly what it says\n"
                "(one character per tile, in order):").arg(ln.tiles.size()),
        QLineEdit::Normal, ln.text, &ok);
    if (!ok || reading.isEmpty()) return;

    int n = qMin(reading.size(), ln.tiles.size());
    for (int i = 0; i < n; i++)
        Table.byCode[(u32)ln.tiles[i]] = QString(reading.at(i));
    Table.loaded = true;
    Table.buildEnc();
    lblTable->setText(QString("table: %1").arg(Table.byCode.size()));
    lastTopSig.clear(); lastBotSig.clear();
    refresh(true);
    lblStatus->setText(QString("Learned %1 tile(s). Save the table to keep it.").arg(n));
}

void TranslateWindow::onSaveTable()
{
    if (Table.byCode.isEmpty())
    { QMessageBox::information(this, "Save table", "The table is empty. Use \"Teach reading\" first."); return; }
    QString fn = QFileDialog::getSaveFileName(this, "Save tile table", "table.tbl",
                                              "Table (*.tbl);;Text file (*.txt)");
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    { QMessageBox::critical(this, "Translate Mode", "Could not write the file."); return; }
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    QList<u32> keys = Table.byCode.keys();
    std::sort(keys.begin(), keys.end());
    for (u32 k : keys)
        out << QString("%1=%2\n").arg(k, 0, 16).arg(Table.byCode.value(k));
    f.close();
    lblStatus->setText(QString("Saved table: %1 entries.").arg(Table.byCode.size()));
}

void TranslateWindow::onApplyLive()
{
    if (!Table.isLoaded())
    { QMessageBox::information(this, "Apply to screen",
        "Load or teach a tile table first, so translations can be turned back into tiles."); return; }

    // find a blank/space tile (for padding) from the table
    int spaceTile = -1;
    for (auto it = Table.byCode.constBegin(); it != Table.byCode.constEnd(); ++it)
        if (it.value() == " ") { spaceTile = (int)it.key(); break; }

    int applied = 0, obj = 0;
    auto applyList = [&](const QVector<ScreenLine>& lines)
    {
        for (const auto& ln : lines)
        {
            if (ln.translation.isEmpty()) continue;
            QVector<int> tiles = encodeTiles(ln.translation);
            int span = ln.c1 - ln.c0 + 1;

            if (ln.kind == 0)
            {
                for (int i = 0; i < span; i++)
                {
                    int tile = (i < tiles.size()) ? tiles[i] : spaceTile;
                    if (tile < 0) continue; // unknown char / no space: leave as-is
                    writeTileBG(ln.engineNum, ln.bg, (ln.c0 + i) * 8, ln.ty * 8, tile);
                }
                applied++;
            }
            else // OBJ: rewrite each sprite's tile index
            {
                NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
                if (!nds) continue;
                for (int i = 0; i < ln.oam.size(); i++)
                {
                    int tile = (i < tiles.size()) ? tiles[i] : spaceTile;
                    if (tile < 0) continue;
                    u32 base = (ln.engineNum ? 0x400 : 0) + ln.oam[i] * 8 + 4;
                    u16 a2 = nds->GPU.OAM[base] | (nds->GPU.OAM[base + 1] << 8);
                    a2 = (u16)((a2 & 0xFC00) | (tile & 0x3FF));
                    nds->GPU.WriteOAM<u16>((ln.engineNum ? 0x400 : 0) + ln.oam[i] * 8 + 4, a2);
                }
                obj++;
            }
        }
    };
    applyList(topLines);
    applyList(botLines);

    lblStatus->setText(QString("Applied translations to screen: %1 BG line(s), %2 sprite line(s). "
                               "Static text stays; animated text may be redrawn by the game.")
                       .arg(applied).arg(obj));
}

// ---------------------------------------------------------------------------
// automatic OCR (offline glyph matching, best-effort)
// ---------------------------------------------------------------------------

quint64 TranslateWindow::tileMask(int engineNum, int kind, int bg, int tile)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return 0;
    GPU2D& eng = engineNum ? nds->GPU.GPU2D_B : nds->GPU.GPU2D_A;

    u8* vram = nullptr; u32 mask = 0; bool bpp8 = false; u32 charBase = 0;
    if (kind == 0)
    {
        u16 bgcnt = eng.BGCnt[bg]; u32 d = eng.DispCnt;
        bpp8 = bgcnt & 0x80;
        charBase = ((bgcnt >> 2) & 0x3) * 0x4000u + ((engineNum == 0) ? (((d >> 24) & 0x7) * 0x10000u) : 0u);
        eng.GetBGVRAM(vram, mask);
    }
    else eng.GetOBJVRAM(vram, mask);
    if (!vram || mask == 0) return 0;

    u32 tb = bpp8 ? 64u : 32u;
    u32 base = charBase + (u32)tile * tb;
    quint64 bits = 0; int bit = 0;
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
        {
            int px;
            if (bpp8) px = vram[(base + row * 8 + col) & mask];
            else { u8 b = vram[(base + row * 4 + col / 2) & mask]; px = (col & 1) ? (b >> 4) : (b & 0x0F); }
            if (px != 0) bits |= (1ULL << bit);
            bit++;
        }
    return bits;
}

void TranslateWindow::buildOcrRef()
{
    if (ocrRefBuilt) return;

    QFont f;
    for (const QString& name : { QString("MS Gothic"), QString("Yu Gothic"),
                                 QString("Meiryo"), QString("Noto Sans CJK JP") })
        if (QFontDatabase::families().contains(name)) { f.setFamily(name); break; }
    f.setPixelSize(8);

    auto addChar = [&](QChar c)
    {
        QImage im(8, 8, QImage::Format_ARGB32);
        im.fill(qRgba(255, 255, 255, 255));
        QPainter p(&im);
        p.setFont(f);
        p.setPen(Qt::black);
        p.drawText(im.rect(), Qt::AlignCenter, QString(c));
        p.end();
        quint64 bits = 0; int bit = 0;
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
            {
                if (qGray(im.pixel(col, row)) < 128) bits |= (1ULL << bit);
                bit++;
            }
        ocrRef.insert(c, bits);
    };

    for (ushort c = 0x21; c <= 0x7E; c++) addChar(QChar(c));            // ASCII
    for (ushort c = 0x3041; c <= 0x3096; c++) addChar(QChar(c));        // hiragana
    for (ushort c = 0x30A1; c <= 0x30FA; c++) addChar(QChar(c));        // katakana
    for (ushort c = 0xFF10; c <= 0xFF19; c++) addChar(QChar(c));        // fullwidth digits
    ocrRefBuilt = true;
}

void TranslateWindow::onAutoOCR()
{
    buildOcrRef();

    // collect unique (engine,kind,bg,tile) from what is on screen
    struct Key { int e, k, b, t; };
    QHash<int, Key> uniq; // key = compact hash
    auto collect = [&](const QVector<ScreenLine>& lines)
    {
        for (const auto& ln : lines)
            for (int t : ln.tiles)
            {
                int h = ((ln.engineNum & 1) << 30) | ((ln.kind & 1) << 29) | ((ln.bg & 3) << 27) | (t & 0x3FF);
                if (!uniq.contains(h)) uniq.insert(h, { ln.engineNum, ln.kind, ln.bg, t });
            }
    };
    collect(topLines); collect(botLines);

    const int THRESH = 10; // max differing pixels (of 64) to accept
    int filled = 0;
    for (auto it = uniq.constBegin(); it != uniq.constEnd(); ++it)
    {
        const Key& k = it.value();
        quint64 g = tileMask(k.e, k.k, k.b, k.t);
        if (g == 0) continue; // blank tile
        int best = 65; QChar bestChar;
        for (auto r = ocrRef.constBegin(); r != ocrRef.constEnd(); ++r)
        {
            int d = (int)std::bitset<64>(g ^ r.value()).count();
            if (d < best) { best = d; bestChar = r.key(); }
        }
        if (best <= THRESH) { Table.byCode[(u32)k.t] = QString(bestChar); filled++; }
    }

    if (filled == 0)
    {
        QMessageBox::information(this, "Auto-OCR",
            "No confident matches. Game fonts often don't match a system font "
            "(especially kanji). Use \"Teach reading\" to map the tiles by hand.");
        return;
    }
    Table.loaded = true;
    Table.buildEnc();
    lblTable->setText(QString("table: %1").arg(Table.byCode.size()));
    lastTopSig.clear(); lastBotSig.clear();
    refresh(true);
    lblStatus->setText(QString("Auto-OCR guessed %1 tile(s). Fix wrong ones with Teach.").arg(filled));
}

// ---------------------------------------------------------------------------
// real-time online translation
// ---------------------------------------------------------------------------

void TranslateWindow::applyTranslationToRows(const QString& sig, const QString& translation)
{
    updatingTable = true;
    auto upd = [&](QTableWidget* tbl, QVector<ScreenLine>& lines)
    {
        for (int i = 0; i < lines.size(); i++)
            if (lines[i].sig == sig)
            {
                lines[i].translation = translation;
                if (auto* it = tbl->item(i, COL_TRANS)) it->setText(translation);
            }
    };
    upd(tblTop, topLines);
    upd(tblBottom, botLines);
    updatingTable = false;
}

void TranslateWindow::translateSig(const QString& sig, const QString& text)
{
    if (!net || text.trimmed().isEmpty()) return;
    if (requestedSigs.contains(sig)) return;
    requestedSigs.insert(sig);

    QString tl = txtLang ? txtLang->text().trimmed() : "pt";
    if (tl.isEmpty()) tl = "pt";

    QUrl url("https://translate.googleapis.com/translate_a/single");
    QUrlQuery q;
    q.addQueryItem("client", "gtx");
    q.addQueryItem("sl", "auto");
    q.addQueryItem("tl", tl);
    q.addQueryItem("dt", "t");
    q.addQueryItem("q", text);
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Mozilla/5.0 melonDS");
    QNetworkReply* rep = net->get(req);
    connect(rep, &QNetworkReply::finished, this, [this, rep, sig]()
    {
        QString out;
        if (rep->error() == QNetworkReply::NoError)
        {
            QJsonDocument doc = QJsonDocument::fromJson(rep->readAll());
            if (doc.isArray())
            {
                QJsonArray segs = doc.array().at(0).toArray();
                for (const auto& s : segs) out += s.toArray().at(0).toString();
            }
        }
        rep->deleteLater();
        if (!out.isEmpty())
        {
            transBySig[sig] = out;
            applyTranslationToRows(sig, out);
        }
        else
        {
            requestedSigs.remove(sig); // allow a later retry
        }
    });
}

void TranslateWindow::doAutoTranslate()
{
    if (!Table.isLoaded()) return; // only translate real decoded text
    auto go = [&](const QVector<ScreenLine>& lines)
    {
        for (const auto& ln : lines)
            if (ln.translation.isEmpty() && !ln.text.trimmed().isEmpty())
                translateSig(ln.sig, ln.text);
    };
    go(topLines); go(botLines);
}

void TranslateWindow::onTranslateNow()
{
    if (!Table.isLoaded())
    { QMessageBox::information(this, "Translate",
        "Translation works on decoded text. Load/teach a table or run Auto-OCR first."); return; }
    doAutoTranslate();
    lblStatus->setText("Requested online translation for the on-screen lines...");
}

void TranslateWindow::onAutoTranslateToggled(bool on)
{
    lblStatus->setText(on ? "Auto-translate on (online; decoded lines only)."
                          : "Auto-translate off.");
    if (on) doAutoTranslate();
}

// ---------------------------------------------------------------------------
// guide
// ---------------------------------------------------------------------------

void TranslateWindow::onGuide()
{
    QMessageBox::information(this, "Translate Mode - Guide",
        "Live on-screen text\n"
        "-------------------\n"
        "This shows what the DS is drawing right now, split into the top and the\n"
        "bottom screen. Only the text currently on screen is listed, in real time.\n\n"
        "The game draws text with its own font tiles, so each line is shown as tile\n"
        "codes until you provide a tile table.\n\n"
        "Steps:\n"
        "1) Reach a screen with text. Lines appear in the two tables.\n"
        "2) Pause emulation to freeze the frame and inspect calmly.\n"
        "3) Inspect (click screen): arm it, then click a piece of text on the bottom\n"
        "   (touch) screen - the matching line is highlighted in the bottom table.\n"
        "4) Build a tile table: a text file with lines like\n"
        "        15c=A\n"
        "        15d=B\n"
        "   mapping each tile code (shown in the list) to its letter. Load it with\n"
        "   \"Load table...\" and the lines become readable text.\n"
        "5) Type your translation in the Translation column; it is remembered per\n"
        "   line. Save/Load project or Export/Import .txt to work in bulk.\n\n"
        "Note: only background-layer (BG) text is read, and click-inspect works on\n"
        "the bottom (touch) screen only.");
}

// ---------------------------------------------------------------------------
// export / import / project
// ---------------------------------------------------------------------------

void TranslateWindow::onExportTxt()
{
    QString fn = QFileDialog::getSaveFileName(this, "Export text", "dump.txt", "Text file (*.txt)");
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    { QMessageBox::critical(this, "Translate Mode", "Could not write the file."); return; }
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << "# ORIGINAL<TAB>TRANSLATION (per on-screen line)\n";
    auto dump = [&](const QVector<ScreenLine>& v) {
        for (const auto& l : v) {
            QString o = l.text; o.replace('\t', ' ');
            QString t = l.translation; t.replace('\t', ' ');
            out << o << '\t' << t << '\n';
        }
    };
    dump(topLines); dump(botLines);
    f.close();
    lblStatus->setText("Exported current on-screen lines.");
}

void TranslateWindow::onImportTxt()
{
    QString fn = QFileDialog::getOpenFileName(this, "Import text", "", "Text file (*.txt)");
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    { QMessageBox::critical(this, "Translate Mode", "Could not open the file."); return; }
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    int applied = 0;
    while (!in.atEnd())
    {
        QString line = in.readLine();
        if (line.startsWith('#')) continue;
        int tab = line.indexOf('\t');
        if (tab < 0) continue;
        QString orig = line.left(tab), trans = line.mid(tab + 1);
        if (trans.isEmpty()) continue;
        auto apply = [&](QVector<ScreenLine>& v) {
            for (auto& l : v) if (l.text == orig) { l.translation = trans; transBySig[l.sig] = trans; applied++; }
        };
        apply(topLines); apply(botLines);
    }
    f.close();
    rebuildTable(tblTop, topLines);
    rebuildTable(tblBottom, botLines);
    lblStatus->setText(QString("Imported %1 translation(s).").arg(applied));
}

void TranslateWindow::onSaveProject()
{
    QString fn = QFileDialog::getSaveFileName(this, "Save project", "translation.json",
                                              "Translation project (*.json)");
    if (fn.isEmpty()) return;
    QJsonArray arr;
    for (auto it = transBySig.constBegin(); it != transBySig.constEnd(); ++it)
    {
        QJsonObject o;
        o["sig"] = it.key();
        o["translation"] = it.value();
        arr.append(o);
    }
    QJsonObject root;
    root["tool"] = "melonDS Translate Mode (tiles)";
    root["entries"] = arr;
    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly))
    { QMessageBox::critical(this, "Translate Mode", "Could not write the file."); return; }
    f.write(QJsonDocument(root).toJson());
    f.close();
    lblStatus->setText(QString("Project saved: %1 translations.").arg(transBySig.size()));
}

void TranslateWindow::onLoadProject()
{
    QString fn = QFileDialog::getOpenFileName(this, "Load project", "",
                                              "Translation project (*.json)");
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::ReadOnly))
    { QMessageBox::critical(this, "Translate Mode", "Could not open the file."); return; }
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) { QMessageBox::critical(this, "Translate Mode", "Invalid project file."); return; }
    QJsonArray arr = doc.object()["entries"].toArray();
    for (const auto& v : arr)
    {
        QJsonObject o = v.toObject();
        transBySig[o["sig"].toString()] = o["translation"].toString();
    }
    lastTopSig.clear(); lastBotSig.clear();
    refresh(true);
    lblStatus->setText(QString("Project loaded: %1 translations.").arg(transBySig.size()));
}
