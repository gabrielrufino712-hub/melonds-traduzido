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

// Translate Mode
// --------------
// A general-purpose text-reading / translation tool for ANY Nintendo DS game.
// It scans the console main RAM (and optionally the whole cartridge ROM) in real
// time for readable text in several encodings (ASCII, Shift-JIS/cp932, UTF-16LE,
// UTF-8, or a user-supplied custom character table .tbl), logs every unique
// string with its address and raw bytes, lets the user pause the game, edit a
// translation, preview it live in RAM, and finally bake a patched .nds ROM.
//
// For games that use their own encoding (very common on the DS) it supports:
//   * loading a custom character table (.tbl, Thingy format),
//   * a relative-search to locate on-screen text and help build such a table,
//   * exporting/importing all captured text as a plain .txt for bulk editing.

#ifndef TRANSLATEWINDOW_H
#define TRANSLATEWINDOW_H

#include <QDialog>
#include <QTimer>
#include <QString>
#include <QByteArray>
#include <QHash>
#include <QVector>
#include <QPair>
#include <vector>

#include "types.h"

class QTableWidget;
class QTableWidgetItem;
class QPushButton;
class QCheckBox;
class QComboBox;
class QSpinBox;
class QLineEdit;
class QLabel;
class EmuInstance;

namespace melonDS { class NDS; }

enum TranslateEncoding
{
    ENC_ASCII = 0,
    ENC_SJIS,
    ENC_UTF16LE,
    ENC_UTF8,
    ENC_TABLE
};

enum TranslateSource
{
    SRC_RAM = 0,
    SRC_ROM = 1
};

// One captured string.
struct TranslateEntry
{
    melonDS::u32 Address = 0;       // main-RAM address where it was first seen
    qint64       RomOffset = -1;    // offset in the cart ROM (if known), else -1
    int          Source = SRC_RAM;
    QByteArray   RawBytes;          // exact source bytes (used for search/patch)
    QString      Original;          // decoded, human-readable original text
    QString      Translation;       // user-supplied replacement (empty = untouched)
    int          Encoding = ENC_SJIS;
    bool         PatchedRAM = false;
};

// A custom character table (Thingy-style .tbl: "HEX=value" per line).
struct CharTable
{
    QHash<melonDS::u16, QString> two;                 // 2-byte code -> string
    QHash<melonDS::u8,  QString> one;                 // 1-byte code -> string
    QVector<QPair<QString, QByteArray>> encodeList;   // string -> bytes (longest first)
    bool loaded = false;

    void clear() { two.clear(); one.clear(); encodeList.clear(); loaded = false; }
    bool isLoaded() const { return loaded; }
    void buildEncodeList();
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

    // Called by the emulator screen widget when the user clicks while the
    // experimental "Inspect (click screen)" mode is armed. dsx/dsy are DS
    // pixel coordinates on the touch (bottom) screen.
    bool isInspectArmed() const { return inspectArmed; }
    void screenPick(int dsx, int dsy);

private slots:
    void onTick();
    void onScanRAM();
    void onScanROM();
    void onTogglePause();
    void onClear();
    void onFilterChanged(const QString& text);
    void onCellChanged(int row, int col);
    void onApplyToRAM();
    void onCreatePatchedROM();
    void onSaveProject();
    void onLoadProject();
    void onLoadTable();
    void onRelativeSearch();
    void onExportTxt();
    void onImportTxt();
    void onGuide();
    void onToggleInspect(bool on);

private:
    void doScan(int source);
    void scanBuffer(const melonDS::u8* buf, melonDS::u32 size, int source);
    void rebuildTable();
    void appendNewRows();
    int  addTableRow(int entryIndex);
    void applyHighlight(const QVector<int>& rows);
    void refreshPauseButton();
    void addOrUpdateEntry(melonDS::u32 addr, qint64 romOffset, int source,
                          const QByteArray& raw, const QString& text, int enc);
    int  encodeTranslation(const TranslateEntry& e, QByteArray& out);

    EmuInstance* emuInstance = nullptr;

    QTableWidget* table = nullptr;
    QPushButton*  btnPause = nullptr;
    QPushButton*  btnScanRAM = nullptr;
    QPushButton*  btnScanROM = nullptr;
    QCheckBox*    chkAuto = nullptr;
    QCheckBox*    chkJpOnly = nullptr;
    QComboBox*    cmbEncoding = nullptr;
    QSpinBox*     spnMinLen = nullptr;
    QLineEdit*    txtFilter = nullptr;
    QLineEdit*    txtRelSearch = nullptr;
    QLabel*       lblStatus = nullptr;
    QLabel*       lblTable = nullptr;
    QPushButton*  btnInspect = nullptr;
    QCheckBox*    chkHighlight = nullptr;

    bool inspectArmed = false;
    QVector<int> highlightedRows;

    QTimer* scanTimer = nullptr;

    std::vector<TranslateEntry> Entries;
    QHash<QString, int> KeyToIndex;
    int displayedEntryCount = 0;

    CharTable Table;

    bool updatingTable = false;
};

#endif // TRANSLATEWINDOW_H
