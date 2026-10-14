/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Modified by BW-Tech GmbH for owncloud.online branding.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "theme.h"
#include "common/utility.h"
#include "common/version.h"
#include "common/vfs.h"
#include "config.h"

#include <QGuiApplication>
#include <QSslSocket>
#include <QStyle>

#ifdef THEME_INCLUDE
#include THEME_INCLUDE
#endif

namespace OCC {

Theme *Theme::_instance = nullptr;

Theme *Theme::instance()
{
    if (!_instance) {
        _instance = new THEME_CLASS;
    }
    return _instance;
}

Theme::~Theme() { }

QString Theme::appNameGUI() const
{
    return QStringLiteral(APPLICATION_NAME);
}

QString Theme::appName() const
{
    return QStringLiteral(APPLICATION_SHORTNAME);
}

QString Theme::orgDomainName() const
{
    return QStringLiteral(APPLICATION_REV_DOMAIN);
}

QString Theme::vendor() const
{
    return QStringLiteral(APPLICATION_VENDOR);
}

QString Theme::configFileName() const
{
    return QStringLiteral(APPLICATION_EXECUTABLE ".cfg");
}

QString Theme::applicationIconName() const
{
    // attention! if APPLICATION_ICON_NAME is not defined, it is auto-set to APPLICATION_SHORTNAME in CMakeLists.txt:131
    // I STRONGLY prefer to move that "fallback" here, to avoid confusion, as *how many cmake files do we have to navigate to
    // find these values*?
    // it's not ok, imo. I quasi understand the "need" for these vars but they are nothing but trouble.
    return QStringLiteral(APPLICATION_SHORTNAME) + "-icon";
}

QString Theme::aboutIconName() const
{
    return applicationIconName();
}

Theme::Theme()
    : QObject(nullptr)
{
}

QString Theme::helpUrl() const
{
    return QStringLiteral("https://owncloud.online/");
}

QString Theme::conflictHelpUrl() const
{
    auto baseUrl = helpUrl();
    if (baseUrl.isEmpty())
        return QString();
    if (!baseUrl.endsWith(QLatin1Char('/')))
        baseUrl.append(QLatin1Char('/'));
    return baseUrl + QStringLiteral("conflicts.html");
}

QString Theme::overrideServerUrl() const
{
    return QString();
}

QString Theme::overrideServerPath() const
{
    return {};
}

QUrl Theme::updateCheckUrl() const
{
#ifndef APPLICATION_UPDATE_URL
    return QUrl(QString());
#else
    return QUrl(QStringLiteral(APPLICATION_UPDATE_URL));
#endif
}

QString Theme::gitSHA1(VersionFormat format) const
{
    Q_UNUSED(format)
    return Version::gitSha().left(6);
}

QString Theme::aboutVersions(Theme::VersionFormat format) const
{
    const QString br = [&format] {
        switch (format) {
        case Theme::VersionFormat::RichText:
            return QStringLiteral("<br>");
        case Theme::VersionFormat::Url:
            Q_FALLTHROUGH();
        case Theme::VersionFormat::Plain:
            return QStringLiteral("\n");
        case Theme::VersionFormat::OneLiner:
            return QStringLiteral(" ");
        }
        Q_UNREACHABLE();
    }();
    const QString qtVersion = QString::fromUtf8(qVersion());
    const QString qtVersionString = (QLatin1String(QT_VERSION_STR) == qtVersion
            ? qtVersion
            : QCoreApplication::translate("ownCloudTheme::qtVer", "%1 (Built against Qt %2)").arg(qtVersion, QStringLiteral(QT_VERSION_STR)));
    QString _version = Version::displayString();
    QString gitUrl;
    // just use the version::gitSha if (!Version::gitSha().isEmpty())
    {
        if (format != Theme::VersionFormat::Url) {
            _version = QCoreApplication::translate("ownCloudTheme::versionWithSha", "%1 %2").arg(_version, gitSHA1(format));
        } else {
            gitUrl = gitSHA1(format) + br;
        }
    }
    QStringList sysInfo = {QStringLiteral("OS: %1-%2 (build arch: %3, CPU arch: %4)")
            .arg(QSysInfo::productType(), QSysInfo::kernelVersion(), QSysInfo::buildCpuArchitecture(), Utility::currentCpuArch())};
    // may be called by both GUI and CLI, but we can display QPA only for the former
    if (auto guiApp = qobject_cast<QGuiApplication *>(qApp)) {
        sysInfo << QStringLiteral("QPA: %1").arg(guiApp->platformName());
    }

    return QCoreApplication::translate("ownCloudTheme::aboutVersions()",
        "%1 %2%7"
        "%8"
        "Libraries Qt %3, %4%7"
        "Using virtual files plugin: %5%7"
        "%6")
        .arg(appName(), _version, qtVersionString, QSslSocket::sslLibraryVersionString(),
            Utility::enumToString(VfsPluginManager::instance().bestAvailableVfsMode()), sysInfo.join(br), br, gitUrl);
}


QString Theme::about() const
{
    const QString vendor = QStringLiteral(APPLICATION_VENDOR);
    return tr("<p>Version %1. For more information visit <a href=\"%2\">https://%3</a></p>"
              "<p><small>By Klaas Freitag, Daniel Molkentin, Olivier Goffart, Markus Götz, "
              " Jan-Christoph Borchardt, Thomas Müller,<br>"
              "Dominik Schmidt, Michael Stingl, Hannah von Reth, Fabian Müller and others.</small></p>"
              "<p>owncloud.online Desktop Client is distributed by %4 and licensed under the GNU General Public License (GPL) Version 2.0.</p>"
              "<p>Based on the ownCloud Desktop Client.</p>"
              "<p><small>%5</small></p>")
        .arg(Utility::escape(Version::displayString()), Utility::escape(QStringLiteral("https://" APPLICATION_DOMAIN)),
            Utility::escape(QStringLiteral(APPLICATION_DOMAIN)), Utility::escape(vendor),
            aboutVersions(Theme::VersionFormat::RichText));
}

QColor Theme::wizardHeaderTitleColor() const
{
    return QColor();
}

QColor Theme::wizardHeaderBackgroundColor() const
{
    return QColor();
}

QColor Theme::accentColor(bool) const
{
    return QColor();
}

QColor Theme::accentTextColor(bool) const
{
    return QColor();
}

QColor Theme::errorColor(bool) const
{
    return QColor();
}

QColor Theme::selectionColor() const
{
    return QColor();
}

QString Theme::wizardHeaderLogoName() const
{
    return applicationIconName();
}

QString Theme::wizardFooterLogoName() const
{
    return {};
}

bool Theme::forceSystemNetworkProxy() const
{
    return false;
}

QString Theme::oauthClientId() const
{
    return QStringLiteral("xdXOt13JKxym1B1QcEncf2XDkLAexMBFwiT9j6EfhhHFJhs2KM9jbjTmf8JBXE69");
}

QString Theme::oauthClientSecret() const
{
    return QStringLiteral("UBntmLjC2yYCeHwsyj73Uwo9TAaecAetRwMw0xYcvNL9yRdLSUi0hUAHfvCHFeFh");
}

QVector<quint16> Theme::oauthPorts() const
{
    // zero means a random port
    return {0};
}

QString Theme::openIdConnectScopes() const
{
    return QStringLiteral("openid offline_access email profile");
}

QString Theme::openIdConnectPrompt() const
{
    return QStringLiteral("select_account consent");
}

QString Theme::wizardUrlPlaceholder() const
{
    return QString();
}

bool Theme::forceVirtualFilesOption() const
{
    // owncloud.online: VFS is still the *recommended default* on platforms that
    // support it (Windows Cloud Files API, and soon the macOS File Provider), but it
    // is no longer FORCED. Returning false here means:
    //   * the new-account wizard's "Advanced settings" page shows all three sync
    //     modes (Use virtual files / Download and sync all / Choose folders to sync),
    //     with VFS pre-selected as the default when available,
    //   * the folder context menu offers "Activate/Deactivate virtual files", so VFS
    //     can be turned off again, and
    //   * "Manage subfolder sync" (selective sync) is available once VFS is disabled.
    // If this returned true, the wizard would hide the other two modes and the
    // enable/disable-VFS action would never be created (see
    // AdvancedSettingsPageController::buildPage and
    // AccountFoldersController::buildFolderActions).
    return false;
}

QVector<std::tuple<QString, QString, QUrl>> Theme::urlActions() const
{
    return {};
}

bool Theme::moveToTrashDefaultValue() const
{
    return false;
}

bool Theme::allowSystemConfigOverrides() const
{
    return false;
}

bool Theme::syncNewlyDiscoveredSpaces() const
{
    return false;
}

bool Theme::spacesAreCalledFolders() const
{
    // owncloud.online (11.x) has no "Spaces" concept; the UI talks about folders.
    // "Space" wording is misleading here, so present everything as "folder".
    return true;
}

bool Theme::withCrashReporter() const
{
#ifdef WITH_CRASHREPORTER
    return true;
#else
    return false;
#endif
}

} // end namespace client
