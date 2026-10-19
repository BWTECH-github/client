/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// owncloud.online: Löschschutz wie im 2.x-Client (upstream mit 4aea05663 entfernt), erweitert um „sehr viele“
// Dateien und um „Behalten“, das nur die gemeldeten Löschungen zurücknimmt.

#include "testutils/syncenginetestutils.h"
#include <syncengine.h>

#include <QSignalSpy>
#include <QtTest>

using namespace OCC;

namespace {

void changeAllFileId(FileInfo &info)
{
    info.fileId = generateFileId();
    if (!info.isDir)
        return;
    info.etag = generateEtag();
    for (auto it = info.children.begin(); it != info.children.end(); ++it) {
        changeAllFileId(*it);
    }
}

using RemoveAllSpy = QSignalSpy;

std::unique_ptr<RemoveAllSpy> removeAllSpy(FakeFolder &fakeFolder)
{
    return std::make_unique<RemoveAllSpy>(fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles);
}

// Struktur für die Fälle „sehr viele“: count Dateien in Big/ und zehn weitere in Small/
FileInfo bigTree(int count)
{
    FileInfo root;
    root.mkdir(QStringLiteral("Big"));
    for (int i = 0; i < count; ++i) {
        root.insert(QStringLiteral("Big/f%1").arg(i));
    }
    root.mkdir(QStringLiteral("Small"));
    for (int i = 0; i < 10; ++i) {
        root.insert(QStringLiteral("Small/s%1").arg(i));
    }
    return root;
}

}

class TestAllFilesDeleted : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testAllFilesDeletedKeep_data()
    {
        QTest::addColumn<bool>("deleteOnRemote");
        QTest::newRow("local") << false;
        QTest::newRow("remote") << true;
    }

    // Alle Dateien auf einer Seite gelöscht, „Behalten“: der nächste Lauf stellt alles wieder her
    void testAllFilesDeletedKeep()
    {
        QFETCH(bool, deleteOnRemote);
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);

        // Die Liste der abgewählten Ordner bleibt bei „Behalten“ erhalten
        const QSet<QString> selectiveSyncBlackList = {QStringLiteral("Q/")};
        fakeFolder.syncJournal()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, selectiveSyncBlackList);

        const auto initialState = fakeFolder.currentLocalState();
        auto spy = removeAllSpy(fakeFolder);

        auto &modifier = deleteOnRemote ? static_cast<FileModifier &>(fakeFolder.remoteModifier()) : fakeFolder.localModifier();
        const auto children = fakeFolder.currentRemoteState().children; // Kopie: remove() ändert die Liste
        for (auto it = children.cbegin(); it != children.cend(); ++it) {
            modifier.remove(it.key());
        }
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());
        QVERIFY(!fakeFolder.syncOnce());

        QCOMPARE(spy->count(), 1);
        const auto args = spy->first();
        QCOMPARE(args.at(0).value<SyncFileItem::Direction>(), deleteOnRemote ? SyncFileItem::Down : SyncFileItem::Up);
        QCOMPARE(args.at(1).toBool(), true);
        QCOMPARE(args.at(2).toInt(), 8);
        QCOMPARE(args.at(3).toInt(), 8);
        // nichts gelöscht: die andere Seite ist unverändert
        QCOMPARE(deleteOnRemote ? fakeFolder.currentLocalState() : fakeFolder.currentRemoteState(), initialState);

        fakeFolder.syncEngine()->keepFilesOfRemoveAllPrompt();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), initialState);
        QCOMPARE(fakeFolder.currentRemoteState(), initialState);
        QCOMPARE(spy->count(), 1);

        bool ok = true;
        QCOMPARE(fakeFolder.syncJournal()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, ok), selectiveSyncBlackList);
        QVERIFY(ok);
    }

    void testAllFilesDeletedDelete_data() { testAllFilesDeletedKeep_data(); }

    // Wie oben, aber „Löschen“: der nächste Lauf löscht ohne erneute Rückfrage
    void testAllFilesDeletedDelete()
    {
        QFETCH(bool, deleteOnRemote);
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        auto spy = removeAllSpy(fakeFolder);

        auto &modifier = deleteOnRemote ? static_cast<FileModifier &>(fakeFolder.remoteModifier()) : fakeFolder.localModifier();
        const auto children = fakeFolder.currentRemoteState().children;
        for (auto it = children.cbegin(); it != children.cend(); ++it) {
            modifier.remove(it.key());
        }
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());

        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 1);

        fakeFolder.syncEngine()->allowRemoveAllFilesOnce();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 0);

        // ein weiterer Lauf hat nichts mehr zu tun und fragt nicht
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 1);
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 0);
    }

    // Die Erlaubnis gilt nur für einen Lauf: danach greift der Schutz wieder
    void testAllowOnceIsConsumed()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        auto spy = removeAllSpy(fakeFolder);
        // Stand vorher festhalten: ein neu erzeugter Baum trüge andere Zeitstempel
        const auto initialState = fakeFolder.currentLocalState();

        fakeFolder.syncEngine()->allowRemoveAllFilesOnce();
        QVERIFY(fakeFolder.syncOnce()); // verbraucht die Erlaubnis ohne Löschungen

        for (const auto &dir : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("S")}) {
            fakeFolder.remoteModifier().remove(dir);
        }
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 1);
        QCOMPARE(fakeFolder.currentLocalState(), initialState);
    }

    // Ohne Einschalten (Standard der Engine) fragt nichts
    void testDisabledByDefault()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        QVERIFY(!fakeFolder.syncEngine()->promptRemoveAllFiles());
        auto spy = removeAllSpy(fakeFolder);

        for (const auto &dir : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("S")}) {
            fakeFolder.localModifier().remove(dir);
        }
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(spy->count(), 0);
        QCOMPARE(fakeFolder.currentRemoteState().children.count(), 0);
    }

    // Nur Metadaten geändert (z. B. Server-Aktualisierung): keine Rückfrage
    void testNotDeleteMetaDataChange()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        auto spy = removeAllSpy(fakeFolder);
        QVERIFY(fakeFolder.syncOnce());

        const auto children = fakeFolder.currentRemoteState().children;
        for (auto it = children.cbegin(); it != children.cend(); ++it) {
            fakeFolder.syncJournal()->avoidRenamesOnNextSync(it.key()); // löscht Datei-IDs und Inodes
        }
        fakeFolder.localModifier().remove(QStringLiteral("A/a1"));
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());
        auto expectedState = fakeFolder.currentLocalState();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), expectedState);
        QCOMPARE(fakeFolder.currentRemoteState(), expectedState);

        fakeFolder.remoteModifier().remove(QStringLiteral("B/b1"));
        changeAllFileId(fakeFolder.remoteModifier());
        expectedState = fakeFolder.currentRemoteState();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), expectedState);
        QCOMPARE(fakeFolder.currentRemoteState(), expectedState);
        QCOMPARE(spy->count(), 0);
    }

    // Server zurückgesetzt (alles ersetzt, eine Datei fehlt): Rückfrage Richtung Server
    void testResetServer()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        auto spy = removeAllSpy(fakeFolder);

        fakeFolder.localModifier().mkdir(QStringLiteral("Q"));
        fakeFolder.localModifier().insert(QStringLiteral("Q/q1"));
        fakeFolder.localModifier().appendByte(QStringLiteral("B/b1"));
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 0);

        fakeFolder.localModifier().appendByte(QStringLiteral("A/a1"));
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());

        fakeFolder.remoteModifier() = FileInfo::A12_B12_C12_S12();

        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 1);
        QCOMPARE(spy->first().at(0).value<SyncFileItem::Direction>(), SyncFileItem::Down);
        fakeFolder.syncEngine()->allowRemoveAllFilesOnce();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 1);
    }

    // Die einzige Datei umbenannt: nichts wird gelöscht
    void testSingleFileRenamed()
    {
        FakeFolder fakeFolder{FileInfo{}};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        auto spy = removeAllSpy(fakeFolder);

        fakeFolder.localModifier().insert(QStringLiteral("hello.txt"));
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.localModifier().rename(QStringLiteral("hello.txt"), QStringLiteral("goodbye.txt"));
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(spy->count(), 0);
    }

    // Alle Ordner abgewählt (Auswahl-Sync): gewollt, keine Rückfrage
    void testSelectiveSyncNoPopup()
    {
        const auto original = FileInfo::A12_B12_C12_S12();
        FakeFolder fakeFolder(original);
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        auto spy = removeAllSpy(fakeFolder);

        QVERIFY(fakeFolder.syncOnce());
        fakeFolder.syncJournal()->setSelectiveSyncList(
            SyncJournalDb::SelectiveSyncBlackList, {QStringLiteral("A/"), QStringLiteral("B/"), QStringLiteral("C/"), QStringLiteral("S/")});

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), FileInfo{});
        QCOMPARE(fakeFolder.currentRemoteState(), original);
        QCOMPARE(spy->count(), 0);
    }

    void testManyFilesDeleted_data() { testAllFilesDeletedKeep_data(); }

    // „Sehr viele“: ein großer Ordner verschwindet, ein kleiner bleibt (Laufwerk/Speicher teilweise weg)
    void testManyFilesDeleted()
    {
        QFETCH(bool, deleteOnRemote);
        FakeFolder fakeFolder{bigTree(300)};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        const auto initialState = fakeFolder.currentLocalState();
        auto spy = removeAllSpy(fakeFolder);

        if (deleteOnRemote) {
            fakeFolder.remoteModifier().remove(QStringLiteral("Big"));
        } else {
            fakeFolder.localModifier().remove(QStringLiteral("Big"));
        }
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());
        QVERIFY(!fakeFolder.syncOnce());

        QCOMPARE(spy->count(), 1);
        const auto args = spy->first();
        QCOMPARE(args.at(0).value<SyncFileItem::Direction>(), deleteOnRemote ? SyncFileItem::Down : SyncFileItem::Up);
        QCOMPARE(args.at(1).toBool(), false);
        QCOMPARE(args.at(2).toInt(), 300);
        QCOMPARE(args.at(3).toInt(), 310);

        fakeFolder.syncEngine()->keepFilesOfRemoveAllPrompt();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), initialState);
        QCOMPARE(fakeFolder.currentRemoteState(), initialState);
        QCOMPARE(spy->count(), 1);
    }

    // Unterhalb der Schwellen (zu wenige oder weniger als die Hälfte): gewöhnliches Löschen ohne Rückfrage
    void testFewFilesDeletedNoPopup_data()
    {
        QTest::addColumn<int>("bigCount");
        QTest::addColumn<int>("smallExtra");
        // 150 von 160: viel, aber unter dem Mindestwert
        QTest::newRow("below minimum") << 150 << 0;
        // 250 von 510: knapp unter der Hälfte
        QTest::newRow("below half") << 250 << 250;
    }

    void testFewFilesDeletedNoPopup()
    {
        QFETCH(int, bigCount);
        QFETCH(int, smallExtra);
        auto tree = bigTree(bigCount);
        for (int i = 0; i < smallExtra; ++i) {
            tree.insert(QStringLiteral("Small/x%1").arg(i));
        }
        FakeFolder fakeFolder{tree};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        auto spy = removeAllSpy(fakeFolder);

        fakeFolder.localModifier().remove(QStringLiteral("Big"));
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());
        QCOMPARE(spy->count(), 0);
        QVERIFY(!fakeFolder.currentRemoteState().find(QStringLiteral("Big")));
    }

    // „Behalten“ nimmt nur die gemeldeten Löschungen zurück: Dateien in einem sonst unveränderten Ordner kommen
    // zurück, eine gleichzeitig lokal geänderte Datei wird normal hochgeladen und nicht zum Konflikt
    void testKeepRestoresOnlyRemovedFiles()
    {
        FakeFolder fakeFolder{bigTree(300)};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        auto spy = removeAllSpy(fakeFolder);

        for (int i = 0; i < 260; ++i) {
            fakeFolder.localModifier().remove(QStringLiteral("Big/f%1").arg(i));
        }
        fakeFolder.localModifier().appendByte(QStringLiteral("Big/f299"));
        fakeFolder.localModifier().appendByte(QStringLiteral("Small/s0"));
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());
        auto modifiedState = fakeFolder.currentLocalState();

        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 1);
        QCOMPARE(spy->first().at(2).toInt(), 260);

        fakeFolder.syncEngine()->keepFilesOfRemoveAllPrompt();
        OperationCounter counter(fakeFolder);
        QVERIFY(fakeFolder.syncOnce());
        auto localState = fakeFolder.currentLocalState();
        QCOMPARE(localState, fakeFolder.currentRemoteState());
        QCOMPARE(localState.find(QStringLiteral("Big"))->children.count(), 300);
        // die 260 gelöschten kommen herunter, sonst nichts; gelöscht wird nichts
        QCOMPARE(counter.nGET, 260);
        QCOMPARE(counter.nDELETE, 0);
        // die geänderten Dateien sind hochgeladen, ohne Konfliktkopie
        for (const auto &path : {QStringLiteral("Big/f299"), QStringLiteral("Small/s0")}) {
            QCOMPARE(fakeFolder.currentRemoteState().find(path)->fileSize, modifiedState.find(path)->fileSize);
            QVERIFY(!findConflict(localState, path));
        }
        QCOMPARE(spy->count(), 1);
    }

    // Wie 2.x: eine lokal geänderte Datei, die hochgeladen wird, bleibt auf beiden Seiten erhalten. Ohne diese Regel hieße es
    // „alle Dateien gelöscht“, und „Behalten“ holte die mit Absicht gelöschten Dateien zurück.
    void testModifiedFileCountsAsRemaining()
    {
        FakeFolder fakeFolder{FileInfo{}};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        auto spy = removeAllSpy(fakeFolder);
        for (const auto &name : {QStringLiteral("a.txt"), QStringLiteral("b.txt"), QStringLiteral("c.txt")}) {
            fakeFolder.localModifier().insert(name);
        }
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());

        fakeFolder.localModifier().remove(QStringLiteral("a.txt"));
        fakeFolder.localModifier().remove(QStringLiteral("b.txt"));
        fakeFolder.localModifier().appendByte(QStringLiteral("c.txt"));
        QVERIFY(fakeFolder.applyLocalModificationsAndSync());

        QCOMPARE(spy->count(), 0);
        QCOMPARE(fakeFolder.currentRemoteState(), fakeFolder.currentLocalState());
        QCOMPARE(fakeFolder.currentRemoteState().children.count(), 1);
    }

    // „Behalten“ gilt für alle Löschungen des Laufs, auch für die der anderen Richtung: sonst löschte der nächste Lauf
    // diesen Teil ohne Rückfrage, weil er für sich allein unter den Schwellen bleibt
    void testKeepRestoresBothDirections()
    {
        FakeFolder fakeFolder{bigTree(300)};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        const auto initialState = fakeFolder.currentLocalState();
        auto spy = removeAllSpy(fakeFolder);

        for (int i = 0; i < 150; ++i) {
            fakeFolder.localModifier().remove(QStringLiteral("Big/f%1").arg(i));
            fakeFolder.remoteModifier().remove(QStringLiteral("Big/f%1").arg(i + 150));
        }
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 1);

        fakeFolder.syncEngine()->keepFilesOfRemoveAllPrompt();
        OperationCounter counter(fakeFolder);
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(counter.nDELETE, 0);
        QCOMPARE(fakeFolder.currentLocalState(), initialState);
        QCOMPARE(fakeFolder.currentRemoteState(), initialState);
        QCOMPARE(spy->count(), 1);
    }

    // „Löschen“ gilt nur für die gemeldeten Löschungen. Was bis zum nächsten Lauf dazukommt, prüft der Schutz wieder;
    // die schon bestätigten Löschungen bleiben bestätigt, bis ein Lauf sie ausführt.
    void testApprovalCoversOnlyPromptedRemovals()
    {
        FakeFolder fakeFolder{bigTree(300)};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        auto spy = removeAllSpy(fakeFolder);

        fakeFolder.localModifier().remove(QStringLiteral("Big"));
        QVERIFY(fakeFolder.applyLocalModificationsWithoutSync());
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 1);
        QCOMPARE(spy->at(0).at(0).value<SyncFileItem::Direction>(), SyncFileItem::Up);
        fakeFolder.syncEngine()->allowRemoveAllFilesOnce();

        // bevor der nächste Lauf beginnt, verschwindet der Rest auf dem Server
        fakeFolder.remoteModifier().remove(QStringLiteral("Small"));
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 2);
        QCOMPARE(spy->at(1).at(0).value<SyncFileItem::Direction>(), SyncFileItem::Down);
        QCOMPARE(spy->at(1).at(2).toInt(), 10);
        // nichts gelöscht: Big liegt noch auf dem Server, Small noch lokal
        QVERIFY(fakeFolder.currentRemoteState().find(QStringLiteral("Big")));
        QVERIFY(fakeFolder.currentLocalState().find(QStringLiteral("Small")));

        // auch das bestätigt: der nächste Lauf führt beides aus
        fakeFolder.syncEngine()->allowRemoveAllFilesOnce();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 2);
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 0);
        QCOMPARE(fakeFolder.currentRemoteState().children.count(), 0);

        // die Bestätigung ist verbraucht: neue Löschungen fragen wieder
        fakeFolder.remoteModifier().mkdir(QStringLiteral("Neu"));
        fakeFolder.remoteModifier().insert(QStringLiteral("Neu/n1"));
        QVERIFY(fakeFolder.syncOnce());
        fakeFolder.remoteModifier().remove(QStringLiteral("Neu"));
        QVERIFY(!fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 3);
    }

    // Auf dem Server gelöscht, lokal nur Platzhalter ohne Inhalt: es geht nichts verloren, keine Rückfrage
    void testDehydratedPlaceholdersDoNotCount()
    {
        if (!VfsPluginManager::instance().isVfsPluginAvailable(Vfs::WindowsCfApi)) {
            QSKIP("no virtual files plugin");
        }
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12(), Vfs::WindowsCfApi, true};
        fakeFolder.syncEngine()->setPromptRemoveAllFiles(true);
        auto spy = removeAllSpy(fakeFolder);

        for (const auto &dir : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("S")}) {
            fakeFolder.remoteModifier().remove(dir);
        }
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(spy->count(), 0);
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 0);
    }
};

QTEST_GUILESS_MAIN(TestAllFilesDeleted)
#include "testallfilesdeleted.moc"
