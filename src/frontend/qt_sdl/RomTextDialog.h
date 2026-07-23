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

// ROM Text Translator
// -------------------
// Reads the ACTUAL text stored in the loaded cartridge ROM (not what is drawn on
// screen), decodes it to readable text, lets the user translate it, and writes a
// patched .nds. It parses the NDS filesystem, finds the game's pointer-table
// Shift-JIS text files ([u16 count][count u16 offsets][cp932 strings]) and shows
// every string as real, editable text.

#ifndef ROMTEXTDIALOG_H
#define ROMTEXTDIALOG_H

#include <QDialog>
#include <QString>
#include <QByteArray>
#include <QVector>
#include <QPair>
#include <vector>

#include "types.h"

class QTableWidget;
class QPushButton;
class QLineEdit;
class QLabel;
class EmuInstance;

struct RomTextFile
{
    QString path;
    melonDS::u32 start = 0;   // FAT start offset in ROM
    melonDS::u32 end = 0;     // FAT end offset in ROM
    QVector<QString> originals;
    QVector<QString> translations;
    QVector<QByteArray> raws;   // exact original bytes per string (round-trip)
};

class RomTextDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RomTextDialog(QWidget* parent);
    ~RomTextDialog() override;

    static RomTextDialog* currentDlg;
    static RomTextDialog* openDlg(QWidget* parent)
    {
        if (currentDlg) { currentDlg->activateWindow(); return currentDlg; }
        currentDlg = new RomTextDialog(parent);
        currentDlg->show();
        return currentDlg;
    }
    static void closeDlg() { currentDlg = nullptr; }

private slots:
    void onScan();
    void onFilterChanged(const QString& text);
    void onCellChanged(int row, int col);
    void onSaveProject();
    void onLoadProject();
    void onCreateRom();

private:
    void rebuildTable();
    const melonDS::u8* romData(melonDS::u32& len);

    EmuInstance* emuInstance = nullptr;

    QTableWidget* table = nullptr;
    QLineEdit* txtFilter = nullptr;
    QLabel* lblStatus = nullptr;

    std::vector<RomTextFile> Files;
    // flat index -> (fileIndex, stringIndex) for table rows
    QVector<QPair<int,int>> Rows;

    bool updatingTable = false;
};

#endif // ROMTEXTDIALOG_H
