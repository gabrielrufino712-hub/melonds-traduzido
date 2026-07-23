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
#include <QGridLayout>
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
#include <QTextStream>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QHash>
#include <QStringConverter>
#include <QColor>
#include <QBrush>
#include <QScrollBar>
#include <algorithm>

#include "main.h"
#include "EmuInstance.h"
#include "EmuThread.h"
#include "NDS.h"
#include "NDSCart.h"
#include "GPU.h"

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

static bool sjisDecodeChar(const u8* data, int avail, uint& cp, int& len)
{
    u8 b = data[0];
    if (b == 0x00) return false;
    if (b >= 0x20 && b <= 0x7E) { cp = b; len = 1; return true; }
    if (b >= 0xA1 && b <= 0xDF) { cp = 0xFF61 + (b - 0xA1); len = 1; return true; }
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

// "Real" Japanese script: full-width hiragana/katakana and kanji. Excludes
// half-width katakana and latin/symbols, which is what most random-byte false
// positives decode to.
static bool isRealJapaneseCp(uint cp)
{
    if (cp >= 0x3040 && cp <= 0x30FF) return true; // hiragana + katakana
    if (cp >= 0x3400 && cp <= 0x4DBF) return true; // CJK ext A
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true; // CJK unified (kanji)
    if (cp >= 0xF900 && cp <= 0xFAFF) return true; // CJK compatibility
    return false;
}

// Generic "is this codepoint printable text?"
static bool isTextCp(uint cp)
{
    if (cp >= 0x20 && cp < 0x7F) return true;
    if (cp >= 0xA0 && cp != 0xFFFD) return true;
    return false;
}

// Minimal UTF-8 decoder. Returns bytes consumed (0 = invalid).
static int utf8Decode(const u8* p, int avail, uint& cp)
{
    u8 b = p[0];
    if (b < 0x80) { cp = b; return 1; }
    if ((b & 0xE0) == 0xC0 && avail >= 2 && (p[1] & 0xC0) == 0x80)
    { cp = ((b & 0x1F) << 6) | (p[1] & 0x3F); return cp >= 0x80 ? 2 : 0; }
    if ((b & 0xF0) == 0xE0 && avail >= 3 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
    { cp = ((b & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); return cp >= 0x800 ? 3 : 0; }
    if ((b & 0xF8) == 0xF0 && avail >= 4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
    { cp = ((b & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); return cp >= 0x10000 ? 4 : 0; }
    return 0;
}

// ---------------------------------------------------------------------------

void CharTable::buildEncodeList()
{
    encodeList.clear();
    for (auto it = two.constBegin(); it != two.constEnd(); ++it)
    {
        QByteArray b; b.append((char)(it.key() >> 8)); b.append((char)(it.key() & 0xFF));
        encodeList.append({ it.value(), b });
    }
    for (auto it = one.constBegin(); it != one.constEnd(); ++it)
    {
        QByteArray b; b.append((char)it.key());
        encodeList.append({ it.value(), b });
    }
    // longest source string first, so multi-character tokens win
    std::sort(encodeList.begin(), encodeList.end(),
              [](const QPair<QString,QByteArray>& a, const QPair<QString,QByteArray>& b)
              { return a.first.size() > b.first.size(); });
}

// ---------------------------------------------------------------------------

enum { COL_TIME, COL_SRC, COL_ADDR, COL_ENC, COL_ORIG, COL_TRANS, COL_COUNT };

static const char* encName(int e)
{
    switch (e)
    {
    case ENC_ASCII:   return "ASCII";
    case ENC_SJIS:    return "SJIS";
    case ENC_UTF16LE: return "UTF16";
    case ENC_UTF8:    return "UTF8";
    case ENC_TABLE:   return "TABLE";
    default:          return "?";
    }
}

TranslateWindow* TranslateWindow::currentDlg = nullptr;

TranslateWindow::TranslateWindow(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Translate Mode");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(980, 620);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();

    auto* root = new QVBoxLayout(this);

    // ---- row 1: scan controls ----
    auto* bar = new QHBoxLayout();

    btnPause = new QPushButton("Pause emulation", this);
    connect(btnPause, &QPushButton::clicked, this, &TranslateWindow::onTogglePause);
    bar->addWidget(btnPause);

    btnScanRAM = new QPushButton("Scan RAM", this);
    connect(btnScanRAM, &QPushButton::clicked, this, &TranslateWindow::onScanRAM);
    bar->addWidget(btnScanRAM);

    btnScanROM = new QPushButton("Scan ROM (full)", this);
    connect(btnScanROM, &QPushButton::clicked, this, &TranslateWindow::onScanROM);
    bar->addWidget(btnScanROM);

    chkAuto = new QCheckBox("Auto-scan RAM", this);
    chkAuto->setChecked(true);
    bar->addWidget(chkAuto);

    chkJpOnly = new QCheckBox("Japanese only", this);
    chkJpOnly->setChecked(false);
    chkJpOnly->setToolTip("Keep only strings that contain real Japanese (hiragana, "
                          "katakana or kanji). Removes random-byte noise for JP games.");
    bar->addWidget(chkJpOnly);

    chkHighlight = new QCheckBox("Highlight on-screen", this);
    chkHighlight->setChecked(true);
    chkHighlight->setToolTip("Highlight and scroll to the strings that just appeared in "
                             "memory on each scan (i.e. what is on screen now).");
    bar->addWidget(chkHighlight);

    auto* btnGuide = new QPushButton("Guide", this);
    connect(btnGuide, &QPushButton::clicked, this, &TranslateWindow::onGuide);
    bar->addWidget(btnGuide);

    bar->addStretch();
    root->addLayout(bar);

    // ---- row 2: encoding / options ----
    auto* opt = new QHBoxLayout();

    opt->addWidget(new QLabel("Encoding:", this));
    cmbEncoding = new QComboBox(this);
    cmbEncoding->addItem("ASCII");
    cmbEncoding->addItem("Shift-JIS (JP)");
    cmbEncoding->addItem("UTF-16LE");
    cmbEncoding->addItem("UTF-8");
    cmbEncoding->addItem("Custom table (.tbl)");
    cmbEncoding->setCurrentIndex(ENC_SJIS);
    opt->addWidget(cmbEncoding);

    opt->addWidget(new QLabel("Min length:", this));
    spnMinLen = new QSpinBox(this);
    spnMinLen->setRange(2, 64);
    spnMinLen->setValue(4);
    opt->addWidget(spnMinLen);

    auto* btnTable = new QPushButton("Load table...", this);
    connect(btnTable, &QPushButton::clicked, this, &TranslateWindow::onLoadTable);
    opt->addWidget(btnTable);

    lblTable = new QLabel("(no table)", this);
    opt->addWidget(lblTable);

    auto* btnClear = new QPushButton("Clear log", this);
    connect(btnClear, &QPushButton::clicked, this, &TranslateWindow::onClear);
    opt->addWidget(btnClear);

    opt->addStretch();
    root->addLayout(opt);

    // ---- row 3: filter + relative search ----
    auto* find = new QHBoxLayout();
    find->addWidget(new QLabel("Filter:", this));
    txtFilter = new QLineEdit(this);
    txtFilter->setPlaceholderText("type to filter the captured text...");
    connect(txtFilter, &QLineEdit::textChanged, this, &TranslateWindow::onFilterChanged);
    find->addWidget(txtFilter, 2);

    find->addWidget(new QLabel("Relative search:", this));
    txtRelSearch = new QLineEdit(this);
    txtRelSearch->setPlaceholderText("known text, e.g. ABCDE");
    find->addWidget(txtRelSearch, 1);
    auto* btnRel = new QPushButton("Find", this);
    btnRel->setToolTip("Single-byte relative search: finds where a known text is stored "
                       "even without a table, to help you build one.");
    connect(btnRel, &QPushButton::clicked, this, &TranslateWindow::onRelativeSearch);
    find->addWidget(btnRel);

    btnInspect = new QPushButton("Inspect (click screen)", this);
    btnInspect->setCheckable(true);
    btnInspect->setToolTip("EXPERIMENTAL. Arm, then click text on the bottom (touch) "
                           "screen to read the background tiles under the cursor.");
    connect(btnInspect, &QPushButton::toggled, this, &TranslateWindow::onToggleInspect);
    find->addWidget(btnInspect);
    root->addLayout(find);

    // ---- table ----
    table = new QTableWidget(this);
    table->setColumnCount(COL_COUNT);
    table->setHorizontalHeaderLabels({ "Time", "Src", "Address", "Enc", "Original text", "Translation" });
    table->horizontalHeader()->setSectionResizeMode(COL_ORIG, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(COL_TRANS, QHeaderView::Stretch);
    table->setColumnWidth(COL_TIME, 66);
    table->setColumnWidth(COL_SRC, 42);
    table->setColumnWidth(COL_ADDR, 110);
    table->setColumnWidth(COL_ENC, 54);
    table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(table, &QTableWidget::cellChanged, this, &TranslateWindow::onCellChanged);
    root->addWidget(table);

    // ---- bottom action row ----
    auto* actions = new QHBoxLayout();

    auto* btnApply = new QPushButton("Apply to RAM (live)", this);
    connect(btnApply, &QPushButton::clicked, this, &TranslateWindow::onApplyToRAM);
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
        doScan(SRC_RAM);
}

void TranslateWindow::onScanRAM() { doScan(SRC_RAM); }

void TranslateWindow::onScanROM() { doScan(SRC_ROM); }

void TranslateWindow::doScan(int source)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return;
    EmuThread* thread = emuInstance->getEmuThread();
    if (thread && !thread->emuIsActive()) return;

    if (source == SRC_RAM)
    {
        if (!nds->MainRAM) return;
        scanBuffer(nds->MainRAM, nds->MainRAMMask + 1, SRC_RAM);
    }
    else
    {
        NDSCart::CartCommon* cart = nds->GetNDSCart();
        if (!cart || !cart->GetROM())
        {
            lblStatus->setText("No cartridge ROM is loaded.");
            return;
        }
        scanBuffer(cart->GetROM(), cart->GetROMLength(), SRC_ROM);
    }
}

void TranslateWindow::scanBuffer(const u8* buf, u32 size, int source)
{
    const int enc = cmbEncoding->currentIndex();
    const int minLen = spnMinLen->value();
    const bool jpOnly = chkJpOnly->isChecked();
    const int maxEntries = 20000;

    if (enc == ENC_TABLE && !Table.isLoaded())
    {
        lblStatus->setText("Custom table encoding selected but no .tbl loaded. Use \"Load table...\".");
        return;
    }

    int added = 0;
    const size_t before = Entries.size();

    for (u32 i = 0; i < size && Entries.size() < (size_t)maxEntries; )
    {
        qint64 roff = (source == SRC_ROM) ? (qint64)i : -1;

        if (enc == ENC_ASCII)
        {
            if (buf[i] >= 0x20 && buf[i] <= 0x7E)
            {
                u32 j = i;
                while (j < size && buf[j] >= 0x20 && buf[j] <= 0x7E && (j - i) < 400) j++;
                int len = (int)(j - i);
                if (len >= minLen)
                {
                    QByteArray raw((const char*)(buf + i), len);
                    addOrUpdateEntry(i, roff, source, raw, QString::fromLatin1(raw), ENC_ASCII);
                    added++;
                }
                i = j; continue;
            }
            i++; continue;
        }
        else if (enc == ENC_SJIS)
        {
            if (buf[i] != 0x00)
            {
                u32 j = i; int chars = 0, jpChars = 0; bool hasMulti = false;
                QString decoded; QByteArray raw;
                while (j < size)
                {
                    uint cp; int len;
                    if (!sjisDecodeChar(buf + j, (int)(size - j), cp, len)) break;
                    if (len == 2 || cp >= 0xFF61) hasMulti = true;
                    if (isRealJapaneseCp(cp)) jpChars++;
                    decoded += utf8FromCodepoint(cp);
                    raw.append((const char*)(buf + j), len);
                    j += len; chars++;
                    if (chars > 300) break;
                }
                bool accept = jpOnly ? (chars >= minLen && jpChars >= 2 && jpChars * 2 >= chars)
                                     : (chars >= minLen && hasMulti);
                if (accept) { addOrUpdateEntry(i, roff, source, raw, decoded, ENC_SJIS); added++; i = j; continue; }
            }
            i++; continue;
        }
        else if (enc == ENC_UTF16LE)
        {
            if (i + 1 < size)
            {
                u32 j = i; int chars = 0, jpChars = 0, ascii = 0;
                QString decoded; QByteArray raw;
                while (j + 1 < size)
                {
                    uint cp = buf[j] | (buf[j + 1] << 8);
                    if (!isTextCp(cp)) break;
                    if (isRealJapaneseCp(cp)) jpChars++;
                    if (cp < 0x80) ascii++;
                    decoded += utf8FromCodepoint(cp);
                    raw.append((const char*)(buf + j), 2);
                    j += 2; chars++;
                    if (chars > 300) break;
                }
                bool accept = jpOnly ? (chars >= minLen && jpChars >= 2 && jpChars * 2 >= chars)
                                     : (chars >= minLen);
                if (accept) { addOrUpdateEntry(i, roff, source, raw, decoded, ENC_UTF16LE); added++; i = j; continue; }
            }
            i++; continue;
        }
        else if (enc == ENC_UTF8)
        {
            if (buf[i] != 0x00)
            {
                u32 j = i; int chars = 0, jpChars = 0; bool hasMulti = false;
                QString decoded; QByteArray raw;
                while (j < size)
                {
                    uint cp; int len = utf8Decode(buf + j, (int)(size - j), cp);
                    if (len == 0 || !isTextCp(cp)) break;
                    if (len > 1) hasMulti = true;
                    if (isRealJapaneseCp(cp)) jpChars++;
                    decoded += utf8FromCodepoint(cp);
                    raw.append((const char*)(buf + j), len);
                    j += len; chars++;
                    if (chars > 300) break;
                }
                bool accept = jpOnly ? (chars >= minLen && jpChars >= 2 && jpChars * 2 >= chars)
                                     : (chars >= minLen);
                if (accept) { addOrUpdateEntry(i, roff, source, raw, decoded, ENC_UTF8); added++; i = j; continue; }
            }
            i++; continue;
        }
        else // ENC_TABLE
        {
            u32 j = i; int chars = 0; QString decoded; QByteArray raw;
            while (j < size)
            {
                bool m = false;
                if (j + 1 < size)
                {
                    quint16 k = (quint16)((buf[j] << 8) | buf[j + 1]);
                    auto it = Table.two.find(k);
                    if (it != Table.two.end()) { decoded += it.value(); raw.append((const char*)(buf + j), 2); j += 2; m = true; }
                }
                if (!m)
                {
                    auto it = Table.one.find(buf[j]);
                    if (it != Table.one.end()) { decoded += it.value(); raw.append((const char*)(buf + j), 1); j += 1; m = true; }
                }
                if (!m) break;
                chars++;
                if (chars > 400) break;
            }
            if (chars >= minLen) { addOrUpdateEntry(i, roff, source, raw, decoded, ENC_TABLE); added++; i = j; continue; }
            i++; continue;
        }
    }

    if (Entries.size() != before)
        appendNewRows();

    lblStatus->setText(QString("Captured: %1   (%2 scan added %3%4)")
                       .arg((int)Entries.size())
                       .arg(source == SRC_ROM ? "ROM" : "RAM")
                       .arg(added)
                       .arg(Entries.size() >= (size_t)maxEntries ? ", limit reached" : ""));
}

void TranslateWindow::addOrUpdateEntry(u32 addr, qint64 romOffset, int source,
                                       const QByteArray& raw, const QString& text, int enc)
{
    if (text.trimmed().isEmpty()) return;

    QString key = QString::number(enc) + ":" + text;
    auto it = KeyToIndex.find(key);
    if (it != KeyToIndex.end())
    {
        // already known: fill in a ROM offset if we just learned it
        TranslateEntry& e = Entries[it.value()];
        if (e.RomOffset < 0 && romOffset >= 0) { e.RomOffset = romOffset; e.Source = source; }
        return;
    }

    TranslateEntry e;
    e.Address = addr;
    e.RomOffset = romOffset;
    e.Source = source;
    e.RawBytes = raw;
    e.Original = text;
    e.Encoding = enc;
    KeyToIndex.insert(key, (int)Entries.size());
    Entries.push_back(e);
}

// ---------------------------------------------------------------------------
// table widget
// ---------------------------------------------------------------------------

void TranslateWindow::rebuildTable()
{
    updatingTable = true;
    table->setRowCount(0);
    displayedEntryCount = 0;
    highlightedRows.clear();
    updatingTable = false;
    appendNewRows();
}

void TranslateWindow::appendNewRows()
{
    updatingTable = true;
    const QString filter = txtFilter->text();
    QVector<int> newRows;
    for (int k = displayedEntryCount; k < (int)Entries.size(); k++)
    {
        const TranslateEntry& e = Entries[k];
        if (!filter.isEmpty() &&
            !e.Original.contains(filter, Qt::CaseInsensitive) &&
            !e.Translation.contains(filter, Qt::CaseInsensitive))
            continue;
        newRows.append(addTableRow(k));
    }
    displayedEntryCount = (int)Entries.size();
    updatingTable = false;

    if (chkHighlight && chkHighlight->isChecked() && !newRows.isEmpty())
        applyHighlight(newRows);
}

// Colour the given rows (the ones that just appeared = what is on screen now)
// and scroll to the first, clearing the previous highlight.
void TranslateWindow::applyHighlight(const QVector<int>& rows)
{
    updatingTable = true;
    // clear old highlight
    for (int r : highlightedRows)
        for (int c = 0; c < table->columnCount(); c++)
            if (auto* it = table->item(r, c)) it->setBackground(QBrush());
    highlightedRows = rows;
    QColor hi(255, 244, 150);
    for (int r : rows)
        for (int c = 0; c < table->columnCount(); c++)
            if (auto* it = table->item(r, c)) it->setBackground(hi);
    updatingTable = false;
    if (!rows.isEmpty())
    {
        if (auto* it = table->item(rows.first(), COL_ORIG))
            table->scrollToItem(it, QAbstractItemView::PositionAtCenter);
    }
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

    QString addrStr = (e.Source == SRC_ROM)
        ? QString("ROM+%1").arg(e.RomOffset, 8, 16, QChar('0'))
        : QString("%1").arg(0x02000000 + e.Address, 8, 16, QChar('0'));

    setCell(COL_TIME, QDateTime::currentDateTime().toString("hh:mm:ss"), false);
    setCell(COL_SRC, e.Source == SRC_ROM ? "ROM" : "RAM", false);
    setCell(COL_ADDR, addrStr, false);
    setCell(COL_ENC, encName(e.Encoding), false);
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

void TranslateWindow::onFilterChanged(const QString&) { rebuildTable(); }

void TranslateWindow::onClear()
{
    Entries.clear();
    KeyToIndex.clear();
    displayedEntryCount = 0;
    highlightedRows.clear();
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
    doScan(SRC_RAM);
}

// ---------------------------------------------------------------------------
// translation encoding
// ---------------------------------------------------------------------------

int TranslateWindow::encodeTranslation(const TranslateEntry& e, QByteArray& out)
{
    const int L = e.RawBytes.size();
    QByteArray enc;

    switch (e.Encoding)
    {
    case ENC_SJIS:
    {
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
        break;
    }
    case ENC_UTF16LE:
    {
        for (QChar c : e.Translation)
        {
            ushort u = c.unicode();
            enc.append((char)(u & 0xFF));
            enc.append((char)(u >> 8));
        }
        break;
    }
    case ENC_TABLE:
    {
        const QString& s = e.Translation;
        int pos = 0;
        while (pos < s.size())
        {
            bool matched = false;
            for (const auto& pair : Table.encodeList)
            {
                const QString& tok = pair.first;
                if (!tok.isEmpty() && s.mid(pos, tok.size()) == tok)
                {
                    enc.append(pair.second);
                    pos += tok.size();
                    matched = true;
                    break;
                }
            }
            if (!matched) { enc.append('?'); pos++; }
        }
        break;
    }
    case ENC_UTF8:
    case ENC_ASCII:
    default:
        enc = e.Translation.toUtf8();
        break;
    }

    out = enc.left(L);
    while (out.size() < L) out.append('\0');
    return enc.size() > L ? (enc.size() - L) : 0;
}

void TranslateWindow::onApplyToRAM()
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds || !nds->MainRAM)
    {
        QMessageBox::warning(this, "Translate Mode", "No game is running.");
        return;
    }
    int n = 0, trunc = 0, skipped = 0;
    const u32 mask = nds->MainRAMMask;
    for (auto& e : Entries)
    {
        if (e.Translation.isEmpty()) continue;
        if (e.Source != SRC_RAM) { skipped++; continue; } // ROM entries aren't in RAM here
        QByteArray bytes;
        trunc += encodeTranslation(e, bytes);
        for (int k = 0; k < bytes.size(); k++)
            nds->MainRAM[(e.Address + k) & mask] = (u8)bytes[k];
        e.PatchedRAM = true;
        n++;
    }
    lblStatus->setText(QString("Applied %1 translation(s) to live RAM%2%3.")
                       .arg(n)
                       .arg(trunc ? QString(" (%1 byte(s) truncated)").arg(trunc) : QString())
                       .arg(skipped ? QString(" (%1 ROM-only skipped)").arg(skipped) : QString()));
}

// ---------------------------------------------------------------------------
// patched ROM
// ---------------------------------------------------------------------------

void TranslateWindow::onCreatePatchedROM()
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) { QMessageBox::warning(this, "Translate Mode", "No game is running."); return; }
    NDSCart::CartCommon* cart = nds->GetNDSCart();
    if (!cart || !cart->GetROM())
    { QMessageBox::warning(this, "Translate Mode", "No cartridge ROM is loaded."); return; }

    const u8* rom = cart->GetROM();
    const u32 romlen = cart->GetROMLength();
    QByteArray patched((const char*)rom, (int)romlen);

    int strDone = 0, totalHits = 0, notFound = 0, trunc = 0;
    QStringList missing;

    for (const auto& e : Entries)
    {
        if (e.Translation.isEmpty()) continue;

        QByteArray repl;
        trunc += encodeTranslation(e, repl); // length == RawBytes.size()

        if (e.RawBytes.isEmpty()) continue;

        int hits = 0;

        // exact offset if we captured it straight from the ROM
        if (e.Source == SRC_ROM && e.RomOffset >= 0 && e.RomOffset + repl.size() <= patched.size())
        {
            patched.replace((int)e.RomOffset, repl.size(), repl);
            hits = 1;
        }
        else
        {
            int from = 0;
            while (true)
            {
                int at = patched.indexOf(e.RawBytes, from);
                if (at < 0) break;
                patched.replace(at, e.RawBytes.size(), repl);
                from = at + repl.size();
                hits++;
            }
        }

        if (hits == 0) { notFound++; if (missing.size() < 15) missing << e.Original; }
        else { strDone++; totalHits += hits; }
    }

    if (strDone == 0)
    {
        QMessageBox::warning(this, "Translate Mode",
            "None of the translated strings were found in the ROM.\n\n"
            "The text is probably compressed/packed, or it lives only in RAM. "
            "Try \"Scan ROM (full)\" to capture it directly from the ROM, or use "
            "\"Apply to RAM (live)\" to preview in the running game.");
        return;
    }

    QString fn = QFileDialog::getSaveFileName(this, "Save patched ROM", "translated.nds",
                                              "Nintendo DS ROM (*.nds)");
    if (fn.isEmpty()) return;

    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly))
    { QMessageBox::critical(this, "Translate Mode", "Could not open the output file for writing."); return; }
    f.write(patched);
    f.close();

    QString msg = QString("Patched ROM written to:\n%1\n\nStrings patched: %2 (%3 occurrence(s))")
                          .arg(fn).arg(strDone).arg(totalHits);
    if (notFound) msg += QString("\nNot found in ROM (likely compressed): %1").arg(notFound);
    if (trunc)    msg += QString("\nBytes truncated to preserve length: %1").arg(trunc);
    if (!missing.isEmpty()) msg += "\n\nExamples not found:\n - " + missing.join("\n - ");

    QMessageBox::information(this, "Translate Mode", msg);
    lblStatus->setText(QString("Patched ROM saved (%1 strings, %2 hits).").arg(strDone).arg(totalHits));
}

// ---------------------------------------------------------------------------
// custom table (.tbl)
// ---------------------------------------------------------------------------

void TranslateWindow::onLoadTable()
{
    QString fn = QFileDialog::getOpenFileName(this, "Load character table",
                                              "", "Character table (*.tbl *.txt);;All files (*)");
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
        if (hex.size() == 2) { u8 b = (u8)hex.toUInt(&ok, 16); if (ok) { Table.one[b] = val; n++; } }
        else if (hex.size() == 4) { quint16 k = (quint16)hex.toUInt(&ok, 16); if (ok) { Table.two[k] = val; n++; } }
    }
    f.close();

    if (n == 0)
    { QMessageBox::warning(this, "Translate Mode", "No valid entries found (expected lines like \"41=A\" or \"8140= \")."); return; }

    Table.buildEncodeList();
    Table.loaded = true;
    lblTable->setText(QString("table: %1 entries").arg(n));
    cmbEncoding->setCurrentIndex(ENC_TABLE);
    lblStatus->setText(QString("Loaded table with %1 entries. Encoding set to Custom table.").arg(n));
}

// ---------------------------------------------------------------------------
// relative search
// ---------------------------------------------------------------------------

void TranslateWindow::onRelativeSearch()
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds || !nds->MainRAM) { QMessageBox::warning(this, "Translate Mode", "No game is running."); return; }

    QString s = txtRelSearch->text();
    if (s.size() < 2) { QMessageBox::information(this, "Translate Mode", "Type at least 2 characters to relative-search."); return; }

    std::vector<int> cps;
    for (QChar c : s) cps.push_back((int)c.unicode());

    const u8* buf = nds->MainRAM;
    const u32 size = nds->MainRAMMask + 1;
    const int n = (int)cps.size();

    QStringList hits;
    int count = 0;
    for (u32 k = 0; k + (u32)n <= size && count < 50; k++)
    {
        bool ok = true;
        for (int i = 0; i < n - 1; i++)
        {
            int bd = (int)buf[k + i + 1] - (int)buf[k + i];
            int cd = cps[i + 1] - cps[i];
            if (bd != cd) { ok = false; break; }
        }
        if (ok)
        {
            u8 first = buf[k];
            hits << QString("addr 0x%1  (first byte 0x%2 = '%3')")
                    .arg(0x02000000 + k, 8, 16, QChar('0'))
                    .arg(first, 2, 16, QChar('0'))
                    .arg(s.at(0));
            count++;
        }
    }

    if (hits.isEmpty())
        QMessageBox::information(this, "Relative search", "No single-byte relative match found in RAM.");
    else
        QMessageBox::information(this, "Relative search",
            QString("%1 match(es). Each line suggests: byte value = the character.\n"
                    "Use these to build your .tbl file.\n\n%2")
                    .arg(hits.size()).arg(hits.join("\n")));
}

// ---------------------------------------------------------------------------
// plain-text dump export / import
// ---------------------------------------------------------------------------

void TranslateWindow::onExportTxt()
{
    QString fn = QFileDialog::getSaveFileName(this, "Export text dump", "dump.txt",
                                              "Text file (*.txt)");
    if (fn.isEmpty()) return;
    QFile f(fn);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
    { QMessageBox::critical(this, "Translate Mode", "Could not open the file for writing."); return; }
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << "# Translate Mode dump. Format per line: ORIGINAL<TAB>TRANSLATION\n";
    out << "# Edit the TRANSLATION side and re-import with \"Import .txt\".\n";
    for (const auto& e : Entries)
    {
        QString o = e.Original; o.replace('\t', ' ').replace('\n', ' ');
        QString t = e.Translation; t.replace('\t', ' ').replace('\n', ' ');
        out << o << '\t' << t << '\n';
    }
    f.close();
    lblStatus->setText(QString("Exported %1 lines.").arg((int)Entries.size()));
}

void TranslateWindow::onImportTxt()
{
    QString fn = QFileDialog::getOpenFileName(this, "Import text dump", "", "Text file (*.txt)");
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
        QString orig = line.left(tab);
        QString trans = line.mid(tab + 1);
        if (trans.isEmpty()) continue;
        for (auto& e : Entries)
            if (e.Original == orig) { e.Translation = trans; applied++; break; }
    }
    f.close();
    rebuildTable();
    lblStatus->setText(QString("Imported %1 translation(s).").arg(applied));
}

// ---------------------------------------------------------------------------
// guide + inspect
// ---------------------------------------------------------------------------

void TranslateWindow::onGuide()
{
    QMessageBox::information(this, "Translate Mode - Guide",
        "How to translate a game\n"
        "------------------------\n"
        "1) Boot the game and reach a screen with text.\n"
        "2) Try each Encoding (ASCII / Shift-JIS / UTF-16LE / UTF-8) and press\n"
        "   \"Scan RAM\" and \"Scan ROM (full)\". See in which one the real text\n"
        "   appears in the list.\n"
        "3) With \"Highlight on-screen\" checked, the strings that just appeared\n"
        "   are highlighted and scrolled to - that is what is on screen now.\n"
        "4) If nothing readable appears in any encoding, the game uses its own\n"
        "   font encoding. Use \"Relative search\" with a short on-screen word to\n"
        "   find where it is stored, work out the byte-to-letter mapping, write a\n"
        "   .tbl file (lines like 41=A) and load it with \"Load table...\".\n"
        "5) Double-click the Translation column and type your translation.\n"
        "6) \"Apply to RAM (live)\" previews it instantly in the running game.\n"
        "7) \"Export .txt\" to translate in bulk in a text editor, then\n"
        "   \"Import .txt\" back. \"Save/Load project\" keeps your work.\n"
        "8) \"Create patched ROM...\" writes the translated .nds.\n\n"
        "Inspect (click screen) is EXPERIMENTAL: arm it, then click text on the\n"
        "bottom (touch) screen to read the background tiles under the cursor.");
}

void TranslateWindow::onToggleInspect(bool on)
{
    inspectArmed = on;
    lblStatus->setText(on
        ? "Inspect armed: click the text on the bottom (touch) screen of the emulator."
        : "Inspect off.");
}

// Read a horizontal run of text-mode background tiles under a DS pixel.
// Best-effort / experimental: fully bounds-checked so it can never crash even
// if the game does not use the assumed layout.
static QString readBgRow(GPU2D& eng, bool engineA, int bg, int dsx, int dsy,
                         const CharTable& tbl, QString& tilesOut)
{
    u16 bgcnt = eng.BGCnt[bg];
    u32 dispcnt = eng.DispCnt;

    u32 charBaseOfs   = engineA ? (((dispcnt >> 24) & 0x7) * 0x10000u) : 0u;
    u32 screenBaseOfs = engineA ? (((dispcnt >> 27) & 0x7) * 0x10000u) : 0u;
    (void)charBaseOfs;

    u32 mapBase = (((bgcnt >> 8) & 0x1F) * 0x800u) + screenBaseOfs;
    int scsize = (bgcnt >> 14) & 0x3;
    int mapW = (scsize == 1 || scsize == 3) ? 512 : 256;
    int mapH = (scsize == 2 || scsize == 3) ? 512 : 256;

    u8* vram = nullptr; u32 mask = 0;
    eng.GetBGVRAM(vram, mask);
    if (!vram || mask == 0) return QString();

    int sx = eng.BGXPos[bg];
    int sy = eng.BGYPos[bg];
    int py = (dsy + sy) & (mapH - 1);
    int ty = py / 8;

    QString decoded;
    tilesOut.clear();
    const int maxTiles = 40;
    int startCol = ((dsx + sx) & (mapW - 1)) / 8;

    for (int c = 0; c < maxTiles; c++)
    {
        int ctx = (startCol + c) & ((mapW / 8) - 1);
        int scx = ctx >> 5;
        int scy = ty >> 5;
        int scIndex = 0;
        if (mapW == 512 && mapH == 512) scIndex = scx + scy * 2;
        else if (mapW == 512)           scIndex = scx;
        else if (mapH == 512)           scIndex = scy;

        u32 addr = mapBase + scIndex * 0x800u + (((ty & 31) * 32 + (ctx & 31)) * 2);
        u16 ent = (u16)(vram[addr & mask] | (vram[(addr + 1) & mask] << 8));
        int tile = ent & 0x3FF;

        tilesOut += QString("%1 ").arg(tile, 3, 16, QChar('0'));

        // If a table is loaded and maps this tile code (<=0xFF) treat it as text.
        if (tbl.isLoaded())
        {
            auto it = tbl.one.find((u8)(tile & 0xFF));
            if (it != tbl.one.end()) decoded += it.value();
            else decoded += (tile == 0 ? QChar(' ') : QChar(char16_t(0x00B7)));
        }
    }
    return decoded;
}

void TranslateWindow::screenPick(int dsx, int dsy)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return;

    GPU2D* engines[2] = { &nds->GPU.GPU2D_A, &nds->GPU.GPU2D_B };
    const char* engName[2] = { "A(main)", "B(sub)" };

    QString report = QString("Clicked bottom screen at DS pixel (%1, %2).\n\n").arg(dsx).arg(dsy);
    QString bestDecoded;

    for (int e = 0; e < 2; e++)
    {
        for (int bg = 0; bg < 2; bg++)
        {
            QString tiles;
            QString dec = readBgRow(*engines[e], e == 0, bg, dsx, dsy, Table, tiles);
            report += QString("Engine %1 BG%2 tiles: %3\n").arg(engName[e]).arg(bg).arg(tiles.trimmed());
            if (Table.isLoaded() && !dec.trimmed().isEmpty())
            {
                report += QString("   decoded: %1\n").arg(dec);
                if (dec.trimmed().size() > bestDecoded.trimmed().size())
                    bestDecoded = dec;
            }
        }
    }

    if (!Table.isLoaded())
        report += "\nNo table loaded: showing raw tile indices (hex). Build a tile "
                  ".tbl (index=char) and load it to decode these into text.";

    QMessageBox box(this);
    box.setWindowTitle("Inspect - tiles under cursor");
    box.setIcon(QMessageBox::Information);
    box.setText(report);
    QPushButton* addBtn = nullptr;
    if (!bestDecoded.trimmed().isEmpty())
        addBtn = box.addButton("Add decoded to list", QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Close);
    box.exec();

    if (addBtn && box.clickedButton() == addBtn)
    {
        QString txt = bestDecoded.trimmed();
        QString key = QString::number(ENC_TABLE) + ":" + txt;
        if (!KeyToIndex.contains(key))
        {
            TranslateEntry en;
            en.Address = 0; en.RomOffset = -1; en.Source = SRC_RAM;
            en.Original = txt; en.Encoding = ENC_TABLE;
            KeyToIndex.insert(key, (int)Entries.size());
            Entries.push_back(en);
            rebuildTable();
        }
    }
}

// ---------------------------------------------------------------------------
// project save / load (JSON)
// ---------------------------------------------------------------------------

void TranslateWindow::onSaveProject()
{
    QString fn = QFileDialog::getSaveFileName(this, "Save translation project", "translation.json",
                                              "Translation project (*.json)");
    if (fn.isEmpty()) return;

    QJsonArray arr;
    for (const auto& e : Entries)
    {
        QJsonObject o;
        o["address"] = (double)e.Address;
        o["rom_offset"] = (double)e.RomOffset;
        o["source"] = e.Source;
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
    { QMessageBox::critical(this, "Translate Mode", "Could not open the file for writing."); return; }
    f.write(QJsonDocument(root).toJson());
    f.close();
    lblStatus->setText(QString("Project saved: %1 entries.").arg((int)Entries.size()));
}

void TranslateWindow::onLoadProject()
{
    QString fn = QFileDialog::getOpenFileName(this, "Load translation project", "",
                                              "Translation project (*.json)");
    if (fn.isEmpty()) return;

    QFile f(fn);
    if (!f.open(QIODevice::ReadOnly))
    { QMessageBox::critical(this, "Translate Mode", "Could not open the file."); return; }
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) { QMessageBox::critical(this, "Translate Mode", "Invalid project file."); return; }

    QJsonArray arr = doc.object()["entries"].toArray();
    int loaded = 0;
    for (const auto& v : arr)
    {
        QJsonObject o = v.toObject();
        int enc = o["encoding"].toInt();
        QString orig = o["original"].toString();
        QString key = QString::number(enc) + ":" + orig;

        auto it = KeyToIndex.find(key);
        if (it != KeyToIndex.end())
        {
            Entries[it.value()].Translation = o["translation"].toString();
        }
        else
        {
            TranslateEntry e;
            e.Address = (u32)o["address"].toDouble();
            e.RomOffset = (qint64)o["rom_offset"].toDouble();
            e.Source = o["source"].toInt();
            e.Encoding = enc;
            e.RawBytes = QByteArray::fromHex(o["raw"].toString().toLatin1());
            e.Original = orig;
            e.Translation = o["translation"].toString();
            KeyToIndex.insert(key, (int)Entries.size());
            Entries.push_back(e);
        }
        loaded++;
    }
    rebuildTable();
    lblStatus->setText(QString("Project loaded: %1 entries.").arg(loaded));
}
