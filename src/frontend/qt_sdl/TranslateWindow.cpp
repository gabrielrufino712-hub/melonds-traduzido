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
#include <QLineEdit>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QColor>
#include <QBrush>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <vector>
#include <tuple>
#include <cstring>

#include "main.h"
#include "EmuInstance.h"
#include "EmuThread.h"
#include "NDS.h"
#include "NDSCart.h"

using namespace melonDS;

// ---- cp932 (Shift-JIS) helpers using the compiled TranslateSJIS.h table ----

static const QHash<quint16, uint>& sjisFwd()
{
    static QHash<quint16, uint> m;
    if (m.isEmpty())
        for (int i = 0; i < kSJISTableLen; i++) m.insert(kSJISTable[i].key, kSJISTable[i].cp);
    return m;
}
static const QHash<uint, quint16>& sjisRev()
{
    static QHash<uint, quint16> m;
    if (m.isEmpty())
        for (int i = 0; i < kSJISTableLen; i++)
            if (!m.contains(kSJISTable[i].cp)) m.insert(kSJISTable[i].cp, kSJISTable[i].key);
    return m;
}

// game bytes -> readable, editable text (0x0A -> "\n", control -> {XX})
static QString sjisDecode(const QByteArray& raw)
{
    QString out; int n = raw.size();
    for (int i = 0; i < n; )
    {
        u8 b = (u8)raw[i];
        if (b == 0x0A) { out += "\\n"; i++; continue; }
        if (b >= 0x20 && b <= 0x7E) { out += QChar((char16_t)b); i++; continue; }
        if (b >= 0xA1 && b <= 0xDF) { out += QChar((char16_t)(0xFF61 + (b - 0xA1))); i++; continue; }
        if (((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) && i + 1 < n)
        {
            u8 t = (u8)raw[i + 1];
            auto it = sjisFwd().find((quint16)((b << 8) | t));
            if (it != sjisFwd().end()) { out += QChar((char16_t)it.value()); i += 2; continue; }
        }
        out += QString("{%1}").arg((int)b, 2, 16, QChar('0'));
        i++;
    }
    return out;
}

static QByteArray sjisEncode(const QString& s)
{
    QByteArray out; int i = 0, n = s.size();
    while (i < n)
    {
        QChar c = s.at(i);
        if (c == '\\' && i + 1 < n && s.at(i + 1) == 'n') { out.append((char)0x0A); i += 2; continue; }
        if (c == '{' && i + 3 < n && s.at(i + 3) == '}')
        {
            bool ok = false; int v = s.mid(i + 1, 2).toInt(&ok, 16);
            if (ok) { out.append((char)v); i += 4; continue; }
        }
        uint cp = c.unicode();
        if (cp >= 0x20 && cp <= 0x7E) { out.append((char)cp); i++; continue; }
        if (cp >= 0xFF61 && cp <= 0xFF9F) { out.append((char)(0xA1 + (cp - 0xFF61))); i++; continue; }
        auto it = sjisRev().find(cp);
        if (it != sjisRev().end()) { quint16 k = it.value(); out.append((char)(k >> 8)); out.append((char)(k & 0xFF)); }
        else out.append('?');
        i++;
    }
    return out;
}

// ---- NDS filesystem parsing over a ROM buffer ----

namespace {
struct NdsFS
{
    const u8* d; u32 len; u32 fntOff, fatOff;
    u16 rd16(u32 o) const { return (u32)d[o] | ((u32)d[o+1] << 8); }
    u32 rd32(u32 o) const { return (u32)d[o] | ((u32)d[o+1]<<8) | ((u32)d[o+2]<<16) | ((u32)d[o+3]<<24); }
};
void fsWalk(const NdsFS& fs, u16 did, const QString& path,
            std::vector<std::tuple<QString,u32,u32>>& out)
{
    u32 ent = fs.fntOff + (did & 0xFFF) * 8;
    if (ent + 6 > fs.len) return;
    u32 sub = fs.fntOff + fs.rd32(ent);
    u16 fid = fs.rd16(ent + 4);
    u32 p = sub;
    while (p < fs.len)
    {
        u8 t = fs.d[p]; p++;
        if (t == 0) break;
        int l = t & 0x7F; bool isd = t & 0x80;
        if (p + l > fs.len) break;
        QString name = QString::fromLatin1((const char*)(fs.d + p), l); p += l;
        if (isd)
        {
            if (p + 2 > fs.len) break;
            u16 d2 = fs.rd16(p); p += 2;
            fsWalk(fs, d2, path + name + "/", out);
        }
        else
        {
            u32 fa = fs.fatOff + fid * 8;
            if (fa + 8 <= fs.len) out.push_back(std::make_tuple(path + name, fs.rd32(fa), fs.rd32(fa + 4)));
            fid++;
        }
    }
}
} // namespace

// pointer-table Shift-JIS text file: [u16 count][count u16 offsets][cp932 strings]
static bool parsePtr(const u8* b, u32 size, QVector<QString>& outStr, QVector<QByteArray>& outRaw)
{
    if (size < 6) return false;
    u32 count = (u32)b[0] | ((u32)b[1] << 8);
    if (count == 0 || count > 8000) return false;
    u32 base = 2 + 2 * count;
    if (base > size) return false;
    QVector<u32> offs;
    for (u32 i = 0; i < count; i++)
    {
        u32 o = (u32)b[2 + 2*i] | ((u32)b[2 + 2*i + 1] << 8);
        if (base + o > size) return false;
        offs.append(o);
    }
    int ok = 0;
    for (u32 i = 0; i < count; i++)
    {
        u32 p = base + offs[i]; u32 end = p;
        while (end < size && b[end] != 0x00) end++;
        QByteArray raw((const char*)(b + p), (int)(end - p));
        int bad = 0; for (u8 c : raw) if (c < 0x20 && c != 0x0A) bad++;
        if (bad == 0) ok++;
        outRaw.append(raw);
        outStr.append(sjisDecode(raw));
    }
    return ok >= (int)((count * 7) / 10);
}

// ---------------------------------------------------------------------------

enum { COL_FILE, COL_IDX, COL_ORIG, COL_TRANS, COL_COUNT };

TranslateWindow* TranslateWindow::currentDlg = nullptr;

TranslateWindow::TranslateWindow(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Translate Mode");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1040, 640);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();

    auto* root = new QVBoxLayout(this);

    auto* bar = new QHBoxLayout();
    btnPause = new QPushButton("Pause emulation", this);
    connect(btnPause, &QPushButton::clicked, this, &TranslateWindow::onTogglePause);
    bar->addWidget(btnPause);

    auto* btnScan = new QPushButton("Scan ROM text", this);
    connect(btnScan, &QPushButton::clicked, this, &TranslateWindow::onScan);
    bar->addWidget(btnScan);

    chkHighlight = new QCheckBox("Highlight active text", this);
    chkHighlight->setChecked(true);
    chkHighlight->setToolTip("Highlight the strings the game is currently using "
                             "(text loaded in RAM / on screen right now).");
    bar->addWidget(chkHighlight);

    chkFollow = new QCheckBox("Follow", this);
    chkFollow->setChecked(true);
    chkFollow->setToolTip("Auto-scroll to the active text.");
    bar->addWidget(chkFollow);

    bar->addWidget(new QLabel("Filter:", this));
    txtFilter = new QLineEdit(this);
    txtFilter->setPlaceholderText("filter by text or file...");
    connect(txtFilter, &QLineEdit::textChanged, this, &TranslateWindow::onFilterChanged);
    bar->addWidget(txtFilter, 1);
    root->addLayout(bar);

    table = new QTableWidget(this);
    table->setColumnCount(COL_COUNT);
    table->setHorizontalHeaderLabels({ "File", "#", "Original (JP)", "Translation" });
    table->horizontalHeader()->setSectionResizeMode(COL_ORIG, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(COL_TRANS, QHeaderView::Stretch);
    table->setColumnWidth(COL_FILE, 250);
    table->setColumnWidth(COL_IDX, 40);
    table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    connect(table, &QTableWidget::cellChanged, this, &TranslateWindow::onCellChanged);
    root->addWidget(table);

    auto* actions = new QHBoxLayout();
    auto* btnApply = new QPushButton("Apply to RAM (live)", this);
    btnApply->setToolTip("Write your translations into the game's RAM now, for an "
                         "instant on-screen preview of the active text.");
    connect(btnApply, &QPushButton::clicked, this, &TranslateWindow::onApplyLive);
    actions->addWidget(btnApply);
    auto* btnLoad = new QPushButton("Load project", this);
    connect(btnLoad, &QPushButton::clicked, this, &TranslateWindow::onLoadProject);
    actions->addWidget(btnLoad);
    auto* btnSave = new QPushButton("Save project", this);
    connect(btnSave, &QPushButton::clicked, this, &TranslateWindow::onSaveProject);
    actions->addWidget(btnSave);
    actions->addStretch();
    auto* btnRom = new QPushButton("Create translated ROM...", this);
    btnRom->setStyleSheet("font-weight: bold;");
    connect(btnRom, &QPushButton::clicked, this, &TranslateWindow::onCreateRom);
    actions->addWidget(btnRom);
    root->addLayout(actions);

    lblStatus = new QLabel("Load a game, then press \"Scan ROM text\".", this);
    root->addWidget(lblStatus);

    timer = new QTimer(this);
    timer->setInterval(400);
    connect(timer, &QTimer::timeout, this, &TranslateWindow::onTick);
    timer->start();

    refreshPauseButton();
}

TranslateWindow::~TranslateWindow() { TranslateWindow::closeDlg(); }

const u8* TranslateWindow::romData(u32& len)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return nullptr;
    NDSCart::CartCommon* cart = nds->GetNDSCart();
    if (!cart) return nullptr;
    len = cart->GetROMLength();
    return cart->GetROM();
}

// ---------------------------------------------------------------------------
// scan ROM text
// ---------------------------------------------------------------------------

void TranslateWindow::onScan()
{
    u32 len = 0; const u8* d = romData(len);
    if (!d || len < 0x200) { QMessageBox::warning(this, "Translate Mode", "No cartridge ROM loaded."); return; }

    NdsFS fs; fs.d = d; fs.len = len;
    fs.fntOff = fs.rd32(0x40); fs.fatOff = fs.rd32(0x48);
    if (fs.fntOff + 8 > len || fs.fatOff + 8 > len)
    { QMessageBox::warning(this, "Translate Mode", "Could not read the ROM filesystem."); return; }

    std::vector<std::tuple<QString,u32,u32>> files;
    fsWalk(fs, 0xF000, "", files);

    Files.clear();
    int total = 0;
    for (auto& f : files)
    {
        QString path = std::get<0>(f); u32 s = std::get<1>(f), e = std::get<2>(f);
        if (e <= s || e > len) continue;
        QVector<QString> strs; QVector<QByteArray> raws;
        if (!parsePtr(d + s, e - s, strs, raws)) continue;
        RomTextFile rt;
        rt.path = path; rt.start = s; rt.end = e;
        rt.originals = strs; rt.raws = raws;
        rt.translations = QVector<QString>(strs.size());
        rt.active = QVector<char>(strs.size(), 0);
        rt.ramAddr = QVector<u32>(strs.size(), 0);
        Files.push_back(rt);
        total += strs.size();
    }
    buildPrefixIndex();
    rebuildTable();
    lblStatus->setText(QString("Found %1 text file(s), %2 string(s). Play the game - text in use is highlighted.")
                       .arg((int)Files.size()).arg(total));
}

void TranslateWindow::buildPrefixIndex()
{
    Prefix.clear();
    for (int fi = 0; fi < (int)Files.size(); fi++)
        for (int si = 0; si < Files[fi].raws.size(); si++)
        {
            const QByteArray& r = Files[fi].raws[si];
            if (r.size() < 4) continue;
            quint32 key = (quint32)((u8)r[0]) | ((quint32)((u8)r[1]) << 8)
                        | ((quint32)((u8)r[2]) << 16) | ((quint32)((u8)r[3]) << 24);
            Prefix.insert(key, qMakePair(fi, si));
        }
}

// ---------------------------------------------------------------------------
// live: which strings are currently in RAM (being used by the game)
// ---------------------------------------------------------------------------

void TranslateWindow::scanActive()
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds || !nds->MainRAM || Files.empty() || Prefix.isEmpty()) return;
    EmuThread* thread = emuInstance->getEmuThread();
    if (thread && !thread->emuIsActive()) return;

    const u8* ram = nds->MainRAM;
    const u32 size = nds->MainRAMMask; // scan [0, size)
    // clear
    for (auto& f : Files) for (int i = 0; i < f.active.size(); i++) f.active[i] = 0;

    for (u32 i = 0; i + 4 < size; i++)
    {
        quint32 key = (quint32)ram[i] | ((quint32)ram[i+1] << 8)
                    | ((quint32)ram[i+2] << 16) | ((quint32)ram[i+3] << 24);
        auto range = Prefix.equal_range(key);
        for (auto it = range.first; it != range.second; ++it)
        {
            int fi = it->first, si = it->second;
            const QByteArray& r = Files[fi].raws[si];
            int rl = r.size();
            if (rl < 4 || i + (u32)rl > size) continue;
            if (memcmp(ram + i, r.constData(), rl) == 0)
            {
                Files[fi].active[si] = 1;
                Files[fi].ramAddr[si] = i;
            }
        }
    }
}

void TranslateWindow::onTick()
{
    refreshPauseButton();
    if (!chkHighlight->isChecked() || Files.empty()) return;

    scanActive();

    // recolour rows + optionally scroll to the first active row
    updatingTable = true;
    QColor hi(120, 220, 120);
    int firstActive = -1;
    for (int r = 0; r < Rows.size(); r++)
    {
        int fi = Rows[r].first, si = Rows[r].second;
        bool on = Files[fi].active[si];
        for (int c = 0; c < COL_COUNT; c++)
            if (auto* it = table->item(r, c)) it->setBackground(on ? QBrush(hi) : QBrush());
        if (on && firstActive < 0) firstActive = r;
    }
    updatingTable = false;

    if (firstActive >= 0 && chkFollow->isChecked())
        if (auto* it = table->item(firstActive, COL_ORIG))
            table->scrollToItem(it, QAbstractItemView::PositionAtCenter);
}

// ---------------------------------------------------------------------------
// table
// ---------------------------------------------------------------------------

void TranslateWindow::rebuildTable()
{
    updatingTable = true;
    Rows.clear();
    RowOf.clear();
    QString filter = txtFilter->text();
    for (int fi = 0; fi < (int)Files.size(); fi++)
        for (int si = 0; si < Files[fi].originals.size(); si++)
        {
            if (!filter.isEmpty() &&
                !Files[fi].originals[si].contains(filter, Qt::CaseInsensitive) &&
                !Files[fi].path.contains(filter, Qt::CaseInsensitive) &&
                !Files[fi].translations[si].contains(filter, Qt::CaseInsensitive))
                continue;
            Rows.append(qMakePair(fi, si));
        }
    table->setRowCount(Rows.size());
    for (int r = 0; r < Rows.size(); r++)
    {
        int fi = Rows[r].first, si = Rows[r].second;
        RowOf.insert(fi * 100000 + si, r);
        auto setCell = [&](int col, const QString& txt, bool ed)
        {
            QTableWidgetItem* it = new QTableWidgetItem(txt);
            if (ed) it->setFlags(it->flags() | Qt::ItemIsEditable);
            else    it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            table->setItem(r, col, it);
        };
        setCell(COL_FILE, Files[fi].path, false);
        setCell(COL_IDX, QString::number(si), false);
        setCell(COL_ORIG, Files[fi].originals[si], false);
        setCell(COL_TRANS, Files[fi].translations[si], true);
    }
    updatingTable = false;
}

void TranslateWindow::onFilterChanged(const QString&) { rebuildTable(); }

void TranslateWindow::onCellChanged(int row, int col)
{
    if (updatingTable || col != COL_TRANS) return;
    if (row < 0 || row >= Rows.size()) return;
    int fi = Rows[row].first, si = Rows[row].second;
    Files[fi].translations[si] = table->item(row, col)->text();
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
}

// ---------------------------------------------------------------------------
// apply to RAM (live preview)
// ---------------------------------------------------------------------------

void TranslateWindow::onApplyLive()
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds || !nds->MainRAM) { QMessageBox::warning(this, "Translate Mode", "No game is running."); return; }
    scanActive();
    const u32 mask = nds->MainRAMMask;
    int n = 0, trunc = 0;
    for (auto& f : Files)
        for (int i = 0; i < f.originals.size(); i++)
        {
            if (!f.active[i] || f.translations[i].isEmpty()) continue;
            QByteArray enc = sjisEncode(f.translations[i]);
            int room = f.raws[i].size();
            if (enc.size() > room) { enc = enc.left(room); trunc++; }
            u32 addr = f.ramAddr[i];
            for (int k = 0; k < enc.size(); k++) nds->MainRAM[(addr + k) & mask] = (u8)enc[k];
            for (int k = enc.size(); k < room; k++) nds->MainRAM[(addr + k) & mask] = 0x20; // pad spaces
            n++;
        }
    lblStatus->setText(QString("Applied %1 active translation(s) to RAM%2.")
                       .arg(n).arg(trunc ? QString(" (%1 truncated)").arg(trunc) : QString()));
}

// ---------------------------------------------------------------------------
// create translated ROM
// ---------------------------------------------------------------------------

void TranslateWindow::onCreateRom()
{
    u32 len = 0; const u8* d = romData(len);
    if (!d || len == 0) { QMessageBox::warning(this, "Translate Mode", "No cartridge ROM loaded."); return; }
    QByteArray rom((const char*)d, (int)len);

    int patchedFiles = 0, patchedStrings = 0, tooBig = 0;
    QStringList overflow;
    for (const RomTextFile& f : Files)
    {
        bool any = false;
        for (const QString& t : f.translations) if (!t.isEmpty()) { any = true; break; }
        if (!any) continue;

        int count = f.originals.size();
        QByteArray blob; QVector<int> offs;
        for (int i = 0; i < count; i++)
        {
            offs.append(blob.size());
            blob.append(f.translations[i].isEmpty() ? f.raws[i] : sjisEncode(f.translations[i]));
            blob.append('\0');
        }
        QByteArray rebuilt;
        rebuilt.append((char)(count & 0xFF)); rebuilt.append((char)((count >> 8) & 0xFF));
        bool offOverflow = false;
        for (int o : offs)
        {
            if (o > 0xFFFF) offOverflow = true;
            rebuilt.append((char)(o & 0xFF)); rebuilt.append((char)((o >> 8) & 0xFF));
        }
        rebuilt.append(blob);

        u32 slot = f.end - f.start;
        if (offOverflow || (u32)rebuilt.size() > slot)
        {
            tooBig++;
            if (overflow.size() < 12) overflow << QString("%1 (+%2 bytes)").arg(f.path).arg((int)rebuilt.size() - (int)slot);
            continue;
        }
        memcpy(rom.data() + f.start, rebuilt.constData(), rebuilt.size());
        for (u32 k = f.start + rebuilt.size(); k < f.end; k++) rom[(int)k] = 0;
        patchedFiles++;
        for (const QString& t : f.translations) if (!t.isEmpty()) patchedStrings++;
    }

    if (patchedFiles == 0 && tooBig == 0)
    { QMessageBox::information(this, "Translate Mode", "No translations entered yet."); return; }

    QString fn = QFileDialog::getSaveFileName(this, "Save translated ROM", "translated.nds", "Nintendo DS ROM (*.nds)");
    if (fn.isEmpty()) return;
    QFile out(fn);
    if (!out.open(QIODevice::WriteOnly)) { QMessageBox::critical(this, "Translate Mode", "Could not write the output file."); return; }
    out.write(rom); out.close();

    QString msg = QString("Translated ROM written to:\n%1\n\nFiles patched: %2\nStrings patched: %3")
                  .arg(fn).arg(patchedFiles).arg(patchedStrings);
    if (tooBig)
        msg += QString("\n\n%1 file(s) did NOT fit (translation longer than original space):\n - %2")
               .arg(tooBig).arg(overflow.join("\n - "));
    QMessageBox::information(this, "Translate Mode", msg);
    lblStatus->setText(QString("Saved translated ROM (%1 files, %2 strings, %3 didn't fit).")
                       .arg(patchedFiles).arg(patchedStrings).arg(tooBig));
}

// ---------------------------------------------------------------------------
// project save / load
// ---------------------------------------------------------------------------

void TranslateWindow::onSaveProject()
{
    QString fn = QFileDialog::getSaveFileName(this, "Save project", "translation.json", "Project (*.json)");
    if (fn.isEmpty()) return;
    QJsonArray arr;
    for (const RomTextFile& f : Files)
    {
        QJsonObject o; o["file"] = f.path;
        QJsonArray os, ts;
        for (const QString& s : f.originals) os.append(s);
        for (const QString& s : f.translations) ts.append(s);
        o["originals"] = os; o["translations"] = ts; arr.append(o);
    }
    QJsonObject root; root["tool"] = "melonDS Translate Mode"; root["files"] = arr;
    QFile out(fn);
    if (!out.open(QIODevice::WriteOnly)) { QMessageBox::critical(this, "Translate Mode", "Cannot write file."); return; }
    out.write(QJsonDocument(root).toJson()); out.close();
    lblStatus->setText("Project saved.");
}

void TranslateWindow::onLoadProject()
{
    QString fn = QFileDialog::getOpenFileName(this, "Load project", "", "Project (*.json)");
    if (fn.isEmpty()) return;
    QFile in(fn);
    if (!in.open(QIODevice::ReadOnly)) { QMessageBox::critical(this, "Translate Mode", "Cannot open file."); return; }
    QJsonDocument doc = QJsonDocument::fromJson(in.readAll()); in.close();
    if (!doc.isObject()) { QMessageBox::critical(this, "Translate Mode", "Invalid project."); return; }
    QJsonArray arr = doc.object()["files"].toArray();
    for (const auto& v : arr)
    {
        QJsonObject o = v.toObject();
        QString path = o["file"].toString();
        QJsonArray ts = o["translations"].toArray();
        for (RomTextFile& f : Files)
            if (f.path == path)
                for (int i = 0; i < ts.size() && i < f.translations.size(); i++)
                    f.translations[i] = ts[i].toString();
    }
    rebuildTable();
    lblStatus->setText("Project loaded.");
}
