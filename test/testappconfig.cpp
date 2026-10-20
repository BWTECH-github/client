// SPDX-License-Identifier: GPL-2.0-or-later

#include <QtTest>

#include "common/utility.h"
#include "libsync/config/appconfig.h"
#include "libsync/owncloudtheme.h"

#ifdef Q_OS_WIN
#include <qt_windows.h>

namespace {
// ohne QSettings: das legte den Schlüssel beim Öffnen selbst an
bool policiesKeyExists(const QString &path)
{
    const QString prefix = QStringLiteral("HKEY_LOCAL_MACHINE\\");
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, reinterpret_cast<const wchar_t *>(path.mid(prefix.size()).utf16()), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}
}
#endif

static const std::string_view iniData{
    "[Setup]\n"
    "ServerUrl=https://cloud.example.com\n"
    "AllowServerUrlChange=false\n"
    "MoveToTrash=true\n"
    "\n"
    "[Updater]\n"
    "SkipUpdateCheck=true\n"
    "\n"
    "[OpenIDConnect]\n"
    "ClientId=your-client-id\n"
    "ClientSecret=your-client-secret\n"
    "Ports=8080,8443\n"
    "Scopes=openid offline_access email profile\n"
    "Prompt=select_account consent\n"
};

class TestAppConfig : public QObject
{
    Q_OBJECT

    QString _configPath;

    bool createConfigFile(const QByteArray &data) {
        auto theme = OCC::ownCloudTheme();
        _configPath = OCC::AppConfig::configPath(QOperatingSystemVersion::Unknown, theme);

        QString parent = QFileInfo(_configPath).path();
        if (!QDir().mkpath(parent))
            return false;

        QFile iniFile(_configPath);
        if (!iniFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            qDebug()<<iniFile.errorString();
            return false;
        }
        bool ok = iniFile.write(data) == data.length();
        iniFile.close();
        return ok;
    }

    void cleanupConfigFile() {
        QFile::remove(_configPath);
    }

private Q_SLOTS:
    void testConfigPath()
    {
        auto t = OCC::ownCloudTheme();
        QCOMPARE(OCC::AppConfig::configPath(QOperatingSystemVersion::Windows, t), QString("HKEY_LOCAL_MACHINE\\Software\\Policies\\BW-Tech GmbH\\owncloud.online"));
        QCOMPARE(OCC::AppConfig::configPath(QOperatingSystemVersion::MacOS, t), QString("/Library/Preferences/online.owncloud.desktopclient/owncloud.online.ini"));
        QCOMPARE(OCC::AppConfig::configPath(QOperatingSystemVersion::Unknown, t), QString("/etc/owncloud.online/owncloud.online.ini"));
    }

    // Windows: Fehlen Richtlinien, legt das Lesen keinen leeren Schlüssel unter HKLM\Software\Policies an. Mit
    // Administratorrechten (oder ohne Benutzerkontensteuerung) blieb er sonst nach jedem Start zurück, und keine
    // Deinstallation entfernt ihn. Ohne Schreibrecht auf HKLM besagt der Test nichts; die CI läuft mit.
    void testMissingPoliciesAreNotCreated()
    {
#ifdef Q_OS_WIN
        // Arrange
        const QString path = OCC::AppConfig::configPath(QOperatingSystemVersion::Windows, *OCC::Theme::instance());
        if (policiesKeyExists(path)) {
            QSKIP("Richtlinien sind auf diesem Rechner eingerichtet");
        }

        // Act
        const OCC::AppConfig appConfig;
        Q_UNUSED(appConfig)

        // Assert
        QVERIFY2(!policiesKeyExists(path), qPrintable(path));
#else
        QSKIP("This test only works on Windows");
#endif
    }

    void testFromFile()
    {
        if (!OCC::Utility::isLinux()) {
            QSKIP("This test only works on Linux");
        }

        auto cleanup = qScopeGuard([this]() { TestAppConfig::cleanupConfigFile(); });
        QVERIFY(createConfigFile(QByteArray(iniData)));

        OCC::AppConfig appConfig;
        QVERIFY(!appConfig.allowServerUrlChange());

        OCC::OpenIdConfig oidCfg = appConfig.openIdConfig();
        QCOMPARE(oidCfg.clientId(), "your-client-id");
        QCOMPARE(oidCfg.clientSecret(), "your-client-secret");
        QCOMPARE(oidCfg.ports(), (QList<quint16>{8080, 8443}));
        QCOMPARE(oidCfg.scopes(), "openid offline_access email profile");
        QCOMPARE(oidCfg.prompt(), "select_account consent");
    }
};

QTEST_GUILESS_MAIN(TestAppConfig)
#include "testappconfig.moc"
