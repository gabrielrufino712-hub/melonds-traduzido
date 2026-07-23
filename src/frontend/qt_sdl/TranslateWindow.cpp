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
#include "TranslateSJIS.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <algorithm>

#include "main.h"
#include "EmuInstance.h"
#include "EmuThread.h"
#include "NDS.h"
#include "NDSCart.h"

using namespace melonDS;

// ---------------------------------------------------------------------------
// Shift-JIS (cp932) helpers, backed by the auto-generated TranslateSJIS.h table
// ---------------------------------------------------------------------------

static const QHash<quint16, uint>& sjisForwardMap()
{
    static QHash<quint16, uint> map;
    if (map.isEmpty())
    {
        map.reserve(kSJISTableLen);
        for (int i = 0; i < kSJISTableLen; i++)
            map.insert(kSJISTable[i].key, kSJISTable[i].cp);
    }
    return map;
}

static const QHash<uint, quint16>& sjisReverseMap()
{
    static QHash<uint, quint16> map;
    if (map.isEmpty())
    {
        map.reserve(kSJISTableLen);
        for (int i = 0; i < kSJISTableLen; i++)
            if (!map.contains(kSJISTable[i].cp))
                map.insert(kSJISTable[i].cp, kSJISTable[i].key);
    }
    return map;
}

// Decode a run of Shift-JIS bytes to a QString. Returns the number of source
// bytes consumed via *consumed; used both for detection and display.
static bool sjisDecodeChar(const u8* data, int avail, uint& cp, int& len)
{
    u8 b = data[0];
    if (b == 0x00) return false;
    if (b >= 0x20 && b <= 0x7E) { cp = b; len = 1; return true; }              // ASCII
    if (b >= 0xA1 && b <= 0xDF) { cp = 0xFF61 + (b - 0xA1); len = 1; return true; } // half-width kana
    if (((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) && avail >= 2)
    {
        u8 t = data[1];
        if ((t >= 0x40 && t <= 0x7E) || (t >= 0x80 && t <= 0xFC))
        {
            quint16 key = (quint16)((b << 8) | t);
            auto it = sjisForwardMap().find(key);
            if (it != sjisForwardMap().end()) { cp = it.value(); len = 2; return true; }
        }
    }
    return false;
}

static QString utf8FromCodepoint(uint cp)
{
    if (cp <= 0xFFFF)
        return QString(QChar((char16_t)cp));
    char32_t c = (char32_t)cp;
    return QString::fromUcs4(&c, 1);
}

// ---------------------------------------------------------------------------

enum { COL_TIME, COL_ADDR, COL_ENC, COL_ORIG, COL_TRANS, COL_COUNT };

TranslateWindow* TranslateWindow::currentDlg = nullptr;

TranslateWindow::TranslateWindow(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Translate Mode");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 560);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();

    auto* root = new QVBoxLayout(this);

    // ---- top toolbar ----
    auto* bar = new QHBoxLayout();

    btnPause = new QPushButton("Pause emulation", this);
    connect(btnPause, &QPushButton::clicked, this, &TranslateWindow::onTogglePause);
    bar->addWidget(btnPause);

    btnScan = new QPushButton("Scan now", this);
    connect(btnScan, &QPushButton::clicked, this, &TranslateWindow::onScanNow);
    bar->addWidget(btnScan);

    chkAuto = new QCheckBox("Auto-scan", this);
    chkAuto->setChecked(true);
    bar->addWidget(chkAuto);

    bar->addWidget(new QLabel("Encoding:", this));
    cmbEncoding = new QComboBox(this);
    cmbEncoding->addItem("Shift-JIS + ASCII");
    cmbEncoding->addItem("ASCII only");
    cmbEncoding->addItem("Shift-JIS only");
    bar->addWidget(cmbEncoding);

    bar->addWidget(new QLabel("Min length:", this));
    spnMinLen = new QSpinBox(this);
    spnMinLen->setRange(2, 64);
    spnMinLen->setValue(3);
    bar->addWidget(spnMinLen);

    auto* btnClear = new QPushButton("Clear log", this);
    connect(btnClear, &QPushButton::clicked, this, &TranslateWindow::onClear);
    bar->addWidget(btnClear);

    bar->addStretch();
    root->addLayout(bar);

    // ---- filter ----
    auto* filterBar = new QHBoxLayout();
    filterBar->addWidget(new QLabel("Filter:", this));
    txtFilter = new QLineEdit(this);
    txtFilter->setPlaceholderText("type to filter the captured text...");
    connect(txtFilter, &QLineEdit::textChanged, this, &TranslateWindow::onFilterChanged);
    filterBar->addWidget(txtFilter);
    root->addLayout(filterBar);

    // ---- table ----
    table = new QTableWidget(this);
    table->setColumnCount(COL_COUNT);
    table->setHorizontalHeaderLabels({ "Time", "Address", "Enc", "Original text", "Translation" });
    table->horizontalHeader()->setSectionResizeMode(COL_ORIG, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(COL_TRANS, QHeaderView::Stretch);
    table->setColumnWidth(COL_TIME, 70);
    table->setColumnWidth(COL_ADDR, 90);
    table->setColumnWidth(COL_ENC, 50);
    table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(table, &QTableWidget::cellChanged, this, &TranslateWindow::onCellChanged);
    root->addWidget(table);

    // ---- bottom action row ----
    auto* actions = new QHBoxLayout();

    auto* btnApply = new QPushButton("Apply translation to RAM (live preview)", this);
    connect(btnApply, &QPushButton::clicked, this, &TranslateWindow::onApplyToRAM);
    actions->addWidget(btnApply);

    auto* btnLoad = new QPushButton("Load project...", this);
    connect(btnLoad, &QPushButton::clicked, this, &TranslateWindow::onLoadProject);
    actions->addWidget(btnLoad);

    auto* btnSave = new QPushButton("Save project...", this);
    connect(btnSave, &QPushButton::clicked, this, &TranslateWindow::onSaveProject);
    actions->addWidget(btnSave);

    actions->addStretch();

    auto* btnPatch = new QPushButton("Create patched ROM...", this);
    btnPatch->setStyleSheet("font-weight: bold;");
    connect(btnPatch, &QPushButton::clicked, this, &TranslateWindow::onCreatePatchedROM);
    actions->addWidget(btnPatch);

    root->addLayout(actions);

    lblStatus = new QLabel("Ready. Start a game and the captured text will appear here.", this);
    root->addWidget(lblStatus);

    scanTimer = new QTimer(this);
    scanTimer->setInterval(500);
    connect(scanTimer, &QTimer::timeout, this, &TranslateWindow::onTick);
    scanTimer->start();

    refreshPauseButton();
}

TranslateWindow::~TranslateWindow()
{
    TranslateWindow::closeDlg();
}

// ---------------------------------------------------------------------------
// scanning
// ---------------------------------------------------------------------------

void TranslateWindow::onTick()
{
    refreshPauseButton();
    if (chkAuto->isChecked())
        doScan();
}

void TranslateWindow::onScanNow()
{
    doScan();
}

void TranslateWindow::doScan()
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds || !nds->MainRAM) return;
    EmuThread* thread = emuInstance->getEmuThread();
    if (thread && !thread->emuIsActive()) return;

    const int mode = cmbEncoding->currentIndex(); // 0 both, 1 ascii, 2 sjis
    const int minLen = spnMinLen->value();
    const u32 mask = nds->MainRAMMask;
    const u32 size = mask + 1;
    const u8* ram = nds->MainRAM;

    const bool wantAscii = (mode == 0 || mode == 1);
    const bool wantSjis  = (mode == 0 || mode == 2);

    int added = 0;
    const int maxEntries = 8000;

    for (u32 i = 0; i < size && Entries.size() < (size_t)maxEntries; )
    {
        u8 b = ram[i];

        // Try a Shift-JIS run first (it is a superset of ASCII and can contain kana/kanji).
        if (wantSjis && b != 0x00)
        {
            u32 j = i;
            int chars = 0;
            bool hasMulti = false;
            QString decoded;
            QByteArray raw;
            while (j < size)
            {
                uint cp; int len;
                if (!sjisDecodeChar(ram + j, (int)(size - j), cp, len)) break;
                if (len == 2 || cp >= 0xFF61) hasMulti = true;
                decoded += utf8FromCodepoint(cp);
                raw.append((const char*)(ram + j), len);
                j += len;
                chars++;
                if (chars > 200) break;
            }
            if (chars >= minLen && hasMulti)
            {
                addOrUpdateEntry(i, raw, decoded, 1);
                added++;
                i = j;
                continue;
            }
        }

        // ASCII run.
        if (wantAscii && b >= 0x20 && b <= 0x7E)
        {
            u32 j = i;
            while (j < size && ram[j] >= 0x20 && ram[j] <= 0x7E && (j - i) < 200) j++;
            int len = (int)(j - i);
            if (len >= minLen)
            {
                QByteArray raw((const char*)(ram + i), len);
                addOrUpdateEntry(i, raw, QString::fromLatin1(raw), 0);
                added++;
            }
            i = j;
            continue;
        }

        i++;
    }

    if (added > 0)
        appendNewRows();

    lblStatus->setText(QString("Captured strings: %1   (last scan added %2)")
                       .arg((int)Entries.size()).arg(added));
}

void TranslateWindow::addOrUpdateEntry(u32 addr, const QByteArray& raw, const QString& text, int enc)
{
    QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return;

    QString key = QString::number(enc) + ":" + text;
    if (SeenKeys.contains(key)) return;
    SeenKeys.insert(key);

    TranslateEntry e;
    e.Address = addr;
    e.RawBytes = raw;
    e.Original = text;
    e.Encoding = enc;
    Entries.push_back(e);
}

// ---------------------------------------------------------------------------
// table
// ---------------------------------------------------------------------------

void TranslateWindow::rebuildTable()
{
    updatingTable = true;
    table->setRowCount(0);
    displayedEntryCount = 0;
    updatingTable = false;
    appendNewRows();
}

// Add rows only for entries that have not been displayed yet (keeps existing
// rows and any in-progress edit untouched during auto-scan).
void TranslateWindow::appendNewRows()
{
    updatingTable = true;
    const QString filter = txtFilter->text();
    for (int k = displayedEntryCount; k < (int)Entries.size(); k++)
    {
        const TranslateEntry& e = Entries[k];
        if (!filter.isEmpty() &&
            !e.Original.contains(filter, Qt::CaseInsensitive) &&
            !e.Translation.contains(filter, Qt::CaseInsensitive))
            continue;
        addTableRow(k);
    }
    displayedEntryCount = (int)Entries.size();
    updatingTable = false;
}

int TranslateWindow::addTableRow(int entryIndex)
{
    const TranslateEntry& e = Entries[entryIndex];
    int r = table->rowCount();
    table->insertRow(r);

    auto setCell = [&](int col, const QString& txt, bool editable)
    {
        QTableWidgetItem* it = new QTableWidgetItem(txt);
        it->setData(Qt::UserRole, entryIndex);
        if (editable) it->setFlags(it->flags() | Qt::ItemIsEditable);
        else          it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        table->setItem(r, col, it);
    };

    setCell(COL_TIME, QDateTime::currentDateTime().toString("hh:mm:ss"), false);
    setCell(COL_ADDR, QString("%1").arg(0x02000000 + e.Address, 8, 16, QChar('0')), false);
    setCell(COL_ENC, e.Encoding == 1 ? "SJIS" : "ASCII", false);
    setCell(COL_ORIG, e.Original, false);
    setCell(COL_TRANS, e.Translation, true);
    return r;
}

void TranslateWindow::onCellChanged(int row, int col)
{
    if (updatingTable) return;
    if (col != COL_TRANS) return;
    QTableWidgetItem* it = table->item(row, col);
    if (!it) return;
    int idx = it->data(Qt::UserRole).toInt();
    if (idx < 0 || idx >= (int)Entries.size()) return;
    Entries[idx].Translation = it->text();
}

void TranslateWindow::onFilterChanged(const QString&)
{
    rebuildTable();
}

void TranslateWindow::onClear()
{
    Entries.clear();
    SeenKeys.clear();
    displayedEntryCount = 0;
    table->setRowCount(0);
    lblStatus->setText("Log cleared.");
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
    // A fresh scan right after pausing shows exactly what is on screen now.
    doScan();
}

// ---------------------------------------------------------------------------
// translation encoding / RAM patching
// ---------------------------------------------------------------------------

int TranslateWindow::encodeTranslation(const TranslateEntry& e, QByteArray& out)
{
    const int L = e.RawBytes.size();
    QByteArray enc;

    if (e.Encoding == 1)
    {
        // Encode back to Shift-JIS where possible so the game font can render it.
        const auto& rev = sjisReverseMap();
        for (QChar c : e.Translation)
        {
            uint cp = c.unicode();
            if (cp >= 0x20 && cp <= 0x7E) { enc.append((char)cp); continue; }
            if (cp >= 0xFF61 && cp <= 0xFF9F) { enc.append((char)(0xA1 + (cp - 0xFF61))); continue; }
            auto it = rev.find(cp);
            if (it != rev.end()) { quint16 k = it.value(); enc.append((char)(k >> 8)); enc.append((char)(k & 0xFF)); }
            else enc.append('?');
        }
    }
    else
    {
        enc = e.Translation.toUtf8();
    }

    out = enc.left(L);
    // pad with 0x00 to keep original length (offsets stay valid)
    while (out.size() < L) out.append('\0');
    return enc.size() > L ? (enc.size() - L) : 0; // overflow byte count (truncated)
}

void TranslateWindow::writeTranslationToRAM(TranslateEntry& e)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds || !nds->MainRAM) return;
    if (e.Translation.isEmpty()) return;

    QByteArray bytes;
    encodeTranslation(e, bytes);
    const u32 mask = nds->MainRAMMask;
    for (int k = 0; k < bytes.size(); k++)
        nds->MainRAM[(e.Address + k) & mask] = (u8)bytes[k];
    e.PatchedRAM = true;
}

void TranslateWindow::onApplyToRAM()
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds || !nds->MainRAM)
    {
        QMessageBox::warning(this, "Translate Mode", "No game is running.");
        return;
    }
    int n = 0, trunc = 0;
    for (auto& e : Entries)
    {
        if (e.Translation.isEmpty()) continue;
        QByteArray bytes;
        trunc += encodeTranslation(e, bytes);
        const u32 mask = nds->MainRAMMask;
        for (int k = 0; k < bytes.size(); k++)
            nds->MainRAM[(e.Address + k) & mask] = (u8)bytes[k];
        e.PatchedRAM = true;
        n++;
    }
    lblStatus->setText(QString("Applied %1 translation(s) to live RAM%2.")
                       .arg(n)
                       .arg(trunc ? QString(" (%1 byte(s) truncated to fit)").arg(trunc) : QString()));
}

// ---------------------------------------------------------------------------
// patched ROM
// ---------------------------------------------------------------------------

void TranslateWindow::onCreatePatchedROM()
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds)
    {
        QMessageBox::warning(this, "Translate Mode", "No game is running.");
        return;
    }
    NDSCart::CartCommon* cart = nds->GetNDSCart();
    if (!cart || !cart->GetROM())
    {
        QMessageBox::warning(this, "Translate Mode", "No cartridge ROM is loaded.");
        return;
    }

    const u8* rom = cart->GetROM();
    const u32 romlen = cart->GetROMLength();

    // Work on a mutable copy of the ROM.
    QByteArray patched((const char*)rom, (int)romlen);

    int strDone = 0, totalHits = 0, notFound = 0, trunc = 0;
    QStringList missing;

    for (const auto& e : Entries)
    {
        if (e.Translation.isEmpty()) continue;

        QByteArray repl;
        trunc += encodeTranslation(e, repl); // exactly RawBytes.size() bytes

        const QByteArray& needle = e.RawBytes;
        if (needle.isEmpty()) continue;

        int from = 0, hits = 0;
        while (true)
        {
            int at = patched.indexOf(needle, from);
            if (at < 0) break;
            patched.replace(at, needle.size(), repl);
            from = at + repl.size();
            hits++;
        }
        if (hits == 0)
        {
            notFound++;
            if (missing.size() < 15) missing << e.Original;
        }
        else
        {
            strDone++;
            totalHits += hits;
        }
    }

    if (strDone == 0)
    {
        QMessageBox::warning(this, "Translate Mode",
            "None of the translated strings were found in the ROM.\n\n"
            "This usually means the game stores that text compressed or packed. "
            "You can still use \"Apply translation to RAM (live preview)\" to see the "
            "change in the running game.");
        return;
    }

    QString fn = QFileDialog::getSaveFileName(this, "Save patched ROM",
                                              "translated.nds",
                                              "Nintendo DS ROM (*.nds)");
    if (fn.isEmpty()) return;

    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly))
    {
        QMessageBox::critical(this, "Translate Mode", "Could not open the output file for writing.");
        return;
    }
    f.write(patched);
    f.close();

    QString msg = QString("Patched ROM written to:\n%1\n\n"
                          "Strings patched: %2 (%3 occurrence(s))")
                          .arg(fn).arg(strDone).arg(totalHits);
    if (notFound)
        msg += QString("\nNot found in ROM (likely compressed): %1").arg(notFound);
    if (trunc)
        msg += QString("\nBytes truncated to preserve length: %1").arg(trunc);
    if (!missing.isEmpty())
        msg += "\n\nExamples not found:\n - " + missing.join("\n - ");

    QMessageBox::information(this, "Translate Mode", msg);
    lblStatus->setText(QString("Patched ROM saved (%1 strings, %2 hits).").arg(strDone).arg(totalHits));
}

// ---------------------------------------------------------------------------
// project save / load (JSON)
// ---------------------------------------------------------------------------

void TranslateWindow::onSaveProject()
{
    QString fn = QFileDialog::getSaveFileName(this, "Save translation project",
                                              "translation.json",
                                              "Translation project (*.json)");
    if (fn.isEmpty()) return;

    QJsonArray arr;
    for (const auto& e : Entries)
    {
        QJsonObject o;
        o["address"] = (double)e.Address;
        o["encoding"] = e.Encoding;
        o["raw"] = QString::fromLatin1(e.RawBytes.toHex());
        o["original"] = e.Original;
        o["translation"] = e.Translation;
        arr.append(o);
    }
    QJsonObject root;
    root["tool"] = "melonDS Translate Mode";
    root["entries"] = arr;

    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly))
    {
        QMessageBox::critical(this, "Translate Mode", "Could not open the file for writing.");
        return;
    }
    f.write(QJsonDocument(root).toJson());
    f.close();
    lblStatus->setText(QString("Project saved: %1 entries.").arg((int)Entries.size()));
}

void TranslateWindow::onLoadProject()
{
    QString fn = QFileDialog::getOpenFileName(this, "Load translation project",
                                              "", "Translation project (*.json)");
    if (fn.isEmpty()) return;

    QFile f(fn);
    if (!f.open(QIODevice::ReadOnly))
    {
        QMessageBox::critical(this, "Translate Mode", "Could not open the file.");
        return;
    }
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject())
    {
        QMessageBox::critical(this, "Translate Mode", "Invalid project file.");
        return;
    }

    QJsonArray arr = doc.object()["entries"].toArray();
    int loaded = 0;
    for (const auto& v : arr)
    {
        QJsonObject o = v.toObject();
        QByteArray raw = QByteArray::fromHex(o["raw"].toString().toLatin1());
        int enc = o["encoding"].toInt();
        QString orig = o["original"].toString();
        QString key = QString::number(enc) + ":" + orig;

        // merge translation into an existing captured entry, or add a new one
        bool merged = false;
        for (auto& e : Entries)
        {
            if (e.Encoding == enc && e.Original == orig)
            {
                e.Translation = o["translation"].toString();
                merged = true;
                break;
            }
        }
        if (!merged)
        {
            TranslateEntry e;
            e.Address = (u32)o["address"].toDouble();
            e.Encoding = enc;
            e.RawBytes = raw;
            e.Original = orig;
            e.Translation = o["translation"].toString();
            Entries.push_back(e);
            SeenKeys.insert(key);
        }
        loaded++;
    }
    rebuildTable();
    lblStatus->setText(QString("Project loaded: %1 entries.").arg(loaded));
}
