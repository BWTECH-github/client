/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QSignalSpy>
#include <QtTest>

#include "accountmanager.h"
#include "accountstate.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
#include "configfile.h"
#include "folder.h"
#include "folderman.h"
#include "servermove.h"
#include "testutils/testutils.h"

#include <syncenginetestutils.h>

using namespace OCC;

namespace {

// Fähigkeiten eines ownCloud-10-Servers (owncloud.online): ohne Spaces
QVariantMap oc10Capabilities()
{
    auto caps = TestUtils::testCapabilities();
    caps.remove(QStringLiteral("spaces"));
    return caps;
}

const QByteArray statusOk = "{\"installed\":true,\"maintenance\":false,\"needsDbUpgrade\":false,\"version\":\"11.0.0.0\",\"versionstring\":\"11.0.0\",\"edition\":\"Community\",\"productname\":\"owncloud.online\",\"product\":\"owncloud.online\"}";

ServerStatusCheck::Result runStatusCheck(QNetworkAccessManager *accessManager, const QUrl &url)
{
    ServerStatusCheck check(accessManager, nullptr);
    QSignalSpy spy(&check, &ServerStatusCheck::finished);
    check.start(url);
    if (!spy.wait()) {
        return {};
    }
    return spy.first().first().value<ServerStatusCheck::Result>();
}

FolderDefinition oc10FolderDefinition(Account *account, const QString &localPath, const QString &remotePath)
{
    FolderDefinition definition(account->davUrl(), {}, {});
    definition.setLocalPath(localPath);
    definition.setTargetPath(remotePath);
    definition.setJournalPath(SyncJournalDb::makeDbName(definition.localPath()));
    return definition;
}

// ein synchronisierter Eintrag im Journal, wie ihn ein ownCloud-10-Server mit der Instanz @p instanceId hinterlässt
void addSyncedFile(Folder *folder, const QByteArray &path, const QByteArray &instanceId)
{
    SyncJournalFileRecord record;
    record._path = path;
    record._type = ItemTypeFile;
    record._etag = "etag";
    record._fileId = "00000042" + instanceId;
    record._modtime = 1700000000;
    record._fileSize = 5;
    record._inode = 4711;
    QVERIFY(folder->journalDb()->setFileRecord(record));
}
}

class TestServerMove : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { qRegisterMetaType<ServerStatusCheck::Result>(); }

    void testNormalizeServerUrl_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");

        QTest::newRow("host only") << QStringLiteral("cloud.example.org") << QStringLiteral("https://cloud.example.org");
        QTest::newRow("spaces around") << QStringLiteral("  https://cloud.example.org/  ") << QStringLiteral("https://cloud.example.org");
        QTest::newRow("subfolder") << QStringLiteral("https://example.org/owncloud/") << QStringLiteral("https://example.org/owncloud");
        QTest::newRow("status.php") << QStringLiteral("https://example.org/oc/status.php") << QStringLiteral("https://example.org/oc");
        QTest::newRow("index.php") << QStringLiteral("https://example.org/index.php") << QStringLiteral("https://example.org");
        QTest::newRow("port") << QStringLiteral("example.org:8443") << QStringLiteral("https://example.org:8443");
        QTest::newRow("upper case scheme and host") << QStringLiteral("HTTPS://Cloud.Example.ORG") << QStringLiteral("https://cloud.example.org");
        QTest::newRow("http stays http") << QStringLiteral("http://intranet.local") << QStringLiteral("http://intranet.local");
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("credentials") << QStringLiteral("https://user:secret@example.org") << QString();
        QTest::newRow("query") << QStringLiteral("https://example.org/?a=b") << QString();
        QTest::newRow("fragment") << QStringLiteral("https://example.org/#x") << QString();
        QTest::newRow("other scheme") << QStringLiteral("ftp://example.org") << QString();
        QTest::newRow("no host") << QStringLiteral("https://") << QString();
    }

    void testNormalizeServerUrl()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);

        QString error;
        const QUrl url = ServerMove::normalizeServerUrl(input, &error);
        QCOMPARE(url.toString(), expected);
        // ohne Ergebnis immer mit Begründung
        QCOMPARE(error.isEmpty(), !expected.isEmpty());
    }

    void testIsSafeRedirect()
    {
        const QUrl https(QStringLiteral("https://cloud.example.org/status.php"));
        const QUrl http(QStringLiteral("http://cloud.example.org/status.php"));
        QVERIFY(ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://cloud.example.org/owncloud/status.php"))));
        QVERIFY(ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://CLOUD.example.org/status.php"))));
        QVERIFY(ServerMove::isSafeRedirect(http, QUrl(QStringLiteral("https://cloud.example.org/status.php"))));
        // nie zu HTTP, nie auf einen anderen Host oder Port, keine Zugangsdaten
        QVERIFY(!ServerMove::isSafeRedirect(https, http));
        QVERIFY(!ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://evil.example.com/status.php"))));
        QVERIFY(!ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://cloud.example.org.evil.com/status.php"))));
        QVERIFY(!ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://cloud.example.org:8443/status.php"))));
        QVERIFY(!ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("https://user:pw@cloud.example.org/status.php"))));
        QVERIFY(!ServerMove::isSafeRedirect(https, QUrl(QStringLiteral("ftp://cloud.example.org/status.php"))));
    }

    void testInstanceIdFromFileId()
    {
        QCOMPARE(ServerMove::instanceIdFromFileId("00000123oc1a2b3c4d5e"), QStringLiteral("oc1a2b3c4d5e"));
        QCOMPARE(ServerMove::instanceIdFromFileId("123456789ocabcdefghij"), QStringLiteral("ocabcdefghij"));
        // keine ownCloud-10-ID: zu kurz, ohne Instanz, oCIS-Format, leer
        QCOMPARE(ServerMove::instanceIdFromFileId("1234567ocabc"), QString());
        QCOMPARE(ServerMove::instanceIdFromFileId("00000123"), QString());
        QCOMPARE(ServerMove::instanceIdFromFileId("storage$space!opaque"), QString());
        QCOMPARE(ServerMove::instanceIdFromFileId("00000123oc$bad"), QString());
        QCOMPARE(ServerMove::instanceIdFromFileId(QByteArray()), QString());
    }

    void testFileIdFromPropfind()
    {
        const QByteArray xml = "<?xml version=\"1.0\"?>\n"
            "<d:multistatus xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">\n"
            " <d:response><d:href>/remote.php/dav/files/admin/</d:href>\n"
            "  <d:propstat><d:prop><oc:id>00000005ocabcdefghij</oc:id></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>\n"
            " </d:response>\n"
            "</d:multistatus>";
        QCOMPARE(ServerMove::fileIdFromPropfind(xml), QByteArray("00000005ocabcdefghij"));
        QCOMPARE(ServerMove::fileIdFromPropfind("<d:multistatus xmlns:d=\"DAV:\"/>"), QByteArray());
        QCOMPARE(ServerMove::fileIdFromPropfind("kein xml"), QByteArray());
    }

    void testRebaseUrl()
    {
        const QUrl oldBase(QStringLiteral("https://alt.example.org/owncloud"));
        const QUrl newBase(QStringLiteral("https://neu.example.org"));
        QCOMPARE(ServerMove::rebaseUrl(QUrl(QStringLiteral("https://alt.example.org/owncloud/remote.php/dav/files/admin/")), oldBase, newBase),
            QUrl(QStringLiteral("https://neu.example.org/remote.php/dav/files/admin/")));
        QCOMPARE(ServerMove::rebaseUrl(QUrl(QStringLiteral("https://alt.example.org:443/owncloud/remote.php/dav/files/admin")), oldBase,
                     QUrl(QStringLiteral("https://neu.example.org/oc/"))),
            QUrl(QStringLiteral("https://neu.example.org/oc/remote.php/dav/files/admin")));
        // nicht unter der alten Adresse
        QVERIFY(!ServerMove::rebaseUrl(QUrl(QStringLiteral("https://alt.example.org/owncloud2/remote.php")), oldBase, newBase));
        QVERIFY(!ServerMove::rebaseUrl(QUrl(QStringLiteral("https://anders.example.org/owncloud/remote.php")), oldBase, newBase));
        QVERIFY(!ServerMove::rebaseUrl(QUrl(QStringLiteral("http://alt.example.org/owncloud/remote.php")), oldBase, newBase));
    }

    void testStatusCheck_data()
    {
        QTest::addColumn<int>("httpStatus");
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<bool>("ok");

        QTest::newRow("ok") << 200 << statusOk << true;
        QTest::newRow("installed as string") << 200 << QByteArray("{\"installed\":\"true\",\"maintenance\":\"false\",\"versionstring\":\"10.15.0\"}") << true;
        QTest::newRow("maintenance") << 200 << QByteArray("{\"installed\":true,\"maintenance\":true}") << false;
        QTest::newRow("upgrade pending") << 200 << QByteArray("{\"installed\":true,\"maintenance\":false,\"needsDbUpgrade\":true}") << false;
        QTest::newRow("not installed") << 200 << QByteArray("{\"installed\":false}") << false;
        QTest::newRow("no status") << 200 << QByteArray("<html>Hallo</html>") << false;
        QTest::newRow("not found") << 404 << QByteArray("not found") << false;
    }

    void testStatusCheck()
    {
        QFETCH(int, httpStatus);
        QFETCH(QByteArray, body);
        QFETCH(bool, ok);

        FakeAM accessManager({}, nullptr);
        QList<QNetworkRequest> requests;
        accessManager.setOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            requests.append(request);
            if (httpStatus == 200) {
                return new FakePayloadReply(op, request, body, &accessManager);
            }
            return new FakeErrorReply(op, request, &accessManager, httpStatus, body);
        });

        const auto result = runStatusCheck(&accessManager, QUrl(QStringLiteral("https://neu.example.org/owncloud")));
        QCOMPARE(result.ok, ok);
        QCOMPARE(result.error.isEmpty(), ok);
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.first().url(), QUrl(QStringLiteral("https://neu.example.org/owncloud/status.php")));
        // ohne Zugangsdaten und ohne automatische Weiterleitungen
        QVERIFY(!requests.first().hasRawHeader("Authorization"));
        QCOMPARE(requests.first().attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), static_cast<int>(QNetworkRequest::ManualRedirectPolicy));
        if (ok) {
            QCOMPARE(result.serverUrl, QUrl(QStringLiteral("https://neu.example.org/owncloud")));
        }
    }

    // Weiterleitungen auf demselben Host (hier HTTP zu HTTPS und in einen Unterordner) gelten
    void testStatusCheckFollowsSameHostRedirect()
    {
        FakeAM accessManager({}, nullptr);
        QStringList requested;
        accessManager.setOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            requested.append(request.url().toString());
            if (request.url() == QUrl(QStringLiteral("http://neu.example.org/status.php"))) {
                return new FakeRedirectReply(op, request, &accessManager, 301, QUrl(QStringLiteral("https://neu.example.org/status.php")));
            }
            if (request.url() == QUrl(QStringLiteral("https://neu.example.org/status.php"))) {
                // relativ angegeben
                return new FakeRedirectReply(op, request, &accessManager, 302, QUrl(QStringLiteral("/owncloud/status.php")));
            }
            return new FakePayloadReply(op, request, statusOk, &accessManager);
        });

        const auto result = runStatusCheck(&accessManager, QUrl(QStringLiteral("http://neu.example.org")));
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.serverUrl, QUrl(QStringLiteral("https://neu.example.org/owncloud")));
        QCOMPARE(result.version, QStringLiteral("11.0.0"));
        QCOMPARE(requested.size(), 3);
    }

    // eine Weiterleitung auf einen anderen Host wird gemeldet, aber nicht befolgt
    void testStatusCheckRefusesForeignRedirect()
    {
        FakeAM accessManager({}, nullptr);
        QStringList hosts;
        accessManager.setOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            hosts.append(request.url().host());
            if (request.url().host() == QLatin1String("neu.example.org")) {
                return new FakeRedirectReply(op, request, &accessManager, 308, QUrl(QStringLiteral("https://evil.example.com/oc/status.php")));
            }
            return new FakePayloadReply(op, request, statusOk, &accessManager);
        });

        const auto result = runStatusCheck(&accessManager, QUrl(QStringLiteral("https://neu.example.org")));
        QVERIFY(!result.ok);
        QCOMPARE(result.foreignRedirect, QUrl(QStringLiteral("https://evil.example.com/oc")));
        QVERIFY(result.error.contains(QStringLiteral("evil.example.com")));
        QCOMPARE(hosts, QStringList{QStringLiteral("neu.example.org")});
    }

    void testStatusCheckRedirectLimit()
    {
        FakeAM accessManager({}, nullptr);
        int requests = 0;
        accessManager.setOverride([&](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            ++requests;
            // im Kreis: /a → /b → /a …
            const bool atA = request.url().path().startsWith(QLatin1String("/a/"));
            return new FakeRedirectReply(op, request, &accessManager, 307, QUrl(atA ? QStringLiteral("/b/status.php") : QStringLiteral("/a/status.php")));
        });

        const auto result = runStatusCheck(&accessManager, QUrl(QStringLiteral("https://neu.example.org/a")));
        QVERIFY(!result.ok);
        QCOMPARE(requests, ServerStatusCheck::maxRedirects + 1);
    }

    // Umstellen: Konto, Ordnerpaare, Journale und Einstellungen ziehen mit, nichts entsteht neu
    void testApplyNewServerUrl()
    {
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QDir dir2(dir.path());
        QVERIFY(dir2.mkpath(QStringLiteral("docs")));
        QVERIFY(dir2.mkpath(QStringLiteral("photos")));
        const QString dirPath = dir2.canonicalPath();

        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        const QUuid uuid = account->uuid();
        FolderMan *folderMan = TestUtils::folderMan();

        Folder *docs = folderMan->addFolder(accountState.get(), oc10FolderDefinition(account, dirPath + QStringLiteral("/docs"), QStringLiteral("/A")));
        Folder *photos = folderMan->addFolder(accountState.get(), oc10FolderDefinition(account, dirPath + QStringLiteral("/photos"), QStringLiteral("/B")));
        QVERIFY(docs && photos);
        addSyncedFile(docs, "a1", "ocinstance01");
        docs->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, {QStringLiteral("sub/")});
        const QByteArray docsId = docs->id();

        const QUrl newUrl(QStringLiteral("https://neu.example.org/owncloud"));
        QCOMPARE(ServerMove::applyNewServerUrl(accountState.get(), newUrl, QStringLiteral("token"), QStringLiteral("refresh")), QString());

        QCOMPARE(account->url(), newUrl);
        QCOMPARE(account->davUrl(), QUrl(QStringLiteral("https://neu.example.org/owncloud/remote.php/dav/files/admin/")));

        const auto folders = folderMan->foldersForAccount(uuid);
        QCOMPARE(folders.size(), 2);
        Folder *newDocs = nullptr;
        for (Folder *folder : folders) {
            QVERIFY2(folder->webDavUrl().toString().startsWith(newUrl.toString() + QLatin1String("/remote.php/dav/files/admin")),
                qPrintable(folder->webDavUrl().toString()));
            if (folder->id() == docsId) {
                newDocs = folder;
            }
        }
        QVERIFY(newDocs);
        QCOMPARE(newDocs->remotePath(), QStringLiteral("/A"));
        QCOMPARE(newDocs->path(), dirPath + QStringLiteral("/docs/"));

        // das Journal bleibt: synchronisierte Einträge und Auswahl-Synchronisation sind noch da
        SyncJournalFileRecord record;
        QVERIFY(newDocs->journalDb()->getFileRecord(QByteArrayLiteral("a1"), &record));
        QVERIFY(record.isValid());
        QCOMPARE(record._fileId, QByteArray("00000042ocinstance01"));
        bool ok = false;
        QCOMPARE(newDocs->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, ok), QSet<QString>{QStringLiteral("sub/")});
        QVERIFY(ok);

        // die Einstellungen tragen die neue Adresse
        auto settings = ConfigFile::makeQSettings();
        settings.beginGroup(QStringLiteral("Accounts/%1").arg(account->groupIndex()));
        QCOMPARE(settings.value(QStringLiteral("url")).toUrl(), newUrl);
        settings.beginGroup(QStringLiteral("Folders/%1").arg(QString::fromUtf8(docsId)));
        QVERIFY(settings.value(QStringLiteral("davUrl")).toUrl().toString().startsWith(newUrl.toString()));
    }

    // derselbe Benutzer an einer anderen Installation: nichts wird umgestellt
    void testVerifyRejectsOtherInstance()
    {
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("docs")));

        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        const QUrl oldUrl = account->url();
        Folder *docs = TestUtils::folderMan()->addFolder(
            accountState.get(), oc10FolderDefinition(account, QDir(dir.path()).canonicalPath() + QStringLiteral("/docs"), QStringLiteral("/")));
        QVERIFY(docs);
        addSyncedFile(docs, "a1", "ocinstance01");
        QCOMPARE(ServerMove::knownInstanceId(account->uuid()), QStringLiteral("ocinstance01"));

        FileInfo otherServer = FileInfo::A12_B12_C12_S12();
        otherServer.fileId = "00000001ocother00001";
        auto *otherAm = new FakeAM(otherServer, nullptr);
        QList<QNetworkRequest> requests;
        otherAm->setOverride([&requests](QNetworkAccessManager::Operation, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
            requests.append(request);
            return nullptr;
        });

        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([otherAm](QObject *parent) -> QNetworkAccessManager * {
            otherAm->setParent(parent);
            return otherAm;
        });
        QSignalSpy failed(&controller, &ServerMoveController::failed);
        QSignalSpy succeeded(&controller, &ServerMoveController::succeeded);
        controller.verifyAndApply(QUrl(QStringLiteral("https://neu.example.org/owncloud")), QStringLiteral("new-token"), QStringLiteral("new-refresh"));
        QVERIFY(failed.wait());
        QCOMPARE(succeeded.size(), 0);
        QCOMPARE(account->url(), oldUrl);

        // geprüft wurde mit dem neuen Token an der neuen Adresse, ohne Weiterleitungen
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.first().url().host(), QStringLiteral("neu.example.org"));
        QCOMPARE(requests.first().rawHeader("Authorization"), QByteArray("Bearer new-token"));
        QCOMPARE(requests.first().attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(), static_cast<int>(QNetworkRequest::ManualRedirectPolicy));
    }

    void testVerifyAcceptsSameInstance()
    {
        auto dir = TestUtils::createTempDir();
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("docs")));

        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});
        Folder *docs = TestUtils::folderMan()->addFolder(
            accountState.get(), oc10FolderDefinition(account, QDir(dir.path()).canonicalPath() + QStringLiteral("/docs"), QStringLiteral("/")));
        QVERIFY(docs);
        addSyncedFile(docs, "a1", "ocinstance01");

        FileInfo sameServer = FileInfo::A12_B12_C12_S12();
        sameServer.fileId = "00000001ocinstance01";
        auto *sameAm = new FakeAM(sameServer, nullptr);

        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([sameAm](QObject *parent) -> QNetworkAccessManager * {
            sameAm->setParent(parent);
            return sameAm;
        });
        QSignalSpy failed(&controller, &ServerMoveController::failed);
        QSignalSpy succeeded(&controller, &ServerMoveController::succeeded);
        const QUrl newUrl(QStringLiteral("https://neu.example.org/owncloud"));
        controller.verifyAndApply(newUrl, QStringLiteral("new-token"), QStringLiteral("new-refresh"));
        QVERIFY(succeeded.wait());
        QCOMPARE(failed.size(), 0);
        QCOMPARE(account->url(), newUrl);
        QCOMPARE(TestUtils::folderMan()->foldersForAccount(account->uuid()).size(), 1);
    }

    // Eingaben, die gar nicht erst an einen Server gehen
    void testStartRejectsInvalidTargets()
    {
        auto accountState = createDummyAccountWithFileSupport(FileInfo::A12_B12_C12_S12());
        Account *account = accountState->account();
        account->setCapabilities({account->url(), oc10Capabilities()});

        int requests = 0;
        ServerMoveController controller(accountState.get(), nullptr);
        controller.setAccessManagerFactory([&requests](QObject *parent) -> QNetworkAccessManager * {
            auto *accessManager = new FakeAM({}, parent);
            accessManager->setOverride([&requests](QNetworkAccessManager::Operation, const QNetworkRequest &, QIODevice *) -> QNetworkReply * {
                ++requests;
                return nullptr;
            });
            return accessManager;
        });
        QSignalSpy failed(&controller, &ServerMoveController::failed);

        // dieselbe Adresse wie bisher
        controller.start(account->url().toString());
        QCOMPARE(failed.size(), 1);
        // Unsinn
        controller.start(QStringLiteral("https://user:pw@neu.example.org"));
        QCOMPARE(failed.size(), 2);
        QCOMPARE(requests, 0);
        QVERIFY(!controller.isBusy());
    }
};

QTEST_GUILESS_MAIN(TestServerMove)
#include "testservermove.moc"
