/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

// owncloudcmd: Befehlszeile mit den Optionen des 6.0.3-Clients

#include "cmdoptions.h"
#include "testutils/testutils.h"

#include <QtTest>

using namespace OCC;

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(content), content.size());
}

}

class TestCmdOptions : public QObject
{
    Q_OBJECT

    QTemporaryDir _dir = TestUtils::createTempDir();

    QString local() const { return _dir.path(); }

    CmdOptionsResult parse(const QStringList &arguments, const QProcessEnvironment &environment = {})
    {
        return parseCmdOptions(QStringList{QStringLiteral("owncloudcmd")} + arguments, environment);
    }

private Q_SLOTS:
    void testDefaults()
    {
        const auto result = parse({QStringLiteral("-u"), QStringLiteral("anna"), QStringLiteral("-p"), QStringLiteral("geheim"), local(),
            QStringLiteral("https://cloud.example.com")});
        QCOMPARE(result.action, CmdOptionsResult::Action::Run);
        const auto &options = result.options;
        QCOMPARE(options.localDir, QDir(local()).absolutePath() + QLatin1Char('/'));
        QCOMPARE(options.serverUrl, QUrl(QStringLiteral("https://cloud.example.com")));
        QCOMPARE(options.remotePath, QStringLiteral("/"));
        QCOMPARE(options.user, QStringLiteral("anna"));
        QVERIFY(options.hasPassword);
        QCOMPARE(options.password, QStringLiteral("geheim"));
        QVERIFY(options.tokenFile.isEmpty());
        QVERIFY(options.interactive);
        QVERIFY(options.ignoreHiddenFiles);
        QVERIFY(!options.trustSsl);
        QVERIFY(!options.silent);
        QVERIFY(!options.logDebug);
        QVERIFY(!options.proxy);
        QVERIFY(options.excludeFile.isEmpty());
        QVERIFY(!options.unsyncedFolders);
        QCOMPARE(options.maxSyncRetries, 3);
        QCOMPARE(options.uploadLimit, qint64{0});
        QCOMPARE(options.downloadLimit, qint64{0});
    }

    void testAllOptionsOf603()
    {
        const QString exclude = _dir.filePath(QStringLiteral("exclude.lst"));
        writeFile(exclude, QByteArrayLiteral("*.tmp\n"));
        const QString unsynced = _dir.filePath(QStringLiteral("unsynced.lst"));
        writeFile(unsynced, QByteArrayLiteral("Fotos\n"));

        const auto result = parse({QStringLiteral("--silent"), QStringLiteral("--httpproxy"), QStringLiteral("http://proxy.example.com:3128"),
            QStringLiteral("--trust"), QStringLiteral("--exclude"), exclude, QStringLiteral("--unsyncedfolders"), unsynced,
            QStringLiteral("--non-interactive"), QStringLiteral("--max-sync-retries"), QStringLiteral("5"), QStringLiteral("--uplimit"),
            QStringLiteral("100"), QStringLiteral("--downlimit"), QStringLiteral("250"), QStringLiteral("--sync-hidden-files"),
            QStringLiteral("--logdebug"), QStringLiteral("-u"), QStringLiteral("anna"), QStringLiteral("-p"), QStringLiteral("geheim"), local(),
            QStringLiteral("https://cloud.example.com/"), QStringLiteral("Dokumente")});
        QCOMPARE(result.action, CmdOptionsResult::Action::Run);
        const auto &options = result.options;
        QVERIFY(options.silent);
        QVERIFY(options.proxy);
        QCOMPARE(options.proxy->type(), QNetworkProxy::HttpProxy);
        QCOMPARE(options.proxy->hostName(), QStringLiteral("proxy.example.com"));
        QCOMPARE(options.proxy->port(), quint16{3128});
        QVERIFY(options.trustSsl);
        QCOMPARE(options.excludeFile, QFileInfo(exclude).absoluteFilePath());
        QCOMPARE(options.unsyncedFolders, QSet<QString>{QStringLiteral("Fotos/")});
        QVERIFY(!options.interactive);
        QCOMPARE(options.maxSyncRetries, 5);
        QCOMPARE(options.uploadLimit, qint64{100 * 1024});
        QCOMPARE(options.downloadLimit, qint64{250 * 1024});
        QVERIFY(!options.ignoreHiddenFiles);
        QVERIFY(options.logDebug);
        // dritter Parameter wie in 6.0.3
        QCOMPARE(options.remotePath, QStringLiteral("/Dokumente"));
    }

    void testProxy_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("valid");
        QTest::addColumn<int>("type");
        QTest::addColumn<QString>("host");
        QTest::addColumn<int>("port");
        QTest::newRow("http") << QStringLiteral("http://proxy:8080") << true << int(QNetworkProxy::HttpProxy) << QStringLiteral("proxy") << 8080;
        QTest::newRow("ohne Schema") << QStringLiteral("proxy:8080") << true << int(QNetworkProxy::HttpProxy) << QStringLiteral("proxy") << 8080;
        QTest::newRow("socks5") << QStringLiteral("socks5://10.0.0.1:1080") << true << int(QNetworkProxy::Socks5Proxy) << QStringLiteral("10.0.0.1") << 1080;
        QTest::newRow("ohne Port") << QStringLiteral("http://proxy") << false << 0 << QString() << 0;
        QTest::newRow("ohne Host") << QStringLiteral("http://:8080") << false << 0 << QString() << 0;
        QTest::newRow("ftp") << QStringLiteral("ftp://proxy:21") << false << 0 << QString() << 0;
        QTest::newRow("Port zu groß") << QStringLiteral("http://proxy:70000") << false << 0 << QString() << 0;
    }

    void testProxy()
    {
        QFETCH(QString, value);
        QFETCH(bool, valid);
        QString error;
        const auto proxy = parseCmdProxy(value, &error);
        QCOMPARE(proxy.has_value(), valid);
        if (valid) {
            QFETCH(int, type);
            QFETCH(QString, host);
            QFETCH(int, port);
            QCOMPARE(int(proxy->type()), type);
            QCOMPARE(proxy->hostName(), host);
            QCOMPARE(int(proxy->port()), port);
        } else {
            QVERIFY(!error.isEmpty());
        }
    }

    void testProxyCredentials()
    {
        QString error;
        const auto proxy = parseCmdProxy(QStringLiteral("http://nutzer:kennwort@proxy:3128"), &error);
        QVERIFY(proxy);
        QCOMPARE(proxy->user(), QStringLiteral("nutzer"));
        QCOMPARE(proxy->password(), QStringLiteral("kennwort"));
    }

    void testInvalidValues_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::newRow("uplimit negativ") << QStringList{QStringLiteral("--uplimit"), QStringLiteral("-1")};
        QTest::newRow("downlimit Text") << QStringList{QStringLiteral("--downlimit"), QStringLiteral("viel")};
        QTest::newRow("retries Text") << QStringList{QStringLiteral("--max-sync-retries"), QStringLiteral("x")};
        QTest::newRow("Proxy") << QStringList{QStringLiteral("--httpproxy"), QStringLiteral("nur-ein-name")};
        QTest::newRow("exclude fehlt") << QStringList{QStringLiteral("--exclude"), QStringLiteral("gibt-es-nicht.lst")};
        QTest::newRow("unsyncedfolders fehlt") << QStringList{QStringLiteral("--unsyncedfolders"), QStringLiteral("gibt-es-nicht.lst")};
        QTest::newRow("unbekannte Option") << QStringList{QStringLiteral("--gibt-es-nicht")};
    }

    void testInvalidValues()
    {
        QFETCH(QStringList, arguments);
        const auto result = parse(arguments + QStringList{QStringLiteral("-u"), QStringLiteral("anna"), local(), QStringLiteral("https://cloud.example.com")});
        QCOMPARE(result.action, CmdOptionsResult::Action::Error);
        QVERIFY(!result.error.isEmpty());
    }

    void testUnsyncedFoldersFile()
    {
        const QString unsynced = _dir.filePath(QStringLiteral("liste.lst"));
        writeFile(unsynced, QByteArrayLiteral("# Kommentar\n\nFotos\n/Archiv/2020\r\nMusik/\n  Videos  \nA\\B\n/\n"));
        QString error;
        const auto folders = readUnsyncedFolders(unsynced, &error);
        QVERIFY(folders);
        const QSet<QString> expected{QStringLiteral("Fotos/"), QStringLiteral("Archiv/2020/"), QStringLiteral("Musik/"), QStringLiteral("Videos/"),
            QStringLiteral("A/B/")};
        QCOMPARE(*folders, expected);
    }

    void testPathAndRemoteFolderConflict()
    {
        const auto result = parse({QStringLiteral("--path"), QStringLiteral("/a"), QStringLiteral("-u"), QStringLiteral("anna"), local(),
            QStringLiteral("https://cloud.example.com"), QStringLiteral("b")});
        QCOMPARE(result.action, CmdOptionsResult::Action::Error);
    }

    void testLocalFolderMustExist()
    {
        const auto result = parse({QStringLiteral("-u"), QStringLiteral("anna"), _dir.filePath(QStringLiteral("fehlt")), QStringLiteral("https://cloud.example.com")});
        QCOMPARE(result.action, CmdOptionsResult::Action::Error);
        QVERIFY(result.error.contains(QLatin1String("does not exist")));
    }

    void testUsage()
    {
        const auto result = parse({QStringLiteral("-u"), QStringLiteral("anna")});
        QCOMPARE(result.action, CmdOptionsResult::Action::Error);
        QVERIFY(result.error.startsWith(QLatin1String("Usage")));
    }

    void testHelpAndVersion()
    {
        auto result = parse({QStringLiteral("--help")});
        QCOMPARE(result.action, CmdOptionsResult::Action::ShowHelp);
        for (const auto &option : {"--trust", "--httpproxy", "--exclude", "--unsyncedfolders", "--uplimit", "--downlimit", "--sync-hidden-files",
                 "--max-sync-retries", "--non-interactive", "--silent", "--logdebug", "--oauth-token-file"}) {
            QVERIFY2(result.helpText.contains(QLatin1String(option)), option);
        }
        result = parse({QStringLiteral("--version")});
        QCOMPARE(result.action, CmdOptionsResult::Action::ShowVersion);
    }

    // Kennwort aus der Umgebung statt aus der Befehlszeile (dort sähe es jeder in der Prozessliste)
    void testPasswordFromEnvironment()
    {
        QProcessEnvironment environment;
        environment.insert(QStringLiteral("OWNCLOUDCMD_PASSWORD"), QStringLiteral("aus-der-umgebung"));
        auto result = parse({QStringLiteral("-u"), QStringLiteral("anna"), local(), QStringLiteral("https://cloud.example.com")}, environment);
        QCOMPARE(result.action, CmdOptionsResult::Action::Run);
        QVERIFY(result.options.hasPassword);
        QCOMPARE(result.options.password, QStringLiteral("aus-der-umgebung"));

        // die Befehlszeile geht vor
        result = parse({QStringLiteral("-u"), QStringLiteral("anna"), QStringLiteral("-p"), QStringLiteral("befehlszeile"), local(),
                           QStringLiteral("https://cloud.example.com")},
            environment);
        QCOMPARE(result.options.password, QStringLiteral("befehlszeile"));
    }

    void testWithoutPassword()
    {
        const auto result = parse({QStringLiteral("-u"), QStringLiteral("anna"), local(), QStringLiteral("https://cloud.example.com")});
        QCOMPARE(result.action, CmdOptionsResult::Action::Run);
        QVERIFY(!result.options.hasPassword);
    }

    void testTokenFile()
    {
        QProcessEnvironment environment;
        environment.insert(QStringLiteral("OWNCLOUDCMD_OAUTH_TOKEN_FILE"), QStringLiteral("tokens.json"));
        environment.insert(QStringLiteral("OWNCLOUDCMD_PASSWORD"), QStringLiteral("geheim"));
        auto result = parse({local(), QStringLiteral("https://cloud.example.com")}, environment);
        QCOMPARE(result.action, CmdOptionsResult::Action::Run);
        QCOMPARE(result.options.tokenFile, QStringLiteral("tokens.json"));
        QVERIFY(!result.options.hasPassword);

        // -p schließt die Token-Datei der Umgebung aus
        result = parse({QStringLiteral("-u"), QStringLiteral("anna"), QStringLiteral("-p"), QStringLiteral("geheim"), local(),
                           QStringLiteral("https://cloud.example.com")},
            environment);
        QVERIFY(result.options.tokenFile.isEmpty());

        result = parse({QStringLiteral("--oauth-token-file"), QStringLiteral("t.json"), QStringLiteral("-p"), QStringLiteral("x"), local(),
            QStringLiteral("https://cloud.example.com")});
        QCOMPARE(result.action, CmdOptionsResult::Action::Error);
    }
};

QTEST_GUILESS_MAIN(TestCmdOptions)
#include "testcmdoptions.moc"
