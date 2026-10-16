/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// owncloudcmd: Wirkung der Optionen aus 6.0.3 in einem echten Lauf gegen den Testserver und der Löschschutz
// (ohne Rückfrage Abbruch mit Exit-Code 3, mit Rückfrage löschen oder behalten)

#include "account.h"
#include "bandwidthmanager.h"
#include "cmdsession.h"
#include "owncloudpropagator.h"
#include "testutils/syncenginetestutils.h"
#include "testutils/testutils.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

using namespace OCC;

namespace {

const QUrl serverUrlC(QStringLiteral("http://localhost/owncloud"));

QByteArray ocsAnswer(const QJsonObject &data)
{
    const QJsonObject meta{{QStringLiteral("status"), QStringLiteral("ok")}, {QStringLiteral("statuscode"), 200}};
    return QJsonDocument(QJsonObject{{QStringLiteral("ocs"), QJsonObject{{QStringLiteral("meta"), meta}, {QStringLiteral("data"), data}}}})
        .toJson(QJsonDocument::Compact);
}

// Testserver: status.php und OCS beantwortet die Übersteuerung, WebDAV der FakeAM selbst
FakeAM *createServer(const FileInfo &remote)
{
    auto *am = new FakeAM(remote, nullptr);
    am->setOverride([am](QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *) -> QNetworkReply * {
        const QString path = request.url().path();
        if (path.endsWith(QLatin1String("/status.php"))) {
            const QJsonObject status{{QStringLiteral("installed"), true}, {QStringLiteral("maintenance"), false},
                {QStringLiteral("needsDbUpgrade"), false}, {QStringLiteral("version"), QStringLiteral("10.5.0.10")},
                {QStringLiteral("versionstring"), QStringLiteral("10.5.0")}, {QStringLiteral("edition"), QStringLiteral("Community")},
                {QStringLiteral("productname"), QStringLiteral("owncloud.online")}};
            return new FakePayloadReply(op, request, QJsonDocument(status).toJson(QJsonDocument::Compact), am);
        }
        if (path.endsWith(QLatin1String("/ocs/v2.php/cloud/user"))) {
            return new FakePayloadReply(op, request, ocsAnswer({{QStringLiteral("id"), QStringLiteral("admin")}}), am);
        }
        if (path.endsWith(QLatin1String("/ocs/v2.php/cloud/capabilities"))) {
            const auto capabilities = QJsonObject::fromVariantMap(TestUtils::testCapabilities());
            return new FakePayloadReply(op, request, ocsAnswer({{QStringLiteral("capabilities"), capabilities}}), am);
        }
        return nullptr;
    });
    return am;
}

// Anmeldung ohne Schlüsselbund: sofort bereit
class TestCredentials : public FakeCredentials
{
public:
    TestCredentials(Account *account, FakeAM *am)
        : FakeCredentials(account, am, account)
    {
    }
    void fetchFromKeychain() override { Q_EMIT fetched(); }
};

// Ein Lauf wie ein Aufruf von owncloudcmd; remote ist danach der Stand des Servers
int runCmd(FileInfo &remote, const QString &localDir, const CmdSession::Settings &settings)
{
    FakeAM *server = nullptr;
    CmdSession session(serverUrlC, QStringLiteral("admin"), localDir, QStringLiteral("/"),
        [&server, &remote](Account *account) -> AbstractCredentials * {
            server = createServer(remote);
            return new TestCredentials(account, server);
        },
        settings);
    QSignalSpy finishedSpy(&session, &CmdSession::finished);
    session.start();
    if (!finishedSpy.wait(30000)) {
        return -1;
    }
    remote = server->currentRemoteState();
    return finishedSpy.first().first().toInt();
}

void writeFile(const QString &path, const QByteArray &content = QByteArrayLiteral("owncloud.online"))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
}

QString localDirOf(const QTemporaryDir &dir)
{
    const QString local = dir.filePath(QStringLiteral("local/"));
    QDir().mkpath(local);
    return local;
}

}

class TestCmdSync : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // --exclude: zusätzliche Ausschlussliste
    void testExcludeList()
    {
        const auto dir = TestUtils::createTempDir();
        const QString local = localDirOf(dir);
        writeFile(local + QStringLiteral("Bericht.txt"));
        writeFile(local + QStringLiteral("Entwurf.tmpx"));
        const QString excludeList = dir.filePath(QStringLiteral("exclude.lst"));
        writeFile(excludeList, QByteArrayLiteral("*.tmpx\n"));

        FileInfo remote;
        CmdSession::Settings settings;
        settings.excludeFile = excludeList;
        QCOMPARE(runCmd(remote, local, settings), static_cast<int>(CmdSession::ExitSuccess));
        QVERIFY(remote.find(QStringLiteral("Bericht.txt")));
        QVERIFY(!remote.find(QStringLiteral("Entwurf.tmpx")));

        // ohne die Liste geht die Datei hoch
        QCOMPARE(runCmd(remote, local, {}), static_cast<int>(CmdSession::ExitSuccess));
        QVERIFY(remote.find(QStringLiteral("Entwurf.tmpx")));
    }

    // --unsyncedfolders: abgewählte Serverordner kommen nicht herunter, erneut gewählte schon
    void testUnsyncedFolders()
    {
        const auto dir = TestUtils::createTempDir();
        const QString local = localDirOf(dir);
        FileInfo remote = FileInfo::A12_B12_C12_S12();

        CmdSession::Settings settings;
        settings.unsyncedFolders = QSet<QString>{QStringLiteral("B/"), QStringLiteral("C/")};
        QCOMPARE(runCmd(remote, local, settings), static_cast<int>(CmdSession::ExitSuccess));
        QVERIFY(QFileInfo::exists(local + QStringLiteral("A/a1")));
        QVERIFY(!QFileInfo::exists(local + QStringLiteral("B")));
        QVERIFY(!QFileInfo::exists(local + QStringLiteral("C")));
        // auf dem Server bleibt alles
        QVERIFY(remote.find(QStringLiteral("B/b1")));

        settings.unsyncedFolders = QSet<QString>{QStringLiteral("C/")};
        QCOMPARE(runCmd(remote, local, settings), static_cast<int>(CmdSession::ExitSuccess));
        QVERIFY(QFileInfo::exists(local + QStringLiteral("B/b1")));
        QVERIFY(!QFileInfo::exists(local + QStringLiteral("C")));
    }

    // wie 6.0.3: versteckte Dateien nur mit --sync-hidden-files
    void testHiddenFiles()
    {
        const auto dir = TestUtils::createTempDir();
        const QString local = localDirOf(dir);
        writeFile(local + QStringLiteral("sichtbar.txt"));
        writeFile(local + QStringLiteral(".versteckt"));

        FileInfo remote;
        QCOMPARE(runCmd(remote, local, {}), static_cast<int>(CmdSession::ExitSuccess));
        QVERIFY(remote.find(QStringLiteral("sichtbar.txt")));
        QVERIFY(!remote.find(QStringLiteral(".versteckt")));

        CmdSession::Settings settings;
        settings.ignoreHiddenFiles = false;
        QCOMPARE(runCmd(remote, local, settings), static_cast<int>(CmdSession::ExitSuccess));
        QVERIFY(remote.find(QStringLiteral(".versteckt")));
    }

    // Lokaler Ordner leer (z. B. Laufwerk nicht eingehängt): ohne Rückfrage wird nichts gelöscht
    void testRemoveAllNonInteractive()
    {
        const auto dir = TestUtils::createTempDir();
        const QString local = localDirOf(dir);
        FileInfo remote = FileInfo::A12_B12_C12_S12();
        QCOMPARE(runCmd(remote, local, {}), static_cast<int>(CmdSession::ExitSuccess));

        for (const auto &name : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("S")}) {
            QVERIFY(QDir(local + name).removeRecursively());
        }
        QCOMPARE(runCmd(remote, local, {}), static_cast<int>(CmdSession::ExitRemoveAllNotConfirmed));
        QCOMPARE(remote.children.size(), qsizetype{4});
        QVERIFY(remote.find(QStringLiteral("S/s2")));
    }

    void testRemoveAllAnswer_data()
    {
        QTest::addColumn<bool>("confirm");
        QTest::newRow("delete") << true;
        QTest::newRow("keep") << false;
    }

    // Mit Rückfrage: „ja“ löscht auf dem Server, „nein“ ändert nichts
    void testRemoveAllAnswer()
    {
        QFETCH(bool, confirm);
        const auto dir = TestUtils::createTempDir();
        const QString local = localDirOf(dir);
        FileInfo remote = FileInfo::A12_B12_C12_S12();
        QCOMPARE(runCmd(remote, local, {}), static_cast<int>(CmdSession::ExitSuccess));
        for (const auto &name : {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C"), QStringLiteral("S")}) {
            QVERIFY(QDir(local + name).removeRecursively());
        }

        QStringList questions;
        CmdSession::Settings settings;
        settings.confirmRemoveAll = [&questions, confirm](const QString &message) {
            questions.append(message);
            return confirm;
        };
        const int exitCode = runCmd(remote, local, settings);

        QCOMPARE(questions.size(), 1);
        QVERIFY2(questions.first().startsWith(QLatin1String("All files in the local folder")), qPrintable(questions.first()));
        if (confirm) {
            QCOMPARE(exitCode, static_cast<int>(CmdSession::ExitSuccess));
            QCOMPARE(remote.children.size(), qsizetype{0});
        } else {
            QCOMPARE(exitCode, static_cast<int>(CmdSession::ExitRemoveAllNotConfirmed));
            QCOMPARE(remote.children.size(), qsizetype{4});
        }
    }

    // --uplimit/--downlimit: die Grenzen des Laufs gehen den Netzwerk-Einstellungen vor und erreichen die Drosselung
    void testLimitsReachThePropagator()
    {
        FakeFolder fakeFolder{FileInfo{}};
        SyncOptions options = fakeFolder.syncEngine()->syncOptions();
        options._uploadLimit = 100 * 1024;
        options._downloadLimit = 250 * 1024;
        OwncloudPropagator propagator(
            fakeFolder.account(), options, fakeFolder.account()->davUrl(), fakeFolder.localPath(), QString(), fakeFolder.syncJournal(), nullptr);
        propagator.start(SyncFileItemSet{});
        QVERIFY(propagator._bandwidthManager);
        QCOMPARE(propagator._bandwidthManager->currentUploadLimit(), qint64{100 * 1024});
        QCOMPARE(propagator._bandwidthManager->currentDownloadLimit(), qint64{250 * 1024});
    }

    // Mit Grenze kommt die Datei vollständig an
    void testDownloadWithLimit()
    {
        const auto dir = TestUtils::createTempDir();
        const QString local = localDirOf(dir);
        FileInfo remote;
        remote.insert(QStringLiteral("runter.bin"), quint64{300 * 1024});

        CmdSession::Settings settings;
        settings.downloadLimit = 100 * 1024;
        QCOMPARE(runCmd(remote, local, settings), static_cast<int>(CmdSession::ExitSuccess));
        QCOMPARE(QFileInfo(local + QStringLiteral("runter.bin")).size(), qint64{300 * 1024});
    }
};

QTEST_GUILESS_MAIN(TestCmdSync)
#include "testcmdsync.moc"
