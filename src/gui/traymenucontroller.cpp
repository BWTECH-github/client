/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#include "traymenucontroller.h"

#include "account.h"
#include "application.h"
#include "common/restartmanager.h"
#include "configfile.h"
#include "guiutility.h"
#include "libsync/theme.h"
#include "mainwindow/colormanager.h"
#include "sharedialog.h"

#include "iconresources.h"

#include <QApplication>
#include <QMenu>
#include <QTimer>

#include <chrono>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace std::chrono_literals;

namespace OCC {

// owncloud.online (Barrierefreiheit): lesbarer, übersetzter Gesamtstatus als Tooltip und damit als
// zugänglicher Name des Tray-Symbols, den Sprachausgaben (NVDA, Narrator, Orca) vorlesen
QString trayStatusToString(SyncResult::Status status)
{
    switch (status) {
    case SyncResult::Status::Offline:
        return TrayMenuController::tr("Offline");
    case SyncResult::Status::NotYetStarted:
    case SyncResult::Status::SyncPrepare:
    case SyncResult::Status::SyncRunning:
        return TrayMenuController::tr("Syncing");
    case SyncResult::Status::SyncAbortRequested:
        return TrayMenuController::tr("Aborting sync");
    case SyncResult::Status::Success:
        return TrayMenuController::tr("Up to date");
    case SyncResult::Status::Problem:
        return TrayMenuController::tr("Up to date, some files were ignored");
    case SyncResult::Status::Error:
    case SyncResult::Status::SetupError:
        return TrayMenuController::tr("Error");
    case SyncResult::Status::Paused:
        return TrayMenuController::tr("Sync paused");
    // trayOverallStatus() biegt für das Symbol Unavailable auf Undefined und ein echtes Undefined auf
    // Offline um; Undefined bedeutet hier also: nicht verfügbar
    case SyncResult::Status::Undefined:
    case SyncResult::Status::Unavailable:
        return TrayMenuController::tr("Unavailable");
    }
    return TrayMenuController::tr("Offline");
}

// simple helper to compute the overall status for the tray icon
SyncResult trayOverallStatus()
{
    TrayOverallStatusResult result;
    for (auto *folder : FolderMan::instance()->folders()) {
        result.addResult(folder);
    }
    SyncResult finalResult = result.overallStatus();

    // this is confusing. basically we have "standard" icons associated with different sync statuses
    // the only way to "pick" a particular icon for our tray use case is to update the status if the "real"
    // icon is not what we want to show in the tray.
    // Also! I would prefer to use the "warning" icon for unavailable status but the resources for the tray icons
    // do not provide a warning icon. this is only offered via the core icons so we can't use it in the tray
    if (finalResult.status() == SyncResult::Undefined) {
        // actual icon for offline is "error"
        finalResult.setStatus(SyncResult::Offline);
    } else if (finalResult.status() == SyncResult::Unavailable) {
        // actual icon for Unavailable is "error" - this is too much for the overall tray status so
        // change it to undefined, which shows the "info" icon :/
        finalResult.setStatus(SyncResult::Undefined);
    }
    return finalResult;
}

TrayMenuController::TrayMenuController(QObject *parent)
    : QObject(parent)
    , _tray(new QSystemTrayIcon(this))
{

    connect(_tray, &QSystemTrayIcon::activated, this, &TrayMenuController::slotTrayClicked);

    // setzt eine zeitlich begrenzte Pause der Synchronisation automatisch zurück
    _pauseSyncTimer = new QTimer(this);
    _pauseSyncTimer->setSingleShot(true);
    connect(_pauseSyncTimer, &QTimer::timeout, this, &TrayMenuController::resumeSync);

    setupTrayContextMenu();

    ConfigFile cfg;
    bool useMonoIcons = cfg.monoIcons();
    useMonoIconsChanged(useMonoIcons);

    slotComputeOverallSyncStatus();
    _tray->show();

    // Refactoring todo: we have a very nasty habit of connecting to globally available managers, etc inside constructors or similar.
    // this should only happen when we are dealing with a formal dependency: eg a dep passed by injection and kept as member,
    // or a dep that is instantiated inside the class (also with a member kept)
    // in cases like this one, the external deps should be instantiated externally and connected externally. This is normally part
    // of an app building routine. The global singletons have to go and this is an important step to achieving that.
    FolderMan *folderMan = FolderMan::instance();
    connect(folderMan, &FolderMan::folderSyncStateChange, this, &TrayMenuController::slotSyncStateChange);
    // adding this as when a folder is causing some "problem" - removing it should immediately update overall sync status instead of
    // waiting for next sync
    connect(folderMan, &FolderMan::folderRemoved, this, &TrayMenuController::slotComputeOverallSyncStatus);
}

TrayMenuController::~TrayMenuController()
{
}

void TrayMenuController::slotTrayClicked(QSystemTrayIcon::ActivationReason reason)
{
    // Left click
    if (reason == QSystemTrayIcon::Trigger) {
        // this covers left click - on mac it shows the menu so we don't want to do anything
        // more. on windows (and linux, presumably, given the original code) we just show the
        // main window on left click.
        if (!Utility::isMac())
            ocApp()->ensureVisible();
    }
}

void TrayMenuController::slotSyncStateChange(Folder *folder)
{
    slotComputeOverallSyncStatus();

    if (!folder) {
        return; // Valid, just a general GUI redraw was needed.
    }

    auto result = folder->syncResult();

    qCInfo(lcApplication) << "Sync state changed for folder " << folder->remoteUrl().toString() << ": " << Utility::enumToDisplayName(result.status());
}

void TrayMenuController::slotTrayMessageIfServerUnsupported(Account *account)
{
    if (account->serverSupportLevel() != Account::ServerSupportLevel::Supported) {
        slotShowTrayMessage(tr("Unsupported Server Version"),
            tr("The server on account %1 runs an unsupported version %2. "
               "Using this client with unsupported server versions is untested and "
               "potentially dangerous. Proceed at your own risk.")
                .arg(account->displayNameWithHost(), account->capabilities().status().versionString()));
    }
}

void TrayMenuController::useMonoIconsChanged(bool useMono)
{
    if (useMono != IconResources::useMonoTrayIcons()) {
        IconResources::setUseMonoTrayIcons(useMono);
        slotComputeOverallSyncStatus();
    }
}

QIcon TrayMenuController::getTrayStatusIcon(const SyncResult &status) const
{
    auto contextMenuVisible = _tray->contextMenu() && _tray->contextMenu()->isVisible();
    bool hasDarkTray = Utility::hasDarkSystray();
    return IconResources::themedTrayIcon(status.iconNameForStatus(), contextMenuVisible, hasDarkTray);
}

void TrayMenuController::slotComputeOverallSyncStatus()
{
    auto status = trayOverallStatus();
    const QIcon statusIcon = getTrayStatusIcon(status);
    _tray->setIcon(statusIcon);
    _tray->setToolTip(Theme::instance()->appNameGUI() + QStringLiteral(" - ") + trayStatusToString(status.status()));
}

void TrayMenuController::updateTrayIcon()
{
    slotComputeOverallSyncStatus();
}

void TrayMenuController::setupTrayContextMenu()
{
    // using the main window as parent for memory management
    // todo: review this as for some reason I remember there is some other way it's supposed to be done
    auto menu = new QMenu(ocApp()->mainWindow());
    menu->setTitle(Theme::instance()->appNameGUI());

    _tray->setContextMenu(menu);

    auto app = ocApp();
    // Populate the context menu now.
    menu->addAction(IconResources::getBrandingIcon(Theme::instance()->applicationIconName()), tr("Show %1").arg(Theme::instance()->appNameGUI()), app,
        &Application::ensureVisible);
    menu->addSeparator();

    // owncloud.online: Synchronisation vorübergehend anhalten; nach Ablauf setzt _pauseSyncTimer sie fort,
    // sonst der Eintrag zum Fortsetzen
    _pauseSyncMenu = menu->addMenu(tr("Pause synchronization"));
    _pauseSyncMenu->addAction(tr("For 30 minutes"), this, [this] { pauseSyncFor(30); });
    _pauseSyncMenu->addAction(tr("For 1 hour"), this, [this] { pauseSyncFor(60); });
    _pauseSyncMenu->addAction(tr("Until I resume"), this, [this] { pauseSyncFor(0); });
    _resumeSyncAction = menu->addAction(tr("Resume synchronization"), this, [this] { resumeSync(); });
    updatePauseSyncMenuState();
    menu->addSeparator();

    if (ocApp()->debugMode()) {
        auto *debugMenu = menu->addMenu(QStringLiteral("Debug actions"));
        debugMenu->addAction(QStringLiteral("Crash if asserts enabled - OC_ENSURE"), this, [] {
            if (OC_ENSURE(false)) {
                Q_UNREACHABLE();
            }
        });
        debugMenu->addAction(QStringLiteral("Crash if asserts enabled - Q_ASSERT"), this, [] { Q_ASSERT(false); });
        debugMenu->addAction(QStringLiteral("Crash now - Utility::crash()"), this, [] { Utility::crash(); });
        debugMenu->addAction(QStringLiteral("Crash now - OC_ENFORCE()"), this, [] { OC_ENFORCE(false); });
        debugMenu->addAction(QStringLiteral("Crash now - qFatal"), this, [] { qFatal("la Qt fatale"); });
        debugMenu->addAction(QStringLiteral("Restart now"), this, [] { RestartManager::requestRestart(); });
        debugMenu->addSeparator();
        auto captivePortalCheckbox = debugMenu->addAction(QStringLiteral("Behind Captive Portal"));
        captivePortalCheckbox->setCheckable(true);
        captivePortalCheckbox->setChecked(NetworkInformation::instance()->isForcedCaptivePortal());
        connect(captivePortalCheckbox, &QAction::triggered, [](bool checked) { NetworkInformation::instance()->setForcedCaptivePortal(checked); });

        menu->addSeparator();
    }

    if (!Theme::instance()->helpUrl().isEmpty()) {
        menu->addAction(tr("Help"), this, &TrayMenuController::requestShowHelp);
    }

    if (! Theme::instance()->about().isEmpty()) {
        menu->addAction(tr("About %1").arg(Theme::instance()->appNameGUI()), this, &TrayMenuController::requestShowAbout);
    }

    menu->addAction(tr("Quit %1").arg(Theme::instance()->appNameGUI()), this, &QApplication::quit);
}

void TrayMenuController::pauseSyncFor(int minutes)
{
    // Hält nur den Scheduler global an und lässt den Pausenzustand einzelner Ordner unberührt: einzeln
    // pausierte Ordner bleiben pausiert. Ein internes setSyncEnabled(true), etwa beim Hinzufügen eines
    // Ordners oder durch FolderMan::updateMoveToTrash(), beendet die Pause vorzeitig; das ist unschädlich.
    FolderMan::instance()->setSyncEnabled(false);

    _pauseSyncTimer->stop();
    _syncPaused = true;
    if (minutes > 0) {
        _syncPausedUntil = QDateTime::currentDateTime().addSecs(static_cast<qint64>(minutes) * 60);
        _pauseSyncTimer->start(std::chrono::minutes(minutes));
    } else {
        _syncPausedUntil = QDateTime(); // pausiert, bis der Nutzer fortsetzt
    }
    qCInfo(lcApplication) << "Sync paused by user for" << minutes << "minutes (0 = indefinite)";
    updatePauseSyncMenuState();
}

void TrayMenuController::resumeSync()
{
    _pauseSyncTimer->stop();
    _syncPaused = false;
    _syncPausedUntil = QDateTime();

    // gibt den Scheduler wieder frei; wartende Ordner synchronisieren weiter
    FolderMan::instance()->setSyncEnabled(true);
    qCInfo(lcApplication) << "Sync resumed by user";
    updatePauseSyncMenuState();
}

void TrayMenuController::updatePauseSyncMenuState()
{
    if (_pauseSyncMenu) {
        _pauseSyncMenu->menuAction()->setVisible(!_syncPaused);
    }
    if (_resumeSyncAction) {
        _resumeSyncAction->setVisible(_syncPaused);
        if (_syncPaused && _syncPausedUntil.isValid()) {
            _resumeSyncAction->setText(tr("Resume synchronization (paused until %1)").arg(_syncPausedUntil.toString(QStringLiteral("HH:mm"))));
        } else {
            _resumeSyncAction->setText(tr("Resume synchronization"));
        }
    }
}

void TrayMenuController::slotShowTrayMessage(const QString &title, const QString &msg, const QIcon &icon)
{
    _tray->showMessage(title, msg, icon.isNull() ? IconResources::getCoreIcon(QStringLiteral("states/information")) : icon);
}

void TrayMenuController::slotShowOptionalTrayMessage(const QString &title, const QString &msg, const QIcon &icon)
{
    ConfigFile cfg;
    if (cfg.optionalDesktopNotifications()) {
        slotShowTrayMessage(title, msg, icon);
    }
}

void TrayMenuController::slotShowShareInBrowser(const QString &sharePath, const QString &localPath)
{
    QString file;
    const auto folder = FolderMan::instance()->folderForPath(localPath, &file);
    if (!folder) {
        qCWarning(lcApplication) << "Could not open browser share page for" << localPath << "no responsible folder found";
        return;
    }

    if (folder->accountState()->account()->capabilities().filesSharing().sharing_roles) {
        fetchPrivateLinkUrl(folder->accountState()->account(), folder->webDavUrl(), sharePath, this, [](const QUrl &url) {
            const auto queryUrl = Utility::concatUrlPath(url, QString(), {{QStringLiteral("details"), QStringLiteral("sharing")}});
            Utility::openBrowser(queryUrl, nullptr);
        });
    }
}

void TrayMenuController::slotShowShareDialog(const QString &sharePath, const QString &localPath)
{
    QString file;
    const auto folder = FolderMan::instance()->folderForPath(localPath, &file);
    if (!folder || !folder->accountState() || !folder->accountState()->account()) {
        qCWarning(lcApplication) << "Could not open the share dialog for" << localPath << "- no responsible folder found";
        return;
    }
    const auto account = folder->accountState()->account();
    // Ohne OCS-Teilen-API kann der Dialog nicht arbeiten: dann die Teilen-Ansicht des Servers im Browser
    // öffnen (die Private-Link-Abfrage per PROPFIND klappt trotzdem). Upstreams slotShowShareInBrowser
    // prüft sharing_roles, das owncloud.online-/oC10-Server nie setzen.
    if (!account->capabilities().filesSharing().api_enabled) {
        if (account->capabilities().privateLinkPropertyAvailable()) {
            fetchPrivateLinkUrl(account, folder->webDavUrl(), sharePath, this, [](const QUrl &url) {
                const auto queryUrl = Utility::concatUrlPath(url, QString(), {{QStringLiteral("details"), QStringLiteral("sharing")}});
                Utility::openBrowser(queryUrl, nullptr);
            });
        } else {
            // ohne privatelink-Eigenschaft täte fetchPrivateLinkUrl still nichts: wenigstens die Weboberfläche öffnen
            Utility::openBrowser(account->url(), nullptr);
        }
        return;
    }
    if (_shareDialog) {
        _shareDialog->close();
    }
    _shareDialog = new ShareDialog(folder->accountState(), sharePath, localPath, nullptr);
    // eigenständiges Fenster: erbt die Palette des Hauptfensters nicht, den Akzent deshalb selbst setzen
    ColorManager::applyAccent(_shareDialog);
    _shareDialog->show();
    _shareDialog->raise();
    _shareDialog->activateWindow();
}


} // end namespace
