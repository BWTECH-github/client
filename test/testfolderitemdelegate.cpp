/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// Fortschrittsbalken der Ordnerzeile: FolderItemDelegate zeichnet ihn unter der Statuszeile, solange
// FolderItemRoles::ProgressRole einen Wert hat. Läuft in der CI auf Windows, macOS und Linux.

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QStandardItemModel>
#include <QtTest>

#include "FoldersGui/folderitem.h"
#include "FoldersGui/folderitemdelegate.h"
#include "theme.h"

using namespace OCC;

namespace {

// Farbe des gefüllten Teils: die Akzentfarbe des Themes (hell oder dunkel) oder die Hervorhebung des Systems
QList<QColor> chunkColors()
{
    QList<QColor> colors;
    for (bool dark : {false, true}) {
        const QColor c = Theme::instance()->accentColor(dark);
        if (c.isValid())
            colors.append(c);
    }
    colors.append(QApplication::palette().color(QPalette::Highlight));
    return colors;
}

bool isChunkColor(QRgb pixel, const QList<QColor> &colors)
{
    for (const QColor &c : colors) {
        if (qAbs(qRed(pixel) - c.red()) + qAbs(qGreen(pixel) - c.green()) + qAbs(qBlue(pixel) - c.blue()) < 30)
            return true;
    }
    return false;
}

// zeichnet eine Ordnerzeile und zählt im unteren Rand die Spalten in der Farbe des gefüllten Teils
int chunkColumns(const QVariant &progress)
{
    QStandardItemModel model;
    auto *item = new QStandardItem;
    item->setData(QStringLiteral("Ordner"), Qt::DisplayRole);
    item->setData(QStringLiteral("Synchronisiere 1 MB von 2 MB (50 %)"), FolderItemRoles::StatusStringRole);
    if (progress.isValid())
        item->setData(progress, FolderItemRoles::ProgressRole);
    model.appendRow(item);
    const QModelIndex index = model.index(0, 0);

    FolderItemDelegate delegate(20);
    QStyleOptionViewItem option;
    option.font = QApplication::font();
    option.palette = QApplication::palette();
    option.state = QStyle::State_Enabled;
    option.rect = QRect(0, 0, 600, 100);
    option.rect.setHeight(delegate.sizeHint(option, index).height());

    QImage image(option.rect.size(), QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    delegate.paint(&painter, option, index);
    painter.end();

    const QList<QColor> colors = chunkColors();
    int columns = 0;
    // der Balken liegt im unteren Rand der Zeile, unter der Statuszeile
    for (int x = 0; x < image.width(); ++x) {
        for (int y = qMax(0, image.height() - 12); y < image.height(); ++y) {
            if (isChunkColor(image.pixel(x, y), colors)) {
                ++columns;
                break;
            }
        }
    }
    return columns;
}
}

class TestFolderItemDelegate : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // ohne Übertragung kein Balken
    void testNoBarWithoutProgress() { QCOMPARE(chunkColumns(QVariant()), 0); }

    // 0 % zeigt nur die Spur, noch keinen gefüllten Teil
    void testZeroPercentShowsOnlyTheTrack() { QCOMPARE(chunkColumns(0), 0); }

    // der gefüllte Teil wächst mit dem Fortschritt
    void testBarGrowsWithProgress()
    {
        const int half = chunkColumns(50);
        const int full = chunkColumns(100);
        QVERIFY2(half > 20, qPrintable(QStringLiteral("50 %: %1 Spalten").arg(half)));
        QVERIFY2(full > half + 20, qPrintable(QStringLiteral("50 %: %1, 100 %: %2 Spalten").arg(half).arg(full)));
    }
};

QTEST_MAIN(TestFolderItemDelegate)
#include "testfolderitemdelegate.moc"
