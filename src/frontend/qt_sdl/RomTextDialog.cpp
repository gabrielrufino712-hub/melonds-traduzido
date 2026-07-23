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

#include "RomTextDialog.h"
#include "TranslateSJIS.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <cstring>
#include <tuple>

#include "main.h"
#include "EmuInstance.h"
#include "NDS.h"
#include "NDSCart.h"

using namespace melonDS;

// ---- cp932 helpers (reuse the compiled TranslateSJIS.h table) ----

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

// Decode game bytes to a readable, editable string. 0x0A -> "\n" (literal),
// control bytes shown as {XX} so they can be preserved by the user if needed.
static QString sjisDecodeReadable(const QByteArray& raw)
{
    QString out;
    int n = raw.size();
    for (int i = 0; i < n; )
    {
        u8 b = (u8)raw[i];
        if (b == 0x0A) { out += "\\n"; i++; continue; }
        if (b >= 0x20 && b <= 0x7E) { out += QChar((char16_t)b); i++; continue; }
        if (b >= 0xA1 && b <= 0xDF) { out += QChar((char16_t)(0xFF61 + (b - 0xA1))); i++; continue; }
        if (((b >= 0x81 && b <= 0x9F) || (b >= 0xE0 && b <= 0xFC)) && i + 1 < n)
        {
            u8 t = (u8)raw[i + 1];
            quint16 key = (quint16)((b << 8) | t);
            auto it = sjisFwd().find(key);
            if (it != sjisFwd().end()) { out += QChar((char16_t)it.value()); i += 2; continue; }
        }
        out += QString("{%1}").arg((int)b, 2, 16, QChar('0'));
        i++;
    }
    return out;
}

// Encode a translated string back to game bytes.
static QByteArray sjisEncode(const QString& s)
{
    QByteArray out;
    int i = 0, n = s.size();
    while (i < n)
    {
        QChar c = s.at(i);
        // literal "\n" -> newline byte
        if (c == '\\' && i + 1 < n && s.at(i + 1) == 'n') { out.append((char)0x0A); i += 2; continue; }
        // {XX} control byte marker
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
struct FS
{
    const u8* d; u32 len;
    u32 fntOff, fatOff, fatSize;
    u16 rd16(u32 o) const { return (u32)d[o] | ((u32)d[o+1] << 8); }
    u32 rd32(u32 o) const { return (u32)d[o] | ((u32)d[o+1] << 8) | ((u32)d[o+2] << 16) | ((u32)d[o+3] << 24); }
};

void fsEntries(const FS& fs, u16 did, const QString& path,
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
            u16 did2 = fs.rd16(p); p += 2;
            fsEntries(fs, did2, path + name + "/", out);
        }
        else
        {
            u32 fa = fs.fatOff + fid * 8;
            if (fa + 8 <= fs.len)
            {
                u32 s = fs.rd32(fa), e = fs.rd32(fa + 4);
                out.push_back(std::make_tuple(path + name, s, e));
            }
            fid++;
        }
    }
}
} // namespace

// Parse a pointer-table SJIS file. Returns strings + raw bytes, or false.
static bool parsePtrText(const u8* b, u32 size, QVector<QString>& outStr, QVector<QByteArray>& outRaw)
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
        u32 p = base + offs[i];
        u32 end = p;
        while (end < size && b[end] != 0x00) end++;
        QByteArray raw((const char*)(b + p), (int)(end - p));
        // quick validity check: does it decode without too many control bytes?
        int bad = 0;
        for (u8 c : raw) if (c < 0x20 && c != 0x0A) bad++;
        if (bad == 0) ok++;
        outRaw.append(raw);
        outStr.append(sjisDecodeReadable(raw));
    }
    if (ok < (int)((count * 7) / 10)) return false;
    return true;
}

// ---------------------------------------------------------------------------

enum { COL_FILE, COL_IDX, COL_ORIG, COL_TRANS, COL_COUNT };

RomTextDialog* RomTextDialog::currentDlg = nullptr;

RomTextDialog::RomTextDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("ROM Text Translator");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1000, 620);

    emuInstance = ((MainWindow*)parent)->getEmuInstance();

    auto* root = new QVBoxLayout(this);

    auto* bar = new QHBoxLayout();
    auto* btnScan = new QPushButton("Scan ROM text", this);
    connect(btnScan, &QPushButton::clicked, this, &RomTextDialog::onScan);
    bar->addWidget(btnScan);
    bar->addWidget(new QLabel("Filter:", this));
    txtFilter = new QLineEdit(this);
    txtFilter->setPlaceholderText("filter by text or file...");
    connect(txtFilter, &QLineEdit::textChanged, this, &RomTextDialog::onFilterChanged);
    bar->addWidget(txtFilter, 1);
    root->addLayout(bar);

    table = new QTableWidget(this);
    table->setColumnCount(COL_COUNT);
    table->setHorizontalHeaderLabels({ "File", "#", "Original (JP)", "Translation" });
    table->horizontalHeader()->setSectionResizeMode(COL_ORIG, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(COL_TRANS, QHeaderView::Stretch);
    table->setColumnWidth(COL_FILE, 260);
    table->setColumnWidth(COL_IDX, 40);
    table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
    connect(table, &QTableWidget::cellChanged, this, &RomTextDialog::onCellChanged);
    root->addWidget(table);

    auto* actions = new QHBoxLayout();
    auto* btnLoad = new QPushButton("Load project", this);
    connect(btnLoad, &QPushButton::clicked, this, &RomTextDialog::onLoadProject);
    actions->addWidget(btnLoad);
    auto* btnSave = new QPushButton("Save project", this);
    connect(btnSave, &QPushButton::clicked, this, &RomTextDialog::onSaveProject);
    actions->addWidget(btnSave);
    actions->addStretch();
    auto* btnRom = new QPushButton("Create translated ROM...", this);
    btnRom->setStyleSheet("font-weight: bold;");
    connect(btnRom, &QPushButton::clicked, this, &RomTextDialog::onCreateRom);
    actions->addWidget(btnRom);
    root->addLayout(actions);

    lblStatus = new QLabel("Load a game, then press \"Scan ROM text\".", this);
    root->addWidget(lblStatus);
}

RomTextDialog::~RomTextDialog() { RomTextDialog::closeDlg(); }

const u8* RomTextDialog::romData(u32& len)
{
    NDS* nds = emuInstance ? emuInstance->getNDS() : nullptr;
    if (!nds) return nullptr;
    NDSCart::CartCommon* cart = nds->GetNDSCart();
    if (!cart) return nullptr;
    len = cart->GetROMLength();
    return cart->GetROM();
}

void RomTextDialog::onScan()
{
    u32 len = 0;
    const u8* d = romData(len);
    if (!d || len < 0x200) { QMessageBox::warning(this, "ROM Text", "No cartridge ROM loaded."); return; }

    FS fs; fs.d = d; fs.len = len;
    fs.fntOff = fs.rd32(0x40); fs.fatOff = fs.rd32(0x48); fs.fatSize = fs.rd32(0x4C);
    if (fs.fntOff + 8 > len || fs.fatOff + 8 > len)
    { QMessageBox::warning(this, "ROM Text", "Could not read the ROM filesystem."); return; }

    std::vector<std::tuple<QString,u32,u32>> files;
    fsEntries(fs, 0xF000, "", files);

    Files.clear();
    int totalStrings = 0;
    for (auto& f : files)
    {
        QString path = std::get<0>(f);
        u32 s = std::get<1>(f), e = std::get<2>(f);
        if (e <= s || e > len) continue;
        QVector<QString> strs; QVector<QByteArray> raws;
        if (!parsePtrText(d + s, e - s, strs, raws)) continue;
        // require at least one string with real Japanese
        bool hasJp = false;
        for (const QString& x : strs)
            for (QChar c : x) if (c.unicode() > 0x2000) { hasJp = true; break; }
        RomTextFile rt;
        rt.path = path; rt.start = s; rt.end = e;
        rt.originals = strs; rt.raws = raws;
        rt.translations = QVector<QString>(strs.size());
        Files.push_back(rt);
        totalStrings += strs.size();
        (void)hasJp;
    }

    rebuildTable();
    lblStatus->setText(QString("Found %1 text file(s), %2 string(s). Type translations and press \"Create translated ROM\".")
                       .arg((int)Files.size()).arg(totalStrings));
}

void RomTextDialog::rebuildTable()
{
    updatingTable = true;
    Rows.clear();
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

void RomTextDialog::onFilterChanged(const QString&) { rebuildTable(); }

void RomTextDialog::onCellChanged(int row, int col)
{
    if (updatingTable || col != COL_TRANS) return;
    if (row < 0 || row >= Rows.size()) return;
    int fi = Rows[row].first, si = Rows[row].second;
    Files[fi].translations[si] = table->item(row, col)->text();
}

void RomTextDialog::onCreateRom()
{
    u32 len = 0;
    const u8* d = romData(len);
    if (!d || len == 0) { QMessageBox::warning(this, "ROM Text", "No cartridge ROM loaded."); return; }

    QByteArray rom((const char*)d, (int)len);

    int patchedFiles = 0, patchedStrings = 0, tooBig = 0;
    QStringList overflow;

    for (const RomTextFile& f : Files)
    {
        bool any = false;
        for (const QString& t : f.translations) if (!t.isEmpty()) { any = true; break; }
        if (!any) continue;

        int count = f.originals.size();
        // rebuild: count, offset table, string blob (translated or original raw)
        QByteArray blob;
        QVector<int> offs;
        for (int i = 0; i < count; i++)
        {
            offs.append(blob.size());
            QByteArray sb = f.translations[i].isEmpty() ? f.raws[i] : sjisEncode(f.translations[i]);
            blob.append(sb);
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
            if (overflow.size() < 12)
                overflow << QString("%1 (+%2 bytes)").arg(f.path).arg((int)rebuilt.size() - (int)slot);
            continue;
        }
        // write in place, pad the rest of the slot with zeros
        memcpy(rom.data() + f.start, rebuilt.constData(), rebuilt.size());
        for (u32 k = f.start + rebuilt.size(); k < f.end; k++) rom[(int)k] = 0;
        patchedFiles++;
        for (const QString& t : f.translations) if (!t.isEmpty()) patchedStrings++;
    }

    if (patchedFiles == 0 && tooBig == 0)
    { QMessageBox::information(this, "ROM Text", "No translations entered yet."); return; }

    QString fn = QFileDialog::getSaveFileName(this, "Save translated ROM", "translated.nds",
                                              "Nintendo DS ROM (*.nds)");
    if (fn.isEmpty()) return;
    QFile out(fn);
    if (!out.open(QIODevice::WriteOnly))
    { QMessageBox::critical(this, "ROM Text", "Could not write the output file."); return; }
    out.write(rom);
    out.close();

    QString msg = QString("Translated ROM written to:\n%1\n\nFiles patched: %2\nStrings patched: %3")
                  .arg(fn).arg(patchedFiles).arg(patchedStrings);
    if (tooBig)
        msg += QString("\n\n%1 file(s) did NOT fit (translation longer than the space in the ROM). "
                       "Shorten those translations, or ROM expansion (repack) is needed:\n - %2")
               .arg(tooBig).arg(overflow.join("\n - "));
    QMessageBox::information(this, "ROM Text", msg);
    lblStatus->setText(QString("Saved translated ROM (%1 files, %2 strings, %3 didn't fit).")
                       .arg(patchedFiles).arg(patchedStrings).arg(tooBig));
}

// ---- project save / load ----

void RomTextDialog::onSaveProject()
{
    QString fn = QFileDialog::getSaveFileName(this, "Save project", "romtext.json", "Project (*.json)");
    if (fn.isEmpty()) return;
    QJsonArray arr;
    for (const RomTextFile& f : Files)
    {
        QJsonObject o;
        o["file"] = f.path;
        QJsonArray os, ts;
        for (const QString& s : f.originals) os.append(s);
        for (const QString& s : f.translations) ts.append(s);
        o["originals"] = os; o["translations"] = ts;
        arr.append(o);
    }
    QJsonObject root; root["tool"] = "melonDS ROM Text"; root["files"] = arr;
    QFile out(fn);
    if (!out.open(QIODevice::WriteOnly)) { QMessageBox::critical(this, "ROM Text", "Cannot write file."); return; }
    out.write(QJsonDocument(root).toJson());
    out.close();
    lblStatus->setText("Project saved.");
}

void RomTextDialog::onLoadProject()
{
    QString fn = QFileDialog::getOpenFileName(this, "Load project", "", "Project (*.json)");
    if (fn.isEmpty()) return;
    QFile in(fn);
    if (!in.open(QIODevice::ReadOnly)) { QMessageBox::critical(this, "ROM Text", "Cannot open file."); return; }
    QJsonDocument doc = QJsonDocument::fromJson(in.readAll()); in.close();
    if (!doc.isObject()) { QMessageBox::critical(this, "ROM Text", "Invalid project."); return; }
    QJsonArray arr = doc.object()["files"].toArray();
    // merge translations into scanned files by path
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
    lblStatus->setText("Project loaded (translations merged into scanned files).");
}
