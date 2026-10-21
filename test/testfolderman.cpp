/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <qglobal.h>
#include <QTemporaryDir>
#include <QtTest>

#include "common/syncjournaldb.h"
#include "common/utility.h"
#include "folder.h"
#include "folderman.h"
#include "accountstate.h"

#include "guiutility.h"
#include "testutils/testutils.h"

#include <syncenginetestutils.h>

#ifndef Q_OS_WIN
#include <unistd.h>
#else
#include "common/utility_win.h"
#endif

using namespace OCC;

namespace {
// Fähigkeiten eines ownCloud-10-Servers (owncloud.online): ohne Spaces
QVariantMap oc10Capabilities()
{
    auto caps = TestUtils::testCapabilities();
    caps.remove(QStringLiteral("spaces"));
    return caps;
}

// ein Ordnerpaar wie aus dem Ordner-Assistenten für ownCloud 10: ohne Space-ID, mit Serverordner
FolderDefinition oc10FolderDefinition(Account *account, const QString &localPath, const QString &remotePath)
{
    FolderDefinition definition(account->davUrl(), {}, {});
    definition.setLocalPath(localPath);
    definition.setTargetPath(remotePath);
    definition.setJournalPath(SyncJournalDb::makeDbName(definition.localPath()));
    return definition;
}
}


class TestFolderMan: public QObject
{
    Q_OBJECT
private Q_SLOTS:
    /*
     * Note: this is only meant for folders/spaces, not `SpacesSyncRoot` testing! That is done in `testSpacesSyncRootAndFolderCreation`.
     */
    void testCheckPathValidityForNewFolder()
    {
#ifdef Q_OS_WIN
        QNtfsPermissionCheckGuard ntfs_perm;
#endif
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QDir dir2(dir.path());
        QVERIFY(dir2.mkpath(QStringLiteral("sub/ownCloud1/folder/f")));
        QVERIFY(dir2.mkpath(QStringLiteral("ownCloud2")));
        QVERIFY(dir2.mkpath(QStringLiteral("sub/free")));
        QVERIFY(dir2.mkpath(QStringLiteral("free2/sub")));
        {
            QFile f(dir.path() + QStringLiteral("/sub/file.txt"));
            f.open(QFile::WriteOnly);
            f.write("hello");
        }
        QString dirPath = dir2.canonicalPath();

        auto newAccountState = createDummyAccount();

        FolderMan *folderman = TestUtils::folderMan();
        QCOMPARE(folderman, FolderMan::instance());
        QVERIFY(folderman->addFolder(
            newAccountState.get(), TestUtils::createDummyFolderDefinition(newAccountState->account(), dirPath + QStringLiteral("/sub/ownCloud1"))));
        QVERIFY(folderman->addFolder(
            newAccountState.get(), TestUtils::createDummyFolderDefinition(newAccountState->account(), dirPath + QStringLiteral("/ownCloud2"))));

        const auto type = FolderMan::NewFolderType::SpacesFolder;
        const QUuid uuid = newAccountState->account()->uuid();

        QCOMPARE(folderman->checkPathValidity(dirPath + QStringLiteral("/sub/free"), type, uuid), QString());
        QCOMPARE(folderman->checkPathValidity(dirPath + QStringLiteral("/free2/"), type, uuid), QString());
        // Not an existing directory -> Ok
        QCOMPARE(folderman->checkPathValidity(dirPath + QStringLiteral("/sub/bliblablu"), type, uuid), QString());
        QCOMPARE(folderman->checkPathValidity(dirPath + QStringLiteral("/sub/free/bliblablu"), type, uuid), QString());

        // A file -> Error
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/sub/file.txt"), type, uuid).isNull());

        // The following both fail because they refer to the same account
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/sub/ownCloud1"), type, uuid).isNull());
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/ownCloud2/"), type, uuid).isNull());

        QVERIFY(!folderman->checkPathValidity(dirPath, type, uuid).isNull());
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/sub/ownCloud1/folder"), type, uuid).isNull());
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/sub/ownCloud1/folder/f"), type, uuid).isNull());

#ifndef Q_OS_WIN // no links on windows, no permissions
        // make a bunch of links
        QVERIFY(QFile::link(dirPath + QStringLiteral("/sub/free"), dirPath + QStringLiteral("/link1")));
        QVERIFY(QFile::link(dirPath + QStringLiteral("/sub"), dirPath + QStringLiteral("/link2")));
        QVERIFY(QFile::link(dirPath + QStringLiteral("/sub/ownCloud1"), dirPath + QStringLiteral("/link3")));
        QVERIFY(QFile::link(dirPath + QStringLiteral("/sub/ownCloud1/folder"), dirPath + QStringLiteral("/link4")));

        // Ok
        QVERIFY(folderman->checkPathValidity(dirPath + QStringLiteral("/link1"), type, uuid).isNull());
        QVERIFY(folderman->checkPathValidity(dirPath + QStringLiteral("/link2/free"), type, uuid).isNull());

        // Not Ok
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/link2"), type, uuid).isNull());

        // link 3 points to an existing sync folder. To make it fail, the account must be the same
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/link3"), type, uuid).isNull());

        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/link4"), type, uuid).isNull());
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/link3/folder"), type, uuid).isNull());

        // test some non existing sub path (error)
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/sub/ownCloud1/some/sub/path"), type, uuid).isNull());
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/ownCloud2/blublu"), type, uuid).isNull());
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/sub/ownCloud1/folder/g/h"), type, uuid).isNull());
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/link3/folder/neu_folder"), type, uuid).isNull());

        // Subfolder of links
        QVERIFY(folderman->checkPathValidity(dirPath + QStringLiteral("/link1/subfolder"), type, uuid).isNull());
        QVERIFY(folderman->checkPathValidity(dirPath + QStringLiteral("/link2/free/subfolder"), type, uuid).isNull());

        if (getuid() != 0) {
            // Should not have the rights
            QVERIFY(!folderman->checkPathValidity(QStringLiteral("/"), type, uuid).isNull());
            QVERIFY(!folderman->checkPathValidity(QStringLiteral("/usr/bin/somefolder"), type, uuid).isNull());
        }
#endif

        if (Utility::isWindows()) { // drive-letter tests
            if (!QFileInfo::exists("v:/")) {
                QVERIFY(!folderman->checkPathValidity(QStringLiteral("v:"), type, uuid).isNull());
                QVERIFY(!folderman->checkPathValidity(QStringLiteral("v:/"), type, uuid).isNull());
                QVERIFY(!folderman->checkPathValidity(QStringLiteral("v:/foo"), type, uuid).isNull());
            }
            if (QFileInfo(QStringLiteral("c:/")).isWritable()) {
                QVERIFY(folderman->checkPathValidity(QStringLiteral("c:"), type, uuid).isNull());
                QVERIFY(folderman->checkPathValidity(QStringLiteral("c:/"), type, uuid).isNull());
                QVERIFY(folderman->checkPathValidity(QStringLiteral("c:/foo"), type, uuid).isNull());
            }
        }

        // Invalid paths
        QVERIFY(!folderman->checkPathValidity({}, type, uuid).isNull());


        // REMOVE ownCloud2 from the filesystem, but keep a folder sync'ed to it.
        QDir(dirPath + QStringLiteral("/ownCloud2/")).removeRecursively();
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/ownCloud2/blublu"), type, uuid).isNull());
        QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/ownCloud2/sub/subsub/sub"), type, uuid).isNull());

        { // check for rejection of a directory with `.sync_*.db`
            QVERIFY(dir2.mkpath(QStringLiteral("db-check1")));
            QVERIFY(folderman->checkPathValidity(dirPath + QStringLiteral("/db-check1"), type, uuid).isNull());
            QFile f(dirPath + QStringLiteral("/db-check1/.sync_something.db"));
            QVERIFY(f.open(QFile::Truncate | QFile::WriteOnly));
            f.close();
            QVERIFY(QFileInfo::exists(dirPath + QStringLiteral("/db-check1/.sync_something.db")));
            QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/db-check1"), type, uuid).isNull());
        }

        { // check for rejection of a directory with `._sync_*.db`
            QVERIFY(dir2.mkpath(QStringLiteral("db-check2")));
            QVERIFY(folderman->checkPathValidity(dirPath + QStringLiteral("/db-check2"), type, uuid).isNull());
            QFile f(dirPath + QStringLiteral("/db-check2/._sync_something.db"));
            QVERIFY(f.open(QFile::Truncate | QFile::WriteOnly));
            f.close();
            QVERIFY(QFileInfo::exists(dirPath + QStringLiteral("/db-check2/._sync_something.db")));
            QVERIFY(!folderman->checkPathValidity(dirPath + QStringLiteral("/db-check2"), type, uuid).isNull());
        }
    }

    /*
     * Note: this is only meant for folders/spaces, not `SpacesSyncRoot` testing! That is done in `testSpacesSyncRootAndFolderCreation`.
     */
    void testFindGoodPathForNewSyncFolder()
    {
        // SETUP

        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QDir dir2(dir.path());

        // Folder in config and on disk:
        // todo: where is this added "to the config" and what is the "config" we are even talking about here?
        QVERIFY(dir2.mkpath(QStringLiteral("sub/ownCloud1/folder/f")));
        // Folders only on disk, not in configuration:
        QVERIFY(dir2.mkpath(QStringLiteral("ownCloud")));
        QVERIFY(dir2.mkpath(QStringLiteral("ownCloud2")));
        QVERIFY(dir2.mkpath(QStringLiteral("ownCloud2/foo")));
        QVERIFY(dir2.mkpath(QStringLiteral("sub/free")));
        QVERIFY(dir2.mkpath(QStringLiteral("free2/sub")));
        QString dirPath = dir2.canonicalPath();

        auto newAccountState = createDummyAccount();

        FolderMan *folderman = TestUtils::folderMan();

        // todo: addFolder needs to be made private to FolderMan because there are other important impls "around" it depending on how the folder
        // is being created.
        // The only reason it's not currently private is because it's used in the tests, which is dubious.
        // AT THE VERY LEAST, the folder needs to exist on disk *before* calling addFolder and with new refactoring, addFolder will fail if that is not the case
        // historically, calling addFolder without an existing local dir resulted in a hideously broken Folder instance that is just waiting to crash ;)
        QVERIFY(dir2.mkpath(QStringLiteral("sub/ownCloud/")));
        QVERIFY(folderman->addFolder(
            newAccountState.get(), TestUtils::createDummyFolderDefinition(newAccountState->account(), dirPath + QStringLiteral("/sub/ownCloud/"))));

        QVERIFY(dir2.mkpath(QStringLiteral("ownCloud (2)/")));
        QVERIFY(folderman->addFolder(
            newAccountState.get(), TestUtils::createDummyFolderDefinition(newAccountState->account(), dirPath + QStringLiteral("/ownCloud (2)/"))));

        // Test todo: verify that addFolder with path that has no existing local folder fails, and do some findGoodPathForNewSyncFolder tests around
        // that too? With new updates, the folder will not be created, hence will not exist in the FolderMan folder container(s) which does change
        // the results of the function under test, afaik

        // TEST
        const auto folderType = FolderMan::NewFolderType::SpacesFolder;
        const auto uuid = newAccountState->account()->uuid();

        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("oc"), folderType, uuid), dirPath + QStringLiteral("/oc"));
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("ownCloud"), folderType, uuid), dirPath + QStringLiteral("/ownCloud (3)"));
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("ownCloud2"), folderType, uuid), dirPath + QStringLiteral("/ownCloud2 (2)"));
        QCOMPARE(
            folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("ownCloud (2)"), folderType, uuid), dirPath + QStringLiteral("/ownCloud (2) (2)"));
        QCOMPARE(
            folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("ownCloud2/foo"), folderType, uuid), dirPath + QStringLiteral("/ownCloud2_foo"));
        QCOMPARE(
            folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("ownCloud2/bar"), folderType, uuid), dirPath + QStringLiteral("/ownCloud2_bar"));
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("sub"), folderType, uuid), dirPath + QStringLiteral("/sub (2)"));

        // REMOVE ownCloud2 from the filesystem, but keep a folder sync'ed to it.
        // We should still not suggest this folder as a new folder.
        // todo: add a verify here to check if the folder exists in the first place before removing it
        QDir(dirPath + QStringLiteral("/ownCloud (2)/")).removeRecursively();
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("ownCloud"), folderType, uuid), dirPath + QStringLiteral("/ownCloud (3)"));
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("ownCloud2"), folderType, uuid),
            QString(dirPath + QStringLiteral("/ownCloud2 (2)")));
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("ownCloud (2)"), folderType, uuid),
            QString(dirPath + QStringLiteral("/ownCloud (2) (2)")));

        // make sure people can't do evil stuff
        QCOMPARE(
            folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("../../../Bo/b"), folderType, uuid), QString(dirPath + QStringLiteral("/___Bo_b")));

        // normalise the name
        QCOMPARE(folderman->findGoodPathForNewSyncFolder(dirPath, QStringLiteral("            Bo:*<>!b          "), folderType, uuid),
            QString(dirPath + QStringLiteral("/Bo____!b")));
    }

    /*
     * Ohne Spaces (ownCloud 10) haben alle Ordnerpaare eines Kontos eine leere Space-ID. Sie dürfen sich in FolderMan nicht
     * gegenseitig ersetzen, und ein Serverordner darf weder selbst noch als Ober- oder Unterordner ein zweites Mal
     * gespiegelt werden.
     */
    void testOc10FolderPairsAndRemotePathValidity()
    {
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QDir dir2(dir.path());
        QVERIFY(dir2.mkpath(QStringLiteral("docs")));
        QVERIFY(dir2.mkpath(QStringLiteral("photos")));
        const QString dirPath = dir2.canonicalPath();

        auto accountState = createDummyAccount();
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        const QUuid uuid = account->uuid();

        FolderMan *folderman = TestUtils::folderMan();
        // ohne Ordnerpaar ist jeder Serverordner frei, auch die Wurzel
        QCOMPARE(folderman->checkRemotePathValidity(uuid, QStringLiteral("/")), QString());

        Folder *docs = folderman->addFolder(accountState.get(), oc10FolderDefinition(account, dirPath + QStringLiteral("/docs"), QStringLiteral("/Dokumente")));
        QVERIFY(docs);
        Folder *photos = folderman->addFolder(accountState.get(), oc10FolderDefinition(account, dirPath + QStringLiteral("/photos"), QStringLiteral("/Fotos")));
        QVERIFY(photos);
        QVERIFY(photos != docs);
        QCOMPARE(folderman->foldersForAccount(uuid).size(), 2);

        // derselbe Ordner, gleich wie geschrieben
        for (const auto &same : {QStringLiteral("/Dokumente"), QStringLiteral("/Dokumente/"), QStringLiteral("Dokumente"), QStringLiteral("//Dokumente//")}) {
            const QString error = folderman->checkRemotePathValidity(uuid, same);
            QVERIFY2(!error.isEmpty(), qPrintable(same));
            // die Meldung nennt den lokalen Ordner, mit dem der Serverordner schon verbunden ist
            QVERIFY(error.contains(QDir::toNativeSeparators(docs->cleanPath())));
        }
        // Unterordner eines gespiegelten Ordners
        QVERIFY(!folderman->checkRemotePathValidity(uuid, QStringLiteral("/Dokumente/Rechnungen")).isEmpty());
        QVERIFY(!folderman->checkRemotePathValidity(uuid, QStringLiteral("/Fotos/2026/Urlaub")).isEmpty());
        // Oberordner, auch die Kontowurzel
        QVERIFY(!folderman->checkRemotePathValidity(uuid, QStringLiteral("/")).isEmpty());
        QVERIFY(!folderman->checkRemotePathValidity(uuid, QString()).isEmpty());
        // gleicher Anfang ist kein Unterordner, Serverpfade unterscheiden Groß- und Kleinschreibung
        QCOMPARE(folderman->checkRemotePathValidity(uuid, QStringLiteral("/Dokumente-Archiv")), QString());
        QCOMPARE(folderman->checkRemotePathValidity(uuid, QStringLiteral("/dokumente")), QString());
        QCOMPARE(folderman->checkRemotePathValidity(uuid, QStringLiteral("/Musik")), QString());
        // andere Konten bleiben unberührt
        QCOMPARE(folderman->checkRemotePathValidity(QUuid::createUuid(), QStringLiteral("/Dokumente")), QString());

        // die letzte Sicherung in FolderMan legt kein Paar für einen schon gespiegelten Serverordner an
        FolderMan::SyncConnectionDescription description{account->davUrl(), {}, dirPath + QStringLiteral("/rechnungen"),
            QStringLiteral("/Dokumente/Rechnungen"), QStringLiteral("Rechnungen"), false, 0, {}};
        folderman->addFolderFromGui(accountState.get(), description);
        QCOMPARE(folderman->foldersForAccount(uuid).size(), 2);
        QVERIFY(!QFileInfo::exists(dirPath + QStringLiteral("/rechnungen")));

        // Entfernen eines Paars lässt das andere stehen und gibt seinen Serverordner frei
        folderman->removeFolderFromGui(photos);
        const auto remaining = folderman->foldersForAccount(uuid);
        QCOMPARE(remaining.size(), 1);
        QCOMPARE(remaining.first(), docs);
        QCOMPARE(folderman->checkRemotePathValidity(uuid, QStringLiteral("/Fotos")), QString());
        QVERIFY(!folderman->checkRemotePathValidity(uuid, QStringLiteral("/Dokumente")).isEmpty());
    }

    // Wird ein Konto mit vielen Ordnerpaaren (ownCloud 10) entfernt, verschwinden alle: Bisher lief die Schleife über den
    // Hash, aus dem sie zugleich löschte, und übersprang Einträge. Die übersprungenen behielten Journal und
    // Sync-Root-Registrierung und lebten bis zum Neustart weiter.
    void testRemovingAccountRemovesAllOc10Pairs()
    {
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        const QString dirPath = QDir(dir.path()).canonicalPath();

        auto accountState = createDummyAccount();
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        const QUuid uuid = account->uuid();
        FolderMan *folderman = TestUtils::folderMan();

        constexpr int pairs = 40;
        int destroyed = 0;
        QStringList journals;
        for (int i = 0; i < pairs; ++i) {
            const QString local = dirPath + QStringLiteral("/paar%1").arg(i);
            QVERIFY(QDir().mkpath(local));
            Folder *folder = folderman->addFolder(accountState.get(), oc10FolderDefinition(account, local, QStringLiteral("/Ordner%1").arg(i)));
            QVERIFY(folder);
            connect(folder, &QObject::destroyed, this, [&destroyed] { ++destroyed; });
            // das Journal anlegen, damit sein Löschen sichtbar ist
            folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, {});
            journals.append(folder->journalDb()->databaseFilePath());
            QVERIFY(QFileInfo::exists(journals.last()));
        }
        QCOMPARE(folderman->foldersForAccount(uuid).size(), pairs);

        QVERIFY(QMetaObject::invokeMethod(folderman, "slotRemoveFoldersForAccount", Q_ARG(OCC::AccountState *, accountState.get())));

        QVERIFY(folderman->foldersForAccount(uuid).isEmpty());
        QTRY_COMPARE(destroyed, pairs);
        for (const auto &journal : std::as_const(journals)) {
            QVERIFY2(!QFileInfo::exists(journal), qPrintable(journal));
        }
    }

    // Spaces (oCIS) bleiben wie bisher: dort gibt es keine Prüfung der Serverordner
    void testRemotePathValidityIgnoresSpaces()
    {
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QDir dir2(dir.path());
        QVERIFY(dir2.mkpath(QStringLiteral("space")));

        auto accountState = createDummyAccount();
        FolderMan *folderman = TestUtils::folderMan();
        QVERIFY(folderman->addFolder(
            accountState.get(), TestUtils::createDummyFolderDefinition(accountState->account(), dir2.canonicalPath() + QStringLiteral("/space"))));

        const QUuid uuid = accountState->account()->uuid();
        QCOMPARE(folderman->checkRemotePathValidity(uuid, QStringLiteral("/")), QString());
        QCOMPARE(folderman->checkRemotePathValidity(uuid, dir2.canonicalPath() + QStringLiteral("/space")), QString());
    }

    void testSpacesSyncRootAndFolderCreation()
    {
        const QString someDeepPath = "/Some/Deep/Path";

        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QDir dir2(dir.path());

        // Create a sync root for another account
        QVERIFY(dir2.mkpath(QStringLiteral("AnotherSpacesSyncRoot") + someDeepPath));
        const auto anotherUuid = QUuid::createUuid();
        Utility::markDirectoryAsSyncRoot(dir2.filePath(QStringLiteral("AnotherSpacesSyncRoot")), anotherUuid);

        FolderMan *folderman = TestUtils::folderMan();
        QCOMPARE(folderman, FolderMan::instance());

        QString dirPath = dir2.canonicalPath();
        const auto ourUuid = QUuid::createUuid();

        // Spaces Sync Root in another Spaces Sync Root should fail
        QVERIFY(!folderman->checkPathValidity(
            dirPath + QStringLiteral("/AnotherSpacesSyncRoot/OurSpacesSyncRoot"), FolderMan::NewFolderType::SpacesSyncRoot, ourUuid)
                .isNull());
        // Also deep inside
        QVERIFY(!folderman->checkPathValidity(
            dirPath + QStringLiteral("/AnotherSpacesSyncRoot") + someDeepPath + "/OurSpacesSyncRoot", FolderMan::NewFolderType::SpacesSyncRoot, ourUuid)
                .isNull());
        // Spaces Sync Root one level up should be fine
        QVERIFY(folderman->checkPathValidity(dirPath + QStringLiteral("/OurSpacesSyncRoot"), FolderMan::NewFolderType::SpacesSyncRoot, ourUuid).isNull());

        // Create the sync root so we can test Spaces Folder creation below
        QVERIFY(dir2.mkpath(QStringLiteral("OurSpacesSyncRoot")));
        Utility::markDirectoryAsSyncRoot(dir2.filePath(QStringLiteral("OurSpacesSyncRoot")), ourUuid);

        // A folder for a Space in a sync root for another account should fail
        QVERIFY(
            !folderman->checkPathValidity(dirPath + QStringLiteral("/AnotherSpacesSyncRoot/OurSpacesFolder"), FolderMan::NewFolderType::SpacesFolder, ourUuid)
                .isNull());
        // Also deep inside
        QVERIFY(!folderman->checkPathValidity(
            dirPath + QStringLiteral("/AnotherSpacesSyncRoot") + someDeepPath + "/OurSpacesFolder", FolderMan::NewFolderType::SpacesFolder, ourUuid)
                .isNull());
        // But in our sync root that should just be fine
        QVERIFY(folderman->checkPathValidity(dirPath + QStringLiteral("/OurSpacesSyncRoot/OurSpacesFolder"), FolderMan::NewFolderType::SpacesFolder, ourUuid)
                .isNull());

        // Ein schon gekennzeichneter Ordner behält seine Kennzeichnung; früher beendete ein Q_ASSERT das Programm (FORCE_ASSERTS)
        Utility::markDirectoryAsSyncRoot(dir2.filePath(QStringLiteral("OurSpacesSyncRoot")), QUuid::createUuid());
        QVERIFY(folderman->checkPathValidity(dirPath + QStringLiteral("/OurSpacesSyncRoot/OurSpacesFolder"), FolderMan::NewFolderType::SpacesFolder, ourUuid)
                .isNull());
    }

    // Tray-Menü je Konto: hält nur die Ordner dieses Kontos an und setzt sie wieder fort
    void testAccountSyncPause()
    {
        // Arrange: zwei Ordner des einen Kontos, einer eines anderen
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QDir dir2(dir.path());
        for (const auto &name : {QStringLiteral("a1"), QStringLiteral("a2"), QStringLiteral("b1")}) {
            QVERIFY(dir2.mkpath(name));
        }
        const QString dirPath = dir2.canonicalPath();
        auto accountA = createDummyAccount();
        auto accountB = createDummyAccount();
        FolderMan *folderman = TestUtils::folderMan();
        Folder *a1 = folderman->addFolder(accountA.get(), TestUtils::createDummyFolderDefinition(accountA->account(), dirPath + QStringLiteral("/a1")));
        Folder *a2 = folderman->addFolder(accountA.get(), TestUtils::createDummyFolderDefinition(accountA->account(), dirPath + QStringLiteral("/a2")));
        Folder *b1 = folderman->addFolder(accountB.get(), TestUtils::createDummyFolderDefinition(accountB->account(), dirPath + QStringLiteral("/b1")));
        QVERIFY(a1 && a2 && b1);
        // einer schon einzeln im Hauptfenster pausiert
        a2->setSyncPaused(true);
        QSignalSpy pausedChanged(a1, &Folder::syncPausedChanged);

        // Act
        folderman->setAccountSyncPaused(accountA->account()->uuid(), true);

        // Assert: nur das eine Konto hält an, der schon pausierte Ordner meldet nichts doppelt
        QVERIFY(a1->syncPaused());
        QVERIFY(a2->syncPaused());
        QVERIFY(!b1->syncPaused());
        QCOMPARE(pausedChanged.size(), 1);

        // Act: fortsetzen
        folderman->setAccountSyncPaused(accountA->account()->uuid(), false);

        // Assert
        QVERIFY(!a1->syncPaused());
        QVERIFY(!a2->syncPaused());
        QVERIFY(!b1->syncPaused());
        QCOMPARE(pausedChanged.size(), 2);
    }
};

QTEST_GUILESS_MAIN(TestFolderMan)
#include "testfolderman.moc"
