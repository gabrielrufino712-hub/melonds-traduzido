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
// A tool window that analyses the running ROM in real time: it scans the DS
// main RAM for readable text (ASCII and Shift-JIS/cp932), logs every unique
// string it finds together with its address and raw bytes, lets the user pause
// the emulation to inspect exactly what is on screen, type a translation for
// any captured string, preview it live in RAM, and finally bake a patched .nds
// ROM (a translated build of the game). Translations can be saved/loaded as a
// JSON project so the work survives across sessions.

#ifndef TRANSLATEWINDOW_H
#define TRANSLATEWINDOW_H

#include <QDialog>
#include <QTimer>
#include <QString>
#include <QByteArray>
#include <QSet>
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

// One captured string.
struct TranslateEntry
{
    melonDS::u32 Address = 0;       // main-RAM address where it was first seen
    QByteArray   RawBytes;          // exact source bytes (used for ROM search/patch)
    QString      Original;          // decoded, human-readable original text
    QString      Translation;       // user-supplied replacement (empty = untouched)
    int          Encoding = 0;      // 0 = ASCII, 1 = Shift-JIS
    bool         PatchedRAM = false;
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

private slots:
    void onTick();
    void onScanNow();
    void onTogglePause();
    void onClear();
    void onFilterChanged(const QString& text);
    void onCellChanged(int row, int col);
    void onApplyToRAM();
    void onCreatePatchedROM();
    void onSaveProject();
    void onLoadProject();

private:
    void doScan();
    void rebuildTable();
    void appendNewRows();
    int  addTableRow(int entryIndex);
    void refreshPauseButton();
    void addOrUpdateEntry(melonDS::u32 addr, const QByteArray& raw, const QString& text, int enc);
    void writeTranslationToRAM(TranslateEntry& e);
    int  encodeTranslation(const TranslateEntry& e, QByteArray& out); // bytes for patching

    EmuInstance* emuInstance = nullptr;

    QTableWidget* table = nullptr;
    QPushButton*  btnPause = nullptr;
    QPushButton*  btnScan = nullptr;
    QCheckBox*    chkAuto = nullptr;
    QComboBox*    cmbEncoding = nullptr;
    QSpinBox*     spnMinLen = nullptr;
    QLineEdit*    txtFilter = nullptr;
    QLabel*       lblStatus = nullptr;

    QTimer* scanTimer = nullptr;

    std::vector<TranslateEntry> Entries;
    QSet<QString> SeenKeys;
    int displayedEntryCount = 0;

    bool updatingTable = false;
};

#endif // TRANSLATEWINDOW_H
