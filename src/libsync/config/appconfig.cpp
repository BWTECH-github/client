// SPDX-License-Identifier: GPL-2.0-or-later
// SPDX-FileCopyrightText: 2026 Thomas Müller <thomas.mueller@tmit.eu>

#include "appconfig.h"

#include "common/asserts.h"
#include "common/utility.h"
#include "openidconfig.h"
#include "theme.h"

#include <QFile>
#include <QOperatingSystemVersion>
#include <QSettings>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace OCC {

Q_LOGGING_CATEGORY(lcSystemConfig, "sync.systemconfig", QtInfoMsg)

namespace chrono = std::chrono;

namespace {
#ifdef Q_OS_WIN
// QSettings öffnet einen Registry-Schlüssel schon zum Lesen mit Schreibrecht und legt ihn dabei an, wenn der Prozess
// das darf (Client mit Administratorrechten, etwa direkt aus dem Setup gestartet, oder ohne Benutzerkontensteuerung).
// Ohne Richtlinien bliebe so ein leerer Schlüssel unter HKLM\Software\Policies zurück, den keine Deinstallation
// entfernt: vorher nur lesend nachsehen.
bool localMachineKeyExists(const QString &path)
{
    const QString prefix = QStringLiteral("HKEY_LOCAL_MACHINE\\");
    if (!path.startsWith(prefix, Qt::CaseInsensitive)) {
        return true;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, reinterpret_cast<const wchar_t *>(path.mid(prefix.size()).utf16()), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}
#endif
}

AppConfig::AppConfig()
{
    _serverUrl = Theme::instance()->overrideServerUrl();
    // If a theme provides a hardcoded URL, do not allow for URL change.
    _allowServerURLChange = Theme::instance()->overrideServerUrl().isEmpty();
    _skipUpdateCheck = false;
    _openIdConfig = loadOpenIdConfigFromTheme();

    if (!Theme::instance()->allowSystemConfigOverrides())
        return;

    // Load all overrides

    const QString path = configPath(QOperatingSystemVersion::currentType(), *Theme::instance());
#ifdef Q_OS_WIN
    if (!localMachineKeyExists(path)) {
        return;
    }
#endif
    auto format = Utility::isWindows() ? QSettings::NativeFormat : QSettings::IniFormat;
    const QSettings system(path, format);

    _serverUrl = system.value(SetupServerUrlKey, QString()).toString();
    if (system.contains(SetupAllowServerUrlChangeKey)) {
        _allowServerURLChange = system.value(SetupAllowServerUrlChangeKey).toBool();
    }

    _skipUpdateCheck = system.value(UpdaterSkipUpdateCheckKey, false).toBool();

    OpenIdConfig systemConfig = loadOpenIdConfigFromSystemConfig(system);
    if (systemConfig.isValid()) {
        qCInfo(lcSystemConfig()) << "Using OpenID config from system config";
        _openIdConfig = systemConfig;
    }
}

OpenIdConfig AppConfig::loadOpenIdConfigFromTheme()
{
    Theme *theme = Theme::instance();

    QString clientId = theme->oauthClientId();
    QString clientSecret = theme->oauthClientSecret();
    QVector<quint16> ports = theme->oauthPorts();
    QString scopes = theme->openIdConnectScopes();
    QString prompt = theme->openIdConnectPrompt();

    OpenIdConfig cfg(clientId, clientSecret, ports, scopes, prompt);
    OC_ASSERT(cfg.isValid());

    return cfg;
}

OpenIdConfig AppConfig::loadOpenIdConfigFromSystemConfig(const QSettings &system)
{
    QString clientId = system.value(OidcClientIdKey, QString()).toString();
    QString clientSecret = system.value(OidcClientSecretKey, QString()).toString();
    QString scopes = system.value(OidcScopesKey, QString()).toString();
    QString prompt = system.value(OidcPromptKey, QString()).toString();

    QVector<quint16> ports;
    QVariant portsVar = system.value(OidcPortsKey);
    QStringList parts;
    if (portsVar.typeId() == QMetaType::QString) {
        // Windows registry: "8080,8888" is a QString
        parts = portsVar.toString().split(QLatin1Char(','), Qt::SkipEmptyParts);
    } else {
        // .ini files (Linux, macOS): "8080,8888" is a QStringlist
        parts = portsVar.toStringList();
    }
    for (const QString &p : std::as_const(parts)) {
        bool ok = false;
        quint16 val = p.trimmed().toUShort(&ok);
        if (ok) {
            ports.append(val);
        }
    }

    if (ports.isEmpty())
        ports.append(0); // 0 means any port

    return OpenIdConfig(clientId, clientSecret, ports, scopes, prompt);
}

QString AppConfig::configPath(const QOperatingSystemVersion::OSType& os, const Theme& theme)
{
    // Important: these paths conform to how names typically work on the systems on which they are used. This includes usage of upper-/lowercase.

    if (os == QOperatingSystemVersion::Windows) {
        // We use HKEY_LOCAL_MACHINE\Software\Policies since this is the location where GPO operates.
        // Note: use of uppercase/camelcase is common.
        return QString("HKEY_LOCAL_MACHINE\\Software\\Policies\\%1\\%2").arg(theme.vendor(), theme.appNameGUI());
    }

    if (os == QOperatingSystemVersion::MacOS) {
        // We use a subfolder to have one common location where in the future more files can be stored (like icons, images and such)
        // ini is used on macOS in contrary to plist because they are easier to maintain.
        // Note: rev-domain notation and lowercase is typically used.
        return QString("/Library/Preferences/%1/%2.ini").arg(theme.orgDomainName(), theme.appName());
    }

    // On Unix style systems, the application name in lowercase is typically used.
    return QString("/etc/%1/%1.ini").arg(theme.appName());
}

bool AppConfig::allowServerUrlChange() const
{
    return _allowServerURLChange;
}

QString AppConfig::serverUrl() const
{
    return _serverUrl;
}

bool AppConfig::skipUpdateCheck() const
{
    return _skipUpdateCheck;
}

OpenIdConfig AppConfig::openIdConfig() const
{
    return _openIdConfig;
}

} // OCC namespace
