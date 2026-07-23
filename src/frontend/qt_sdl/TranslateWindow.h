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

// Translate Mode - live on-screen text reader
// --------------------------------------------
// Instead of scanning raw RAM (which is full of noise), this reads what the DS
// is ACTUALLY drawing right now: it walks the background tilemaps of both 2D
// engines every refresh and turns the visible tile rows into text lines. The
// result is split into two tables - Top screen and Bottom screen - so you always
// see only the text that is on screen at this moment, in real time.
//
// Tiles are the game's own font glyph indices. Load a tile table (.tbl, lines
// like "15c=A") to see them as readable letters; without one the raw tile codes
// are shown so you can build the table. "Inspect (click screen)" lets you click
// text on the bottom screen and highlights the matching line in the table.

#ifndef TRANSLATEWINDOW_H
#define TRANSLATEWINDOW_H

#include <QDialog>
#include <QTimer>
#include <QString>
#include <QByteArray>
#include <QHash>
#include <QVector>
#include <QPair>
#include <QSet>

#include "types.h"

class QTableWidget;
class QTableWidgetItem;
class QPushButton;
class QCheckBox;
class QComboBox;
class QSpinBox;
class QLineEdit;
class QLabel;
class QImage;
class QNetworkAccessManager;
class EmuInstance;

namespace melonDS { class NDS; class GPU2D; }

// A run of tiles forming one on-screen text line.
struct ScreenLine
{
    int kind = 0;        // 0 = BG layer, 1 = OBJ (sprite)
    int engineNum = 0;   // 0 = engine A, 1 = engine B
    int bg = 0;
    int ty = 0;          // visible tile row
    int c0 = 0, c1 = 0;  // visible tile column range
    QVector<int> tiles;
    QVector<int> oam;    // OBJ: source sprite indices, in order
    QString sig;         // stable signature, key for translations
    QString text;        // decoded text (or hex tile codes)
    QString translation;
};

// Custom tile/character table (Thingy-style .tbl: "HEX=value" per line).
struct CharTable
{
    QHash<melonDS::u32, QString> byCode;                  // tile/char code -> string
    QVector<QPair<QString, melonDS::u32>> enc;            // string -> code (longest first)
    bool loaded = false;

    void clear() { byCode.clear(); enc.clear(); loaded = false; }
    bool isLoaded() const { return loaded; }
    void buildEnc();
};

class TranslateWindow : public QDialog
{
    Q_OBJECT

public:
    explicit TranslateWindow(QWidget* parent);
    ~TranslateWindow() override;

    static TranslateWindow* currentDlg;
    static TranslateWindow* openDlg(QWidget* parent)
    {
        if (currentDlg)
        {
            currentDlg->activateWindow();
            return currentDlg;
        }
        currentDlg = new TranslateWindow(parent);
        currentDlg->show();
        return currentDlg;
    }
    static void closeDlg() { currentDlg = nullptr; }

    // Called by the emulator screen widget when the user clicks the bottom
    // (touch) screen while "Inspect (click screen)" is armed.
    bool isInspectArmed() const { return inspectArmed; }
    void screenPick(int dsx, int dsy);

private slots:
    void onTick();
    void onRefreshNow();
    void onTogglePause();
    void onLoadTable();
    void onSaveTable();
    void onTeach();
    void onApplyLive();
    void onGuide();
    void onToggleInspect(bool on);
    void onAutoOCR();
    void onTranslateNow();
    void onAutoTranslateToggled(bool on);
    void onTopCellChanged(int row, int col);
    void onBottomCellChanged(int row, int col);
    void onSaveProject();
    void onLoadProject();
    void onExportTxt();
    void onImportTxt();

private:
    void refresh(bool force);
    void readScreen(melonDS::GPU2D& eng, bool engineA, QVector<ScreenLine>& out);
    void readSprites(melonDS::GPU2D& eng, int engineNum, QVector<ScreenLine>& out);
    int  readTileIndex(melonDS::GPU2D& eng, bool engineA, int bg, int dsx, int dsy);
    bool isTextTile(int engineNum, int kind, int bg, int tile);
    void writeTileBG(int engineNum, int bg, int dsx, int dsy, int tileIndex);
    QVector<int> encodeTiles(const QString& text);
    QString decodeLine(const QVector<int>& tiles);
    QImage renderLineImage(const ScreenLine& ln);
    QString lineSignature(int kind, int bg, const QVector<int>& tiles);
    void rebuildTable(QTableWidget* tbl, const QVector<ScreenLine>& lines);
    void refreshPauseButton();
    void highlightBottomLine(int lineIndex);
    QTableWidget* activeTable();
    quint64 tileMask(int engineNum, int kind, int bg, int tile);
    void buildOcrRef();
    void translateSig(const QString& sig, const QString& text);
    void doAutoTranslate();
    void applyTranslationToRows(const QString& sig, const QString& translation);

    EmuInstance* emuInstance = nullptr;

    QTableWidget* tblTop = nullptr;
    QTableWidget* tblBottom = nullptr;
    QPushButton*  btnPause = nullptr;
    QPushButton*  btnInspect = nullptr;
    QCheckBox*    chkAuto = nullptr;
    QCheckBox*    chkHex = nullptr;
    QCheckBox*    chkGlyph = nullptr;
    QCheckBox*    chkTranslate = nullptr;
    QCheckBox*    chkOverlay = nullptr;
    QLineEdit*    txtLang = nullptr;
    QSpinBox*     spnMinRun = nullptr;
    QLabel*       lblStatus = nullptr;
    QLabel*       lblTable = nullptr;

    QTimer* refreshTimer = nullptr;

    QVector<ScreenLine> topLines, botLines;
    QString lastTopSig, lastBotSig;
    QHash<QString, QString> transBySig;   // persists translations across refreshes

    CharTable Table;

    QNetworkAccessManager* net = nullptr;
    QHash<QChar, quint64> ocrRef;
    bool ocrRefBuilt = false;
    QSet<QString> requestedSigs;

    bool inspectArmed = false;
    int  highlightedRow = -1;
    bool updatingTable = false;
};

#endif // TRANSLATEWINDOW_H
