/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTreeView>
#include <QtTest>

#include "FoldersGui/folderbrowsercontroller.h"
#include "FoldersGui/folderitem.h"

#include <algorithm>

using namespace OCC;

namespace {
using Entry = FolderBrowserController::ListingEntry;

QList<Entry> entries(const QString &pattern, int from, int to)
{
    QList<Entry> result;
    for (int i = from; i <= to; ++i) {
        const QString name = pattern.arg(i, 5, 10, QLatin1Char('0'));
        result.append({name, name, i});
    }
    return result;
}

QList<Entry> shuffled(QList<Entry> list)
{
    QRandomGenerator generator(4711);
    std::shuffle(list.begin(), list.end(), generator);
    return list;
}

int kind(const QStandardItem *item)
{
    return item->data(FolderItemRoles::ItemKindRole).toInt();
}

QStringList texts(const QStandardItem *parent, FolderTreeItemKind wanted)
{
    QStringList result;
    for (int row = 0; row < parent->rowCount(); ++row) {
        if (kind(parent->child(row, 0)) == static_cast<int>(wanted))
            result.append(parent->child(row, 0)->text());
    }
    return result;
}

QStringList names(const QList<Entry> &list)
{
    QStringList result;
    for (const Entry &entry : list)
        result.append(entry.name);
    return result;
}

// eine Ordnerzeile wie in der Ordneransicht (Inhalt und Knopfspalte) mit der Zeile „Wird geladen …“ nach dem Aufklappen
QStandardItem *addFolderRow(QStandardItemModel &model)
{
    auto *root = new QStandardItem(QStringLiteral("Sync-Ordner"));
    auto *placeholder = new QStandardItem(QStringLiteral("Loading …"));
    placeholder->setFlags(Qt::ItemIsEnabled);
    placeholder->setData(static_cast<int>(FolderTreeItemKind::BrowserPlaceholder), FolderItemRoles::ItemKindRole);
    placeholder->setData(-1, FolderItemRoles::SortPriorityRole);
    auto *filler = new QStandardItem;
    filler->setFlags(Qt::ItemIsEnabled);
    root->appendRow({placeholder, filler});
    model.appendRow({root, new QStandardItem});
    return root;
}
}

class TestFolderBrowser : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // 20.000 Dateien und 2.000 Ordner im aufgeklappten Ordner, die Ansicht sichtbar und mit offenem Editor wie in der
    // Ordneransicht (der Optionen-Knopf): unter einer Sekunde, in wenigen Stücken eingefügt, Dateizeilen gedeckelt
    void testLargeListing()
    {
        QStandardItemModel model;
        model.setColumnCount(2);
        QStandardItem *root = addFolderRow(model);

        QTreeView view;
        view.setModel(&model);
        view.resize(600, 400);
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        view.expand(root->index());
        view.openPersistentEditor(model.index(0, 1));

        const QList<Entry> folders = entries(QStringLiteral("Ordner %1"), 1, 2000);
        const QList<Entry> files = entries(QStringLiteral("Rechnung %1.pdf"), 1, 20000);

        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        QElapsedTimer timer;
        timer.start();
        FolderBrowserController::applyListing(root, shuffled(folders), shuffled(files), false, {});
        // Auslegen und Zeichnen gehören dazu: Erst danach reagiert das Fenster wieder
        QCoreApplication::processEvents();
        const qint64 elapsed = timer.elapsed();
        qInfo() << "2.000 Ordner und 20.000 Dateien eingefügt in" << elapsed << "ms";
        QVERIFY2(elapsed < 1000, qPrintable(QStringLiteral("%1 ms").arg(elapsed)));
        // Ordner, Dateien und „weitere Dateien“ in einem Stück, nicht eine Einfügung je Zeile
        QCOMPARE(inserted.count(), 1);

        const int shownFiles = FolderBrowserController::maxFileRows;
        QCOMPARE(root->rowCount(), 2000 + shownFiles + 1);
        QCOMPARE(texts(root, FolderTreeItemKind::BrowserFolder), names(folders));
        QCOMPARE(texts(root, FolderTreeItemKind::BrowserFile), names(files.mid(0, shownFiles)));
        QCOMPARE(texts(root, FolderTreeItemKind::BrowserPlaceholder), QStringList());

        QStandardItem *more = root->child(root->rowCount() - 1, 0);
        QCOMPARE(kind(more), static_cast<int>(FolderTreeItemKind::BrowserMoreFiles));
        QVERIFY2(more->text().contains(QString::number(20000 - shownFiles)), qPrintable(more->text()));

        // jede Ordnerzeile hat ihren Aufklapppfeil, jede neue Zeile ihre inaktive Knopfspalte
        for (int row : {0, 1999}) {
            QCOMPARE(root->child(row, 0)->rowCount(), 1);
            QCOMPARE(kind(root->child(row, 0)->child(0, 0)), static_cast<int>(FolderTreeItemKind::BrowserPlaceholder));
        }
        for (int row : {0, 1999, 2000, 2000 + shownFiles - 1, 2000 + shownFiles}) {
            QCOMPARE(model.flags(model.index(row, 1, root->index())), Qt::ItemIsEnabled);
        }
    }

    // Erneut laden: bleibende Zeilen behalten ihr Item (Haken, Inhalt), neue kommen an ihre Stelle, alte gehen
    void testRelistKeepsRows()
    {
        QStandardItemModel model;
        model.setColumnCount(2);
        QStandardItem *root = addFolderRow(model);

        QList<Entry> folders{{QStringLiteral("Alpha"), QStringLiteral("Alpha")}, {QStringLiteral("Beta"), QStringLiteral("Beta")},
            {QStringLiteral("Gamma"), QStringLiteral("Gamma")}};
        FolderBrowserController::applyListing(root, folders, entries(QStringLiteral("f-%1"), 1, 600), false, {});
        QCOMPARE(texts(root, FolderTreeItemKind::BrowserFolder), QStringList({QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Gamma")}));
        QStandardItem *alpha = root->child(0, 0);
        QStandardItem *beta = root->child(1, 0);
        QStandardItem *secondFile = root->child(3 + 1, 0);
        QCOMPARE(secondFile->text(), QStringLiteral("f-00002"));
        beta->setCheckState(Qt::Unchecked);
        // aufgeklappter Inhalt unter Beta
        beta->appendRow({new QStandardItem(QStringLiteral("Beta-Inhalt")), new QStandardItem});
        QVERIFY(root->child(root->rowCount() - 1, 0)->text().contains(QStringLiteral("100")));

        folders = {{QStringLiteral("Delta"), QStringLiteral("Delta")}, {QStringLiteral("Beta"), QStringLiteral("Beta")},
            {QStringLiteral("Alpha"), QStringLiteral("Alpha")}, {QStringLiteral("Aaa"), QStringLiteral("Aaa")}};
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        FolderBrowserController::applyListing(root, folders, shuffled(entries(QStringLiteral("f-%1"), 2, 601)), false, {});
        QCOMPARE(texts(root, FolderTreeItemKind::BrowserFolder),
            QStringList({QStringLiteral("Aaa"), QStringLiteral("Alpha"), QStringLiteral("Beta"), QStringLiteral("Delta")}));
        QCOMPARE(root->child(1, 0), alpha);
        QCOMPARE(root->child(2, 0), beta);
        QCOMPARE(beta->checkState(), Qt::Unchecked);
        QCOMPARE(beta->rowCount(), 2);
        QCOMPARE(texts(root, FolderTreeItemKind::BrowserFile), names(entries(QStringLiteral("f-%1"), 2, 501)));
        QCOMPARE(root->child(4, 0), secondFile);
        QVERIFY(root->child(root->rowCount() - 1, 0)->text().contains(QStringLiteral("100")));
        // „Aaa“ vor Alpha, „Delta“ nach Beta, „f-00501“ am Ende der Dateien
        QCOMPARE(inserted.count(), 3);

        // wenige Dateien: die Zeile „weitere Dateien“ verschwindet
        FolderBrowserController::applyListing(root, folders, entries(QStringLiteral("f-%1"), 1, 3), false, {});
        QCOMPARE(root->rowCount(), 4 + 3);
        QCOMPARE(texts(root, FolderTreeItemKind::BrowserMoreFiles), QStringList());
    }

    // kleine Ordner wie bisher: Fehlerzeile bleibt oben, Haken aus der Ausschlussliste, Größen, Platzhalter
    void testSmallListing()
    {
        QStandardItemModel model;
        model.setColumnCount(2);
        QStandardItem *root = addFolderRow(model);
        auto *error = new QStandardItem(QStringLiteral("Sync-Fehler"));
        error->setData(static_cast<int>(FolderTreeItemKind::Error), FolderItemRoles::ItemKindRole);
        error->setData(1, FolderItemRoles::SortPriorityRole);
        root->insertRow(0, {error, new QStandardItem});

        const QList<Entry> folders{{QStringLiteral("b"), QStringLiteral("b"), 2048}, {QStringLiteral("a"), QStringLiteral("a"), 1024}};
        const QList<Entry> files{{QStringLiteral("z.txt"), QStringLiteral("z.txt"), 10}, {QStringLiteral("y.txt"), QStringLiteral("y.txt"), 20}};
        FolderBrowserController::applyListing(root, folders, files, false, {QStringLiteral("b/")});

        QCOMPARE(root->rowCount(), 5);
        QCOMPARE(root->child(0, 0), error);
        QCOMPARE(texts(root, FolderTreeItemKind::BrowserFolder), QStringList({QStringLiteral("a"), QStringLiteral("b")}));
        QCOMPARE(texts(root, FolderTreeItemKind::BrowserFile), QStringList({QStringLiteral("y.txt"), QStringLiteral("z.txt")}));
        QCOMPARE(root->child(1, 0)->checkState(), Qt::Checked);
        QCOMPARE(root->child(2, 0)->checkState(), Qt::Unchecked);
        QVERIFY(!root->child(1, 0)->data(FolderItemRoles::DetailStringRole).toString().isEmpty());
        QVERIFY(!root->child(3, 0)->data(FolderItemRoles::DetailStringRole).toString().isEmpty());

        // virtuelle Dateien: ohne Haken
        FolderBrowserController::applyListing(root, {{QStringLiteral("c"), QStringLiteral("c")}}, {}, true, {});
        QCOMPARE(texts(root, FolderTreeItemKind::BrowserFolder), QStringList({QStringLiteral("c")}));
        QVERIFY(!(root->child(1, 0)->flags() & Qt::ItemIsUserCheckable));

        // leer: nur noch die Fehlerzeile und der Platzhalter mit dem Hinweis
        FolderBrowserController::applyListing(root, {}, {}, false, {});
        QCOMPARE(root->rowCount(), 2);
        QCOMPARE(root->child(0, 0), error);
        QCOMPARE(kind(root->child(1, 0)), static_cast<int>(FolderTreeItemKind::BrowserPlaceholder));
    }
};

QTEST_MAIN(TestFolderBrowser)
#include "testfolderbrowser.moc"
