/*
   This software is in the public domain, furnished "as is", without technical
   support, and with no warranty, express or implied, as to its usefulness for
   any purpose.
*/

#include <QtTest>

#include "common/version.h"
#include "configfile.h"
#include "theme.h"
#include "updater/codesignature.h"
#include "updater/ocupdater.h"
#include "updater/updateinfo.h"
#include "updater/updater.h"

#include <QPointer>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>

namespace OCC {

// Antwortet wie GitHub auf einen Release-Download: /feed.xml leitet mit 302 auf /asset.xml um, erst dort liegt der Feed
class FeedServer : public QTcpServer
{
public:
    QByteArray feed;
    QStringList requestedPaths;
    // hold: Anfragen annehmen, aber erst mit release() beantworten (ein Server, der hängt)
    bool hold = false;
    QList<QPointer<QTcpSocket>> held;

    FeedServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { answer(socket); });
            }
        });
    }

    void release()
    {
        for (const auto &socket : std::as_const(held)) {
            if (socket) {
                respond(socket, socket->property("path").toByteArray());
            }
        }
        held.clear();
    }

private:
    void answer(QTcpSocket *socket)
    {
        const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
        socket->setProperty("request", request);
        if (!request.contains("\r\n\r\n")) {
            return;
        }
        // "GET /feed.xml HTTP/1.1"
        const QByteArray path = request.split(' ').value(1);
        requestedPaths.append(QString::fromLatin1(path));
        if (hold) {
            socket->setProperty("path", path);
            held.append(socket);
            return;
        }
        respond(socket, path);
    }

    void respond(QTcpSocket *socket, const QByteArray &path)
    {
        QByteArray head;
        QByteArray body;
        if (path == "/feed.xml") {
            head = "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" + QByteArray::number(serverPort()) + "/asset.xml\r\n";
        } else if (path == "/asset.xml") {
            head = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n";
            body = feed;
        } else {
            head = "HTTP/1.1 404 Not Found\r\n";
        }
        socket->write(head + "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
};

// nimmt den gelesenen Feed entgegen, statt ein Update anzubieten
class FeedProbe : public OCUpdater
{
public:
    using OCUpdater::OCUpdater;
    using OCUpdater::setPreviouslySkippedVersion;
    QList<UpdateInfo> received;
    // die übersprungene Version, die für die jeweilige Prüfung galt
    QStringList skipped;

protected:
    void versionInfoArrived(const UpdateInfo &info) override
    {
        received.append(info);
        skipped.append(skippedVersionForThisCheck());
    }
};

class TestUpdater : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        // der Testschalter darf die Vorgabe nicht verdecken
        qunsetenv("OCC_UPDATE_URL");
    }

    void testFeedUrl()
    {
        const QUrl base(QStringLiteral("https://github.com/BWTECH-github/client/releases/download/update-feed/"));
        QCOMPARE(Updater::feedUrl(base, QStringLiteral("win-x64"), QStringLiteral("stable")).toString(),
            QStringLiteral("https://github.com/BWTECH-github/client/releases/download/update-feed/owncloud.online-update-win-x64-stable.xml"));
        // volle Adresse (z. B. OCC_UPDATE_URL) bleibt unverändert
        const QUrl full(QStringLiteral("http://127.0.0.1:8080/test.xml"));
        QCOMPARE(Updater::feedUrl(full, QStringLiteral("win-x64"), QStringLiteral("beta")), full);
        QVERIFY(Updater::feedUrl(QUrl(), QStringLiteral("win-x64"), QStringLiteral("beta")).isEmpty());
    }

    void testUpdateChannel_data()
    {
        QTest::addColumn<QString>("stored");
        QTest::addColumn<QString>("suffix");
        QTest::addColumn<QString>("expected");

        // ohne Wahl entscheidet das Versionssuffix
        QTest::newRow("release") << QString() << QString() << QStringLiteral("stable");
        QTest::newRow("beta build") << QString() << QStringLiteral("beta3") << QStringLiteral("beta");
        QTest::newRow("rc build") << QString() << QStringLiteral("rc1") << QStringLiteral("beta");
        QTest::newRow("daily build") << QString() << QStringLiteral("daily20261016") << QStringLiteral("beta");
        // die gespeicherte Wahl gilt in beide Richtungen
        QTest::newRow("beta chosen on release") << QStringLiteral("beta") << QString() << QStringLiteral("beta");
        QTest::newRow("stable chosen on beta") << QStringLiteral("stable") << QStringLiteral("beta3") << QStringLiteral("stable");
        // frühe Bauten schrieben ungefragt "ocis": zählt wie keine Wahl
        QTest::newRow("stale ocis value") << QStringLiteral("ocis") << QStringLiteral("beta3") << QStringLiteral("beta");
        QTest::newRow("unknown value") << QStringLiteral("nightly") << QString() << QStringLiteral("stable");
    }

    void testUpdateChannel()
    {
        QFETCH(QString, stored);
        QFETCH(QString, suffix);
        QFETCH(QString, expected);

        QCOMPARE(Updater::channel(stored, suffix), expected);
    }

    // Wahl im Über-Fenster: gespeichert und für die Feed-Adresse maßgeblich
    void testChosenChannelSelectsFeed()
    {
        const QString platform = Updater::feedPlatform();
        const QString otherChannel = Updater::defaultChannel(Version::suffix()) == QLatin1String("beta") ? QStringLiteral("stable") : QStringLiteral("beta");

        ConfigFile().setUpdateChannel(otherChannel);
        QCOMPARE(updateChannel(), otherChannel);
        QVERIFY(Updater::updateUrl().path().endsWith(Updater::feedFileName(platform, otherChannel)));
        QCOMPARE(QUrlQuery(Updater::updateUrl()).queryItemValue(QStringLiteral("channel")), otherChannel);

        // ohne Wahl wieder nach dem Versionssuffix
        ConfigFile().setUpdateChannel(QString());
        QCOMPARE(updateChannel(), Updater::defaultChannel(Version::suffix()));
    }

    void testCheckNow()
    {
        // Arrange: nach einem Kanalwechsel fragt dieselbe Updater-Instanz den neuen Feed
        FeedServer server;
        server.feed = QByteArrayLiteral("<owncloudclient><version>11.0.0-beta3</version></owncloudclient>");
        QVERIFY(server.listen(QHostAddress::LocalHost));
        FeedProbe updater(QUrl(QStringLiteral("http://127.0.0.1:%1/old-channel.xml").arg(server.serverPort())));
        updater.setUpdateUrl(QUrl(QStringLiteral("http://127.0.0.1:%1/feed.xml").arg(server.serverPort())));
        QVERIFY(updater.canCheckNow());

        // Act
        updater.checkNow();
        // eine laufende Prüfung lässt keine zweite zu
        QCOMPARE(updater.downloadState(), OCUpdater::CheckingServer);
        QVERIFY(!updater.canCheckNow());
        updater.checkNow();

        // Assert
        QTRY_COMPARE(updater.received.size(), 1);
        QCOMPARE(server.requestedPaths, QStringList({QStringLiteral("/feed.xml"), QStringLiteral("/asset.xml")}));

        // Prüfen auf Wunsch nur, wenn weder ein Download läuft noch ein Update bereitliegt
        for (const auto state : {OCUpdater::Unknown, OCUpdater::UpToDate, OCUpdater::DownloadFailed, OCUpdater::DownloadTimedOut,
                 OCUpdater::UpdateOnlyAvailableThroughSystem}) {
            updater.setDownloadState(state);
            QVERIFY2(updater.canCheckNow(), QMetaEnum::fromType<OCUpdater::DownloadState>().valueToKey(state));
        }
        for (const auto state : {OCUpdater::CheckingServer, OCUpdater::Downloading, OCUpdater::DownloadComplete}) {
            updater.setDownloadState(state);
            QVERIFY2(!updater.canCheckNow(), QMetaEnum::fromType<OCUpdater::DownloadState>().valueToKey(state));
        }
    }

    // Zeitüberschreitung: die hängende Anfrage wird abgebrochen. Käme ihre Antwort später doch noch, stoppte sie den
    // Wächter einer neuen Prüfung und würde doppelt ausgewertet (zwei Downloads in dieselbe Datei).
    void testTimeoutAbortsRequest()
    {
        // Arrange
        FeedServer server;
        server.feed = QByteArrayLiteral("<owncloudclient><version>11.0.0-beta3</version></owncloudclient>");
        server.hold = true;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        FeedProbe updater(QUrl(QStringLiteral("http://127.0.0.1:%1/feed.xml").arg(server.serverPort())));
        updater.checkForUpdate();
        QTRY_COMPARE(server.held.size(), 1);

        // Act
        QVERIFY(QMetaObject::invokeMethod(&updater, "slotTimedOut"));

        // Assert: abgebrochen, der Server sieht die Verbindung schließen
        QCOMPARE(updater.downloadState(), OCUpdater::DownloadTimedOut);
        QTRY_VERIFY(server.held.first().isNull());

        // eine neue Prüfung auf Wunsch kommt durch, die alte Antwort nie mehr
        server.hold = false;
        QVERIFY(updater.canCheckNow());
        updater.checkNow();
        QTRY_COMPARE(updater.received.size(), 1);
        server.release();
        QTest::qWait(500);
        QCOMPARE(updater.received.size(), 1);
    }

    // „Jetzt nach Updates suchen“ bietet auch eine früher übersprungene Version an; sonst hieße es „Keine Updates
    // verfügbar“, obwohl eine neuere Version existiert
    void testManualCheckIgnoresSkippedVersion()
    {
        FeedServer server;
        server.feed = QByteArrayLiteral("<owncloudclient><version>99.0.0</version></owncloudclient>");
        QVERIFY(server.listen(QHostAddress::LocalHost));
        FeedProbe updater(QUrl(QStringLiteral("http://127.0.0.1:%1/feed.xml").arg(server.serverPort())));
        FeedProbe::setPreviouslySkippedVersion(QStringLiteral("99.0.0"));
        const auto reset = qScopeGuard([] { FeedProbe::setPreviouslySkippedVersion(QString()); });

        // die regelmäßige Prüfung beachtet die übersprungene Version weiter
        updater.checkForUpdate();
        QTRY_COMPARE(updater.received.size(), 1);
        QCOMPARE(updater.skipped.last(), QStringLiteral("99.0.0"));

        updater.setDownloadState(OCUpdater::UpToDate);
        updater.checkNow();
        QTRY_COMPARE(updater.received.size(), 2);
        QCOMPARE(updater.skipped.last(), QString());
    }

    // Wer die Vorgabe ausdrücklich wählt (etwa „Beta“ auf einer Beta), behält sie auch nach dem finalen Release
    void testChoosingTheDefaultChannelPinsIt()
    {
        ConfigFile().setUpdateChannel(QString());
        const auto reset = qScopeGuard([] { ConfigFile().setUpdateChannel(QString()); });
        const QString current = updateChannel();
        const QString other = current == QLatin1String("beta") ? QStringLiteral("stable") : QStringLiteral("beta");

        QVERIFY(!Updater::setChannelChoice(current));
        QCOMPARE(ConfigFile().updateChannel(), current);
        // gilt unabhängig vom Versionssuffix
        QCOMPARE(Updater::channel(ConfigFile().updateChannel(), QString()), current);
        QCOMPARE(Updater::channel(ConfigFile().updateChannel(), QStringLiteral("beta3")), current);

        QVERIFY(Updater::setChannelChoice(other));
        QCOMPARE(updateChannel(), other);
    }

    // Kanalwechsel: ein bereitliegendes Update des alten Kanals wird verworfen, sonst installierte „Neu starten und
    // aktualisieren“ weiter dessen Bau (etwa die Beta nach dem Rückwechsel auf Stabil)
    void testChannelSwitchDiscardsDownloadedUpdate()
    {
        // Arrange: ein heruntergeladenes Setup des alten Kanals
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString setup = dir.filePath(QStringLiteral("owncloud.online-client-99.0.0-beta1-win-x64-Setup.exe"));
        {
            QFile file(setup);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("MZ");
        }
        {
            auto settings = ConfigFile::makeQSettings();
            settings.setValue(QStringLiteral("Updater/updateAvailable"), setup);
            settings.setValue(QStringLiteral("Updater/updateTargetVersion"), QStringLiteral("99.0.0-beta1"));
        }
        FeedServer server;
        // der neue Kanal hat nichts Neueres
        server.feed = QByteArrayLiteral("<owncloudclient><version>1.0.0</version></owncloudclient>");
        QVERIFY(server.listen(QHostAddress::LocalHost));
        WindowsUpdater updater(QUrl(QStringLiteral("http://127.0.0.1:%1/old-channel.xml").arg(server.serverPort())));
        updater.setDownloadState(OCUpdater::DownloadComplete);
        QVERIFY(!updater.canCheckNow());

        // Act
        updater.switchChannel(QUrl(QStringLiteral("http://127.0.0.1:%1/feed.xml").arg(server.serverPort())));

        // Assert
        QVERIFY(!QFile::exists(setup));
        QVERIFY(!ConfigFile::makeQSettings().contains(QStringLiteral("Updater/updateAvailable")));
        QTRY_COMPARE(updater.downloadState(), OCUpdater::UpToDate);
        QCOMPARE(server.requestedPaths.value(0), QStringLiteral("/feed.xml"));
    }

    void testSafeWebUrl_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("safe");
        QTest::newRow("release page") << QStringLiteral("https://github.com/BWTECH-github/client/releases/tag/v11.0.0") << true;
        QTest::newRow("http") << QStringLiteral("http://example.com/") << false;
        QTest::newRow("file") << QStringLiteral("file:///C:/Windows/System32/calc.exe") << false;
        QTest::newRow("user info") << QStringLiteral("https://user@example.com/") << false;
        QTest::newRow("no host") << QStringLiteral("https:///path") << false;
        QTest::newRow("empty") << QString() << false;
    }

    void testSafeWebUrl()
    {
        QFETCH(QString, url);
        QFETCH(bool, safe);
        QCOMPARE(OCUpdater::isSafeWebUrl(QUrl(url)), safe);
    }

    void testDefaultUpdateUrl()
    {
        // fester Ablageort aller Kanäle, nicht releases/latest (das wäre nur das neueste stabile Release)
        QCOMPARE(Theme::instance()->updateCheckUrl().toString(), QStringLiteral("https://github.com/BWTECH-github/client/releases/download/update-feed/"));
        const QString platform = Updater::feedPlatform();
#ifdef Q_OS_WIN
        if (QSysInfo::buildCpuArchitecture() == QLatin1String("x86_64")) {
            QCOMPARE(platform, QStringLiteral("win-x64"));
        }
#elif defined(Q_OS_MACOS)
        QVERIFY(platform.startsWith(QLatin1String("macos-")));
#else
        QVERIFY(platform.startsWith(QLatin1String("linux-")));
#endif
        const QString channel = updateChannel();
        QVERIFY(channel == QLatin1String("stable") || channel == QLatin1String("beta"));
        const QUrl url = Updater::updateUrl();
        QCOMPARE(url.scheme(), QStringLiteral("https"));
        QVERIFY(url.path().endsWith(Updater::feedFileName(platform, channel)));
        QVERIFY(!QUrlQuery(url).hasQueryItem(QStringLiteral("msi")));
    }

    void testParseFeed()
    {
        // Format wie admin/release/make-update-feed.py
        const QString xml = QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<owncloudclient>\n"
            "  <version>11.0.0</version>\n"
            "  <versionstring>owncloud.online 11.0.0</versionstring>\n"
            "  <web>https://github.com/BWTECH-github/client/releases/tag/v11.0.0</web>\n"
            "  <downloadurl>https://github.com/BWTECH-github/client/releases/download/v11.0.0/owncloud.online-client-11.0.0-win-x64-Setup.exe</downloadurl>\n"
            "</owncloudclient>\n");
        bool ok = false;
        const UpdateInfo info = UpdateInfo::parseString(xml, &ok);
        QVERIFY(ok);
        QCOMPARE(info.version(), QStringLiteral("11.0.0"));
        QCOMPARE(info.versionString(), QStringLiteral("owncloud.online 11.0.0"));
        QCOMPARE(info.web(), QStringLiteral("https://github.com/BWTECH-github/client/releases/tag/v11.0.0"));
        QVERIFY(OCUpdater::isTrustedDownloadUrl(QUrl(info.downloadUrl())));

        // Feed ohne Download (macOS/Linux) bleibt gültig
        UpdateInfo::parseString(QStringLiteral("<owncloudclient><version>11.0.0</version></owncloudclient>"), &ok);
        QVERIFY(ok);

        UpdateInfo::parseString(QStringLiteral("<html>404</html>"), &ok);
        QVERIFY(!ok);
        UpdateInfo::parseString(QStringLiteral("not xml"), &ok);
        QVERIFY(!ok);
    }

    // GitHub beantwortet jeden Release-Download mit 302 auf release-assets.githubusercontent.com. Bisher kam nur diese
    // leere Weiterleitung an („Could not parse update information“), kein Client fand je ein Update.
    void testFeedBehindRedirect()
    {
        // Arrange
        FeedServer server;
        server.feed = QByteArrayLiteral("<owncloudclient><version>11.0.0-beta3</version><versionstring>owncloud.online 11.0.0-beta3</versionstring>"
                                        "</owncloudclient>");
        QVERIFY(server.listen(QHostAddress::LocalHost));
        FeedProbe updater(QUrl(QStringLiteral("http://127.0.0.1:%1/feed.xml").arg(server.serverPort())));

        // Act
        updater.checkForUpdate();

        // Assert
        QTRY_COMPARE(updater.received.size(), 1);
        QCOMPARE(updater.received.first().version(), QStringLiteral("11.0.0-beta3"));
        QCOMPARE(server.requestedPaths, QStringList({QStringLiteral("/feed.xml"), QStringLiteral("/asset.xml")}));
    }

    // Gegenprobe gegen GitHub selbst, nur auf Anforderung, weil sie das Netz braucht, z. B.
    // OCC_TEST_LIVE_FEED_URL=https://github.com/BWTECH-github/client/releases/download/v11.0.0-beta1/owncloud.online-update-win-x64-beta.xml
    void testPublishedFeed()
    {
        const QUrl url(qEnvironmentVariable("OCC_TEST_LIVE_FEED_URL"));
        if (url.isEmpty()) {
            QSKIP("OCC_TEST_LIVE_FEED_URL not set");
        }
        FeedProbe updater(url);

        updater.checkForUpdate();

        QTRY_COMPARE_WITH_TIMEOUT(updater.received.size(), 1, 30000);
        qInfo() << "published feed offers" << updater.received.first().version() << updater.received.first().downloadUrl();
        QVERIFY(!updater.received.first().version().isEmpty());
    }

    void testVersionCompare_data()
    {
        QTest::addColumn<QString>("available");
        QTest::addColumn<QString>("installed");
        QTest::addColumn<QString>("skipped");
        QTest::addColumn<bool>("newer");
        QTest::newRow("patch release") << "11.0.1" << "11.0.0.1234" << "" << true;
        QTest::newRow("same release, build number installed") << "11.0.0" << "11.0.0.1234" << "" << false;
        QTest::newRow("equal") << "11.0.0" << "11.0.0" << "" << false;
        QTest::newRow("older") << "7.11.1" << "11.0.0.5" << "" << false;
        QTest::newRow("jump from 7.x to 11") << "11.0.0" << "7.11.1.100" << "" << true;
        QTest::newRow("skipped") << "11.0.1" << "11.0.0" << "11.0.1" << false;
        QTest::newRow("newer than skipped") << "11.0.2" << "11.0.0" << "11.0.1" << true;
        QTest::newRow("empty") << "" << "11.0.0" << "" << false;
        QTest::newRow("garbage") << "garbage" << "1.0.0" << "" << false;
        // installiert wie OCUpdater::installedVersion(): Baunummer 0, Suffix aus VERSION.cmake
        QTest::newRow("beta1 to final") << "11.0.0" << "11.0.0.0-beta1" << "" << true;
        QTest::newRow("final to final+1") << "11.0.1" << "11.0.0.0" << "" << true;
        QTest::newRow("same version") << "11.0.0" << "11.0.0.0" << "" << false;
        QTest::newRow("same pre-release") << "11.0.0-beta1" << "11.0.0.0-beta1" << "" << false;
        QTest::newRow("next pre-release") << "11.0.0-beta2" << "11.0.0.0-beta1" << "" << true;
        QTest::newRow("pre-release numbers compare as numbers") << "11.0.0-beta10" << "11.0.0.0-beta2" << "" << true;
        QTest::newRow("rc after beta") << "11.0.0-rc1" << "11.0.0.0-beta2" << "" << true;
        QTest::newRow("older pre-release") << "11.0.0-beta2" << "11.0.0.0-rc1" << "" << false;
        QTest::newRow("pre-release of the installed final") << "11.0.0-rc1" << "11.0.0.0" << "" << false;
        QTest::newRow("pre-release of the next version") << "11.0.1-beta1" << "11.0.0.0" << "" << true;
        QTest::newRow("final skipped") << "11.0.0" << "11.0.0.0-beta1" << "11.0.0" << false;
        QTest::newRow("final after a skipped pre-release") << "11.0.0" << "11.0.0.0-beta1" << "11.0.0-beta2" << true;
        QTest::newRow("unreadable skipped version") << "11.0.1" << "11.0.0" << "garbage" << true;
    }

    void testVersionCompare()
    {
        QFETCH(QString, available);
        QFETCH(QString, installed);
        QFETCH(QString, skipped);
        QFETCH(bool, newer);
        QCOMPARE(OCUpdater::isNewerVersion(available, installed, skipped), newer);
    }

    void testInstalledVersion()
    {
        const QString installed = OCUpdater::installedVersion();
        QVERIFY(installed.startsWith(Version::versionWithBuildNumber().toString()));
        QCOMPARE(installed.endsWith(QLatin1Char('-') + Version::suffix()), !Version::suffix().isEmpty());
        // der Feed des eigenen Releases (make-update-feed.py: Version aus dem Tag) bietet nichts an
        QVERIFY(!OCUpdater::isNewerVersion(Version::displayString(), installed, {}));
        QVERIFY(OCUpdater::compareVersions(QStringLiteral("11.0.0-beta1"), QStringLiteral("11.0.0")) < 0);
        QCOMPARE(OCUpdater::compareVersions(QStringLiteral("11.0.0-RC1"), QStringLiteral("11.0.0-rc01")), 0);
    }

    void testTrustedDownloadUrl_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("trusted");
        const QString ok = QStringLiteral("https://github.com/BWTECH-github/client/releases/download/v11.0.0/owncloud.online-client-11.0.0-win-x64-Setup.exe");
        QTest::newRow("release asset") << ok << true;
        QTest::newRow("http") << QString(ok).replace(QLatin1String("https:"), QLatin1String("http:")) << false;
        QTest::newRow("other host") << QString(ok).replace(QLatin1String("github.com"), QLatin1String("example.com")) << false;
        QTest::newRow("other repo") << QString(ok).replace(QLatin1String("BWTECH-github/client"), QLatin1String("evil/client")) << false;
        QTest::newRow("msi") << QString(ok).replace(QLatin1String("Setup.exe"), QLatin1String("x64.msi")) << false;
        QTest::newRow("user info") << QString(ok).replace(QLatin1String("https://"), QLatin1String("https://a@")) << false;
        QTest::newRow("port") << QString(ok).replace(QLatin1String("github.com"), QLatin1String("github.com:8443")) << false;
        QTest::newRow("empty") << QString() << false;
    }

    void testTrustedDownloadUrl()
    {
        QFETCH(QString, url);
        QFETCH(bool, trusted);
        QCOMPARE(OCUpdater::isTrustedDownloadUrl(QUrl(url)), trusted);
    }

    void testSignerMatches()
    {
        const QString required = CodeSignature::requiredSigner();
        QCOMPARE(required, QString::fromUtf8("BW-TECH GmbH, IT Service für Systemintegration & Datensicherheit"));
        const QString digiCert = QStringLiteral("DigiCert Trusted G4 Code Signing RSA4096 SHA384 2021 CA1");
        QVERIFY(CodeSignature::signerMatches(required, digiCert));
        // dasselbe "ü" zerlegt (u + U+0308)
        QVERIFY(required.normalized(QString::NormalizationForm_D) != required);
        QVERIFY(CodeSignature::signerMatches(required.normalized(QString::NormalizationForm_D), digiCert));
        // fremde CNs, die unseren Namen nur enthalten oder anders schreiben
        QVERIFY(!CodeSignature::signerMatches(QStringLiteral("XYZ BW-TECH GmbH Holding"), digiCert));
        QVERIFY(!CodeSignature::signerMatches(QStringLiteral("BW-TECH GmbH"), digiCert));
        QVERIFY(!CodeSignature::signerMatches(QStringLiteral("XYZ ") + required, digiCert));
        QVERIFY(!CodeSignature::signerMatches(required + QStringLiteral(" Holding"), digiCert));
        QVERIFY(!CodeSignature::signerMatches(required.toUpper(), digiCert));
        QVERIFY(!CodeSignature::signerMatches(QString(required).replace(QChar(0xfc), QLatin1Char('u')), digiCert));
        // richtiger CN, aber nicht von DigiCert ausgestellt
        QVERIFY(!CodeSignature::signerMatches(required, QStringLiteral("Sectigo Public Code Signing CA R36")));
        QVERIFY(!CodeSignature::signerMatches(required, QString()));
        QVERIFY(!CodeSignature::signerMatches(QString(), digiCert));
    }

    void testSignatureRejection()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        // unsigniertes "Setup" wird abgelehnt und gelöscht
        const QString unsignedFile = dir.filePath(QStringLiteral("owncloud.online-client-99.0.0-win-x64-Setup.exe"));
        {
            QFile f(unsignedFile);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray("MZ") + QByteArray(4096, '\0'));
        }
        QString error;
        QVERIFY(!CodeSignature::verifyUpdatePackage(unsignedFile, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!WindowsUpdater::verifyDownloadedSetup(unsignedFile));
        QVERIFY(!QFile::exists(unsignedFile));

        QVERIFY(!CodeSignature::verifyUpdatePackage(dir.filePath(QStringLiteral("missing.exe")), &error));
    }

    void testForeignSignerRejected()
    {
#ifndef Q_OS_WIN
        QSKIP("Authenticode only on Windows");
#else
        // gültig signiert, aber nicht von uns: muss abgelehnt werden
        const QStringList candidates = {QStringLiteral("C:/Program Files/PowerShell/7/pwsh.exe"), QStringLiteral("C:/Program Files/Git/cmd/git.exe"),
            QStringLiteral("C:/Program Files/nodejs/node.exe"), QStringLiteral("C:/Program Files/GitHub CLI/gh.exe")};
        for (const auto &candidate : candidates) {
            const auto r = CodeSignature::verifyFile(candidate);
            if (!r.valid) {
                continue;
            }
            qInfo() << "foreign signed file" << candidate << r.signer << r.issuer;
            QVERIFY(!r.issuer.isEmpty());
            QVERIFY(!CodeSignature::signerMatches(r.signer, r.issuer));
            QString error;
            QVERIFY(!CodeSignature::verifyUpdatePackage(candidate, &error));
            QVERIFY(error.contains(QLatin1String("unexpected signer")));
            return;
        }
        QSKIP("no validly signed foreign executable found");
#endif
    }
};
}

QTEST_MAIN(OCC::TestUpdater)
#include "testupdater.moc"
