/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// owncloud.online: Umstieg vom offiziellen ownCloud-Desktop-Client – Konfiguration lesen (Beispiele aus 6.0.3 und
// 2.x), Journal kopieren, Ordner übernehmen, ohne erneut herunterzuladen und ohne den Fremd-Client zu verändern

#include "accountstate.h"
#include "common/ownsql.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
#include "folder.h"
#include "folderman.h"
#include "legacyclient/legacyclientconfig.h"
#include "legacyclient/legacyclientimport.h"
#include "testutils/syncenginetestutils.h"
#include "testutils/testutils.h"

#include <QCryptographicHash>
#include <QSignalSpy>
#include <QtTest>

using namespace OCC;

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
}

QByteArray fileHash(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

// Beispiel wie die owncloud.cfg eines 6.0.3-Clients: ein Konto mit einem Unterordner (klassisch) und der Wurzel mit
// virtuellen Dateien, dazu ein zweites Konto ohne Ordner
QByteArray sampleConfig603(const QString &documents, const QString &root)
{
    return QStringLiteral("[General]\n"
                          "clientVersion=6.0.3.18040\n"
                          "optionalDesktopNotifications=true\n"
                          "\n"
                          "[Accounts]\n"
                          "0\\Folders\\31a0efd3-c1d6-45e4-ba22-b3522eeaa744\\localPath=%1\n"
                          "0\\Folders\\a27e5f56-87e8-410e-b41d-d2f970e1e794\\localPath=%2\n"
                          "version=13\n"
                          "0\\Folders\\31a0efd3-c1d6-45e4-ba22-b3522eeaa744\\journalPath=.sync_journal.db\n"
                          "0\\Folders\\a27e5f56-87e8-410e-b41d-d2f970e1e794\\journalPath=.sync_journal.db\n"
                          "0\\version=13\n"
                          "0\\Folders\\31a0efd3-c1d6-45e4-ba22-b3522eeaa744\\targetPath=/Rechnungen/Eingang\n"
                          "0\\Folders\\a27e5f56-87e8-410e-b41d-d2f970e1e794\\targetPath=/\n"
                          "0\\url=https://cloud.example.com\n"
                          "0\\Folders\\31a0efd3-c1d6-45e4-ba22-b3522eeaa744\\davUrl=https://cloud.example.com/remote.php/dav/files/anna/\n"
                          "0\\Folders\\a27e5f56-87e8-410e-b41d-d2f970e1e794\\davUrl=https://cloud.example.com/remote.php/dav/files/anna/\n"
                          "0\\dav_user=anna\n"
                          "0\\Folders\\31a0efd3-c1d6-45e4-ba22-b3522eeaa744\\displayString=Eingang\n"
                          "0\\Folders\\a27e5f56-87e8-410e-b41d-d2f970e1e794\\displayString=ownCloud\n"
                          "0\\display-name=Anna Beispiel\n"
                          "0\\Folders\\31a0efd3-c1d6-45e4-ba22-b3522eeaa744\\paused=true\n"
                          "0\\Folders\\a27e5f56-87e8-410e-b41d-d2f970e1e794\\paused=false\n"
                          "0\\Folders\\31a0efd3-c1d6-45e4-ba22-b3522eeaa744\\virtualFilesMode=off\n"
                          "0\\Folders\\a27e5f56-87e8-410e-b41d-d2f970e1e794\\virtualFilesMode=wincfapi\n"
                          "0\\http_CredentialVersion=1\n"
                          "0\\http_oauth=true\n"
                          "0\\http_user=anna\n"
                          "1\\url=https://other.example.org/owncloud\n"
                          "1\\dav_user=bob\n")
        .arg(documents, root)
        .toUtf8();
}

// Beispiel eines 2.x-Clients: Benutzer unter user/http_user, Platzhalter-Ordner in eigener Gruppe, Journal mit Hash
QByteArray sampleConfig2x(const QString &folder, const QString &photos)
{
    return QStringLiteral("[General]\n"
                          "clientVersion=2.11.1.13579\n"
                          "\n"
                          "[Accounts]\n"
                          "version=2\n"
                          "0\\version=1\n"
                          "0\\url=https://cloud.example.com/\n"
                          "0\\authType=http\n"
                          "0\\user=anna\n"
                          "0\\http_user=anna\n"
                          "0\\Folders\\1\\localPath=%1\n"
                          "0\\Folders\\1\\journalPath=._sync_8a3c1f2b9d0e.db\n"
                          "0\\Folders\\1\\targetPath=/\n"
                          "0\\Folders\\1\\paused=false\n"
                          "0\\Folders\\1\\ignoreHiddenFiles=true\n"
                          "0\\FoldersWithPlaceholders\\2\\localPath=%2\n"
                          "0\\FoldersWithPlaceholders\\2\\targetPath=/Fotos\n"
                          "0\\FoldersWithPlaceholders\\2\\usePlaceholders=true\n")
        .arg(folder, photos)
        .toUtf8();
}

// Journal wie vom Fremd-Client: eine Datei als synchronisiert eingetragen
void writeLegacyJournal(const QString &journalPath)
{
    SyncJournalDb journal(journalPath, nullptr);
    SyncJournalFileRecord record;
    record._path = "Bericht.txt";
    record._type = ItemTypeFile;
    record._etag = "etag-1";
    record._fileId = "00000123";
    record._fileSize = 5;
    record._modtime = 1700000000;
    record._inode = 4711;
    record._remotePerm = RemotePermissions::fromDbValue("WDNVCKR");
    QVERIFY(journal.setFileRecord(record));
    journal.close();
}

// Dummy-Konto wie ownCloud 10 / owncloud.online: ohne Spaces
TestUtils::TestUtilsPrivate::AccountStateRaii oc10Account()
{
    auto accountState = createDummyAccount();
    auto capabilities = TestUtils::testCapabilities();
    capabilities.remove(QStringLiteral("spaces"));
    accountState->account()->setCapabilities({accountState->account()->url(), capabilities});
    return accountState;
}

}

class TestLegacyClient : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testRead603Config()
    {
        const auto dir = TestUtils::createTempDir();
        const QString documents = dir.filePath(QStringLiteral("Rechnungen"));
        const QString root = dir.filePath(QStringLiteral("ownCloud"));
        const QString cfg = dir.filePath(QStringLiteral("ownCloud/owncloud.cfg"));
        writeFile(cfg, sampleConfig603(documents, root + QLatin1Char('/')));

        const auto config = LegacyClient::readConfig(cfg);
        QVERIFY(config);
        QCOMPARE(config->path, cfg);
        QCOMPARE(config->clientVersion, QStringLiteral("6.0.3.18040"));
        QCOMPARE(config->accounts.size(), 2);

        const auto &anna = config->accounts.at(0);
        QCOMPARE(anna.url, QUrl(QStringLiteral("https://cloud.example.com")));
        QCOMPARE(anna.davUser, QStringLiteral("anna"));
        QCOMPARE(anna.displayName, QStringLiteral("Anna Beispiel"));
        QCOMPARE(anna.displayString(), QStringLiteral("Anna Beispiel@cloud.example.com"));
        QCOMPARE(anna.folders.size(), 2);

        const auto &classic = anna.folders.at(0);
        QCOMPARE(classic.localPath, documents + QLatin1Char('/'));
        QCOMPARE(classic.targetPath, QStringLiteral("/Rechnungen/Eingang"));
        QCOMPARE(classic.davUrl, QUrl(QStringLiteral("https://cloud.example.com/remote.php/dav/files/anna/")));
        QCOMPARE(classic.displayName, QStringLiteral("Eingang"));
        QVERIFY(classic.paused);
        QVERIFY(!classic.usesVirtualFiles());
        QCOMPARE(classic.absoluteJournalPath(), documents + QStringLiteral("/.sync_journal.db"));

        const auto &virtualRoot = anna.folders.at(1);
        QCOMPARE(virtualRoot.targetPath, QStringLiteral("/"));
        QVERIFY(virtualRoot.usesVirtualFiles());
        QVERIFY(!virtualRoot.paused);

        const auto &bob = config->accounts.at(1);
        QCOMPARE(bob.url, QUrl(QStringLiteral("https://other.example.org/owncloud")));
        QCOMPARE(bob.davUser, QStringLiteral("bob"));
        QVERIFY(bob.folders.isEmpty());
    }

    void testRead2xConfig()
    {
        const auto dir = TestUtils::createTempDir();
        const QString folder = dir.filePath(QStringLiteral("ownCloud"));
        const QString photos = dir.filePath(QStringLiteral("Fotos"));
        const QString cfg = dir.filePath(QStringLiteral("owncloud.cfg"));
        writeFile(cfg, sampleConfig2x(folder, photos));

        const auto config = LegacyClient::readConfig(cfg);
        QVERIFY(config);
        QCOMPARE(config->accounts.size(), 1);
        const auto &account = config->accounts.first();
        QCOMPARE(account.davUser, QStringLiteral("anna"));
        QCOMPARE(account.folders.size(), 2);
        QCOMPARE(account.folders.at(0).absoluteJournalPath(), folder + QStringLiteral("/._sync_8a3c1f2b9d0e.db"));
        QVERIFY(!account.folders.at(0).usesVirtualFiles());
        QVERIFY(!account.folders.at(0).davUrl.isValid());
        QCOMPARE(account.folders.at(1).targetPath, QStringLiteral("/Fotos"));
        QVERIFY(account.folders.at(1).usesVirtualFiles());
    }

    // Die Konfiguration des Fremd-Clients bleibt Byte für Byte und mit ihrem Zeitstempel, wie sie war
    void testReadingDoesNotTouchTheConfig()
    {
        const auto dir = TestUtils::createTempDir();
        const QString cfg = dir.filePath(QStringLiteral("owncloud.cfg"));
        writeFile(cfg, sampleConfig603(dir.filePath(QStringLiteral("a")), dir.filePath(QStringLiteral("b"))));
        const QByteArray hash = fileHash(cfg);
        const QDateTime modified = QFileInfo(cfg).lastModified();

        QVERIFY(LegacyClient::readConfig(cfg));
        QVERIFY(LegacyClient::findConfig({cfg}));

        QCOMPARE(fileHash(cfg), hash);
        QCOMPARE(QFileInfo(cfg).lastModified(), modified);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden), QStringList{QStringLiteral("owncloud.cfg")});
    }

    // Einen über QSettings geschriebenen Wert (davUrl als QUrl wie im echten Client) liest der Import ebenso
    void testReadsVariantValues()
    {
        const auto dir = TestUtils::createTempDir();
        const QString cfg = dir.filePath(QStringLiteral("owncloud.cfg"));
        {
            QSettings settings(cfg, QSettings::IniFormat);
            settings.setValue(QStringLiteral("Accounts/0/url"), QStringLiteral("https://cloud.example.com"));
            settings.setValue(QStringLiteral("Accounts/0/dav_user"), QStringLiteral("anna"));
            settings.setValue(QStringLiteral("Accounts/0/Folders/x/localPath"), dir.filePath(QStringLiteral("Daten")));
            settings.setValue(QStringLiteral("Accounts/0/Folders/x/davUrl"), QUrl(QStringLiteral("https://cloud.example.com/remote.php/dav/files/anna/")));
        }
        const auto config = LegacyClient::readConfig(cfg);
        QVERIFY(config);
        QCOMPARE(config->accounts.first().folders.first().davUrl, QUrl(QStringLiteral("https://cloud.example.com/remote.php/dav/files/anna/")));
    }

    void testFindConfig()
    {
        const auto dir = TestUtils::createTempDir();
        const QString empty = dir.filePath(QStringLiteral("leer.cfg"));
        writeFile(empty, QByteArrayLiteral("[General]\nclientVersion=6.0.3.18040\n"));
        const QString valid = dir.filePath(QStringLiteral("owncloud.cfg"));
        writeFile(valid, sampleConfig603(dir.filePath(QStringLiteral("a")), dir.filePath(QStringLiteral("b"))));

        // fehlende und kontolose Dateien überspringen
        const auto config = LegacyClient::findConfig({dir.filePath(QStringLiteral("fehlt.cfg")), empty, valid});
        QVERIFY(config);
        QCOMPARE(config->path, valid);
        QVERIFY(!LegacyClient::findConfig({dir.filePath(QStringLiteral("fehlt.cfg")), empty}));
        QVERIFY(!LegacyClient::candidateConfigFiles().isEmpty());
    }

    void testOfficialClientProcessName()
    {
        QVERIFY(LegacyClient::isOfficialClientProcessName(QStringLiteral("owncloud.exe")));
        QVERIFY(LegacyClient::isOfficialClientProcessName(QStringLiteral("ownCloud.EXE")));
        QVERIFY(LegacyClient::isOfficialClientProcessName(QStringLiteral("owncloud")));
        QVERIFY(!LegacyClient::isOfficialClientProcessName(QStringLiteral("owncloud.online.exe")));
        QVERIFY(!LegacyClient::isOfficialClientProcessName(QStringLiteral("owncloudcmd.exe")));
        // im Test läuft er nicht (der Testlauf selbst heißt anders)
        QVERIFY(!LegacyClient::isOfficialClientProcessName(QFileInfo(QCoreApplication::applicationFilePath()).fileName()));
    }

    // Das Journal kommt als Kopie, auch während der Fremd-Client es offen hält; das Original bleibt unverändert
    void testCopyJournal()
    {
        const auto dir = TestUtils::createTempDir();
        const QString source = dir.filePath(QStringLiteral(".sync_journal.db"));
        writeLegacyJournal(source);
        SyncJournalDb openByOfficialClient(source, nullptr);
        SyncJournalFileRecord record;
        QVERIFY(openByOfficialClient.getFileRecord(QStringLiteral("Bericht.txt"), &record));
        const QByteArray hash = fileHash(source);

        const QString target = dir.filePath(QStringLiteral(".sync_online.db"));
        QString error;
        QCOMPARE(LegacyClient::copyJournal(source, target, &error), LegacyClient::JournalCopy::Copied);

        SyncJournalDb copy(target, nullptr);
        SyncJournalFileRecord copied;
        QVERIFY(copy.getFileRecord(QStringLiteral("Bericht.txt"), &copied));
        QCOMPARE(copied._fileId, QByteArray("00000123"));
        QCOMPARE(copied._etag, QByteArray("etag-1"));
        copy.close();
        QCOMPARE(fileHash(source), hash);
    }

    void testCopyJournalWithoutOrBrokenSource()
    {
        const auto dir = TestUtils::createTempDir();
        const QString target = dir.filePath(QStringLiteral(".sync_online.db"));
        QCOMPARE(LegacyClient::copyJournal(dir.filePath(QStringLiteral(".sync_journal.db")), target), LegacyClient::JournalCopy::NoJournal);
        QVERIFY(!QFileInfo::exists(target));

        const QString broken = dir.filePath(QStringLiteral("kaputt.db"));
        writeFile(broken, QByteArray(4096, 'x'));
        QCOMPARE(LegacyClient::copyJournal(broken, target), LegacyClient::JournalCopy::Failed);
        QVERIFY(!QFileInfo::exists(target));
    }

    void testCopyJournalTooNew()
    {
        const auto dir = TestUtils::createTempDir();
        const QString source = dir.filePath(QStringLiteral(".sync_journal.db"));
        writeLegacyJournal(source);
        {
            SqlDatabase db;
            QVERIFY(db.openOrCreateReadWrite(source));
            SqlQuery query(db);
            QCOMPARE(query.prepare("UPDATE version SET major = 99;"), 0);
            QVERIFY(query.exec());
        }
        const QString target = dir.filePath(QStringLiteral(".sync_online.db"));
        QCOMPARE(LegacyClient::copyJournal(source, target), LegacyClient::JournalCopy::TooNew);
        QVERIFY(!QFileInfo::exists(target));
    }

    void testCopiedJournalAvoidsTransfers_data()
    {
        QTest::addColumn<bool>("copyJournal");
        QTest::newRow("mit kopiertem Journal") << true;
        QTest::newRow("ohne Journal") << false;
    }

    // Kern der Übernahme: mit dem kopierten Journal kennt die Engine den Ordner – der Server wird nur oben gefragt,
    // keine Datei wird übertragen oder abgeglichen. Ohne Journal fragt sie jeden Ordner ab und gleicht jede Datei ab.
    void testCopiedJournalAvoidsTransfers()
    {
        QFETCH(bool, copyJournal);
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        const QString source = fakeFolder.localPath() + QStringLiteral(".sync_test.db");
        const QString target = fakeFolder.localPath() + QStringLiteral(".sync_online.db");
        if (copyJournal) {
            QCOMPARE(LegacyClient::copyJournal(source, target), LegacyClient::JournalCopy::Copied);
        }

        auto *journal = new SyncJournalDb(target, &fakeFolder);
        auto *engine = new SyncEngine(fakeFolder.account(), fakeFolder.account()->davUrl(), fakeFolder.localPath(), QString(), journal, &fakeFolder);
        engine->setSyncOptions(fakeFolder.syncEngine()->syncOptions());
        engine->addManualExclude(QStringLiteral("]*.~*"));
        int propfinds = 0;
        int transfers = 0;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            const auto verb = request.attribute(QNetworkRequest::CustomVerbAttribute).toByteArray();
            if (verb == "PROPFIND") {
                ++propfinds;
            } else if (op == QNetworkAccessManager::GetOperation || op == QNetworkAccessManager::PutOperation || op == QNetworkAccessManager::DeleteOperation) {
                ++transfers;
            }
            return nullptr;
        });
        QSignalSpy completed(engine, &SyncEngine::itemCompleted);
        QSignalSpy finished(engine, &SyncEngine::finished);
        engine->startSync();
        QVERIFY(finished.wait(30000));
        QCOMPARE(finished.first().first().toBool(), true);
        QCOMPARE(transfers, 0);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        if (copyJournal) {
            QCOMPARE(propfinds, 1);
            QCOMPARE(completed.count(), 0);
        } else {
            QVERIFY(propfinds > 1);
            QVERIFY(completed.count() > 0);
        }
    }

    // Ein klassischer Ordner wird an Ort und Stelle übernommen, mit eigener Journal-Kopie neben der des Fremd-Clients
    void testImportFolderInPlace()
    {
        const auto dir = TestUtils::createTempDir();
        const QString local = QDir(dir.path()).canonicalPath() + QStringLiteral("/Rechnungen/");
        writeFile(local + QStringLiteral("Bericht.txt"), QByteArrayLiteral("Daten"));
        const QString legacyJournal = local + QStringLiteral(".sync_journal.db");
        writeLegacyJournal(legacyJournal);
        const QByteArray legacyHash = fileHash(legacyJournal);

        auto accountState = oc10Account();
        FolderMan *folderMan = TestUtils::folderMan();
        LegacyClient::Folder folder;
        folder.localPath = local;
        folder.targetPath = QStringLiteral("/Rechnungen");
        folder.davUrl = accountState->account()->davUrl();
        folder.displayName = QStringLiteral("Rechnungen");
        folder.virtualFilesMode = QStringLiteral("off");

        const auto result = LegacyClient::importFolder(folderMan, accountState.get(), folder);
        QCOMPARE(result.result, LegacyClient::FolderImport::Result::Adopted);
        Folder *adopted = folderMan->folder(accountState->account()->uuid(), QString());
        QVERIFY(adopted);
        QCOMPARE(adopted->path(), local);
        QCOMPARE(adopted->remotePath(), QStringLiteral("/Rechnungen"));
        QCOMPARE(adopted->definition().journalPath(), QStringLiteral(".sync_online.db"));
        SyncJournalFileRecord record;
        QVERIFY(adopted->journalDb()->getFileRecord(QStringLiteral("Bericht.txt"), &record));
        QCOMPARE(record._fileId, QByteArray("00000123"));
        // das Journal des Fremd-Clients ist unberührt
        QCOMPARE(fileHash(legacyJournal), legacyHash);

        // ein zweiter Ordner desselben Kontos geht derzeit nicht
        const QString second = QDir(dir.path()).canonicalPath() + QStringLiteral("/Fotos/");
        QVERIFY(QDir().mkpath(second));
        folder.localPath = second;
        const auto secondResult = LegacyClient::importFolder(folderMan, accountState.get(), folder);
        QCOMPARE(secondResult.result, LegacyClient::FolderImport::Result::Skipped);
        QVERIFY(!secondResult.reason.isEmpty());
        QVERIFY(!QFileInfo::exists(second + QStringLiteral(".sync_online.db")));
    }

    void testImportFolderWithoutJournal()
    {
        const auto dir = TestUtils::createTempDir();
        const QString local = QDir(dir.path()).canonicalPath() + QStringLiteral("/Daten/");
        QVERIFY(QDir().mkpath(local));
        auto accountState = oc10Account();
        LegacyClient::Folder folder;
        folder.localPath = local;
        folder.virtualFilesMode = QStringLiteral("off");
        const auto result = LegacyClient::importFolder(TestUtils::folderMan(), accountState.get(), folder);
        QCOMPARE(result.result, LegacyClient::FolderImport::Result::AdoptedWithoutJournal);
    }

    void testImportFolderSkipped()
    {
        const auto dir = TestUtils::createTempDir();
        auto accountState = oc10Account();
        FolderMan *folderMan = TestUtils::folderMan();

        LegacyClient::Folder missing;
        missing.localPath = QDir(dir.path()).canonicalPath() + QStringLiteral("/fehlt/");
        missing.virtualFilesMode = QStringLiteral("off");
        QCOMPARE(LegacyClient::importFolder(folderMan, accountState.get(), missing).result, LegacyClient::FolderImport::Result::Skipped);

        LegacyClient::Folder otherUser;
        otherUser.localPath = QDir(dir.path()).canonicalPath() + QLatin1Char('/');
        otherUser.davUrl = QUrl(QStringLiteral("https://cloud.example.com/remote.php/dav/files/jemand-anders/"));
        otherUser.virtualFilesMode = QStringLiteral("off");
        QCOMPARE(LegacyClient::importFolder(folderMan, accountState.get(), otherUser).result, LegacyClient::FolderImport::Result::Skipped);
        QVERIFY(!folderMan->folder(accountState->account()->uuid(), QString()));
    }
};

QTEST_GUILESS_MAIN(TestLegacyClient)
#include "testlegacyclient.moc"
