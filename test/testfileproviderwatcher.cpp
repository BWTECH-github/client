/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "fileproviderwatcher_mac.h"

using namespace OCC;

class TestFileProviderWatcher : public QObject
{
    Q_OBJECT

    static QSet<QString> received(const QSignalSpy &spy)
    {
        QSet<QString> all;
        for (const auto &arguments : spy)
            all.unite(arguments.at(0).value<QSet<QString>>());
        return all;
    }

private Q_SLOTS:
    // relativ zum Ordner, auch wenn er hinter einer Verknüpfung liegt (QTemporaryDir: /var -> /private/var), und in NFC
    void testRelativePaths()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("Ordner")));
        FileProviderWatcher watcher(dir.path(), nullptr);
        QSignalSpy spy(&watcher, &FileProviderWatcher::changed);
        QTest::qWait(500);

        QFile file(dir.filePath(QStringLiteral("Ordner/Änderung.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();

        QTRY_VERIFY_WITH_TIMEOUT(received(spy).contains(QStringLiteral("Ordner/Änderung.txt")), 10000);
        QVERIFY(!received(spy).contains(QString()));
    }

    void testMissingFolder()
    {
        FileProviderWatcher watcher(QStringLiteral("/gibt/es/nicht"), nullptr);
        QSignalSpy spy(&watcher, &FileProviderWatcher::changed);
        QTest::qWait(300);
        QVERIFY(spy.isEmpty());
    }

    // ein Rückruf darf das zerstörte Objekt nicht mehr erreichen
    void testDestroyWhileBusy()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int round = 0; round < 20; ++round) {
            auto *watcher = new FileProviderWatcher(dir.path(), nullptr);
            QFile file(dir.filePath(QStringLiteral("datei-%1.txt").arg(round)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("x");
            file.close();
            QTest::qWait(round % 4 * 100);
            delete watcher;
        }
        QTest::qWait(500);
    }
};

QTEST_GUILESS_MAIN(TestFileProviderWatcher)
#include "testfileproviderwatcher.moc"
