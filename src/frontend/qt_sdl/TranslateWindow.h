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

// Translate Mode (unified)
// ------------------------
// Reads the REAL text stored in the cartridge ROM (parses the NDS filesystem and
// the game's pointer-table Shift-JIS text files), shows every string as readable,
// editable text, and can write a patched translated .nds. While the game runs it
// also scans main RAM and HIGHLIGHTS the strings the game is currently using
// (the text loaded/on screen at this moment), and can apply a translation live.

#ifndef TRANSLATEWINDOW_H
#define TRANSLATEWINDOW_H

#include <QDialog>
#include <QTimer>
#include <QString>
#include <QByteArray>
#include <QVector>
#include <QPair>
#include <QHash>
#include <vector>

#include "types.h"

class QTableWidget;
class QPushButton;
class QCheckBox;
class QLineEdit;
class QLabel;
class EmuInstance;

namespace melonDS { class NDS; }

struct RomTextFile
{
    QString path;
    melonDS::u32 id = 0;
    melonDS::u32 start = 0;
    melonDS::u32 end = 0;
    QVector<QString> originals;
    QVector<QString> translations;
    QVector<QByteArray> raws;
    QVector<char> active;             // currently present in RAM?
    QVector<QVector<melonDS::u32>> addrs;  // all RAM addresses where it was found
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
        if (currentDlg) { currentDlg->activateWindow(); return currentDlg; }
        currentDlg = new TranslateWindow(parent);
        currentDlg->show();
        return currentDlg;
    }
    static void closeDlg() { currentDlg = nullptr; }

private slots:
    void onTick();
    void onScan();
    void onTogglePause();
    void onFilterChanged(const QString& text);
    void onCellChanged(int row, int col);
    void onApplyLive();
    void onSaveProject();
    void onLoadProject();
    void onCreateRom();

private:
    void rebuildTable();
    void buildPrefixIndex();
    void scanActive();
    void refreshPauseButton();
    const melonDS::u8* romData(melonDS::u32& len);

    EmuInstance* emuInstance = nullptr;

    QTableWidget* table = nullptr;
    QPushButton*  btnPause = nullptr;
    QCheckBox*    chkHighlight = nullptr;
    QCheckBox*    chkFollow = nullptr;
    QCheckBox*    chkActiveOnly = nullptr;
    QLineEdit*    txtFilter = nullptr;
    QLabel*       lblStatus = nullptr;

    QTimer* timer = nullptr;

    std::vector<RomTextFile> Files;
    QVector<QPair<int,int>> Rows;                 // table row -> (file, string)
    QHash<int,int> RowOf;                          // (file*100000+str) -> table row
    QMultiHash<quint32, QPair<int,int>> Prefix;    // first 4 raw bytes -> (file,string)

    bool updatingTable = false;
};

#endif // TRANSLATEWINDOW_H
