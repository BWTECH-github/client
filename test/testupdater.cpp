/*
   This software is in the public domain, furnished "as is", without technical
   support, and with no warranty, express or implied, as to its usefulness for
   any purpose.
*/

#include <QtTest>

#include "common/version.h"
#include "theme.h"
#include "updater/codesignature.h"
#include "updater/ocupdater.h"
#include "updater/updateinfo.h"
#include "updater/updater.h"

namespace OCC {

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
        const QUrl base(QStringLiteral("https://github.com/BWTECH-github/client/releases/latest/download/"));
        QCOMPARE(Updater::feedUrl(base, QStringLiteral("win-x64"), QStringLiteral("stable")).toString(),
            QStringLiteral("https://github.com/BWTECH-github/client/releases/latest/download/owncloud.online-update-win-x64-stable.xml"));
        // volle Adresse (z. B. OCC_UPDATE_URL) bleibt unverändert
        const QUrl full(QStringLiteral("http://127.0.0.1:8080/test.xml"));
        QCOMPARE(Updater::feedUrl(full, QStringLiteral("win-x64"), QStringLiteral("beta")), full);
        QVERIFY(Updater::feedUrl(QUrl(), QStringLiteral("win-x64"), QStringLiteral("beta")).isEmpty());
    }

    void testDefaultUpdateUrl()
    {
        QVERIFY(Theme::instance()->updateCheckUrl().toString().endsWith(QLatin1String("/releases/latest/download/")));
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
        QCOMPARE(required, QStringLiteral("BW-TECH GmbH"));
        QVERIFY(CodeSignature::signerMatches(QStringLiteral("BW-TECH GmbH"), required));
        QVERIFY(CodeSignature::signerMatches(QStringLiteral("BW-Tech GmbH"), required));
        QVERIFY(!CodeSignature::signerMatches(QStringLiteral("Microsoft Corporation"), required));
        QVERIFY(!CodeSignature::signerMatches(QStringLiteral("BW-TECH"), required));
        QVERIFY(!CodeSignature::signerMatches(QString(), required));
        QVERIFY(!CodeSignature::signerMatches(QStringLiteral("BW-TECH GmbH"), QString()));
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
            qInfo() << "foreign signed file" << candidate << r.signer;
            QVERIFY(!CodeSignature::signerMatches(r.signer, CodeSignature::requiredSigner()));
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
