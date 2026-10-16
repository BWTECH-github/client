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
#include "accountmanager.h"
#include "accountstate.h"
#include "application.h"
#include "common/restartmanager.h"
#include "common/utility.h"
#include "configfile.h"
#include "folder.h"
#include "folderman.h"
#include "guiutility.h"
#include "libsync/theme.h"
#include "mainwindow/colormanager.h"
#include "models/trayactivitymodels.h"
#include "networkinformation.h"
#include "openfilemanager.h"
#include "progressdispatcher.h"
#include "serveractivityfetcher.h"
#include "sharedialog.h"
#include "syncfileitem.h"

#include "iconresources.h"

#include <QApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QLocale>
#include <QMenu>
#include <QTimer>

#include <algorithm>
#include <chrono>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

using namespace std::chrono_literals;

namespace OCC {

namespace {
    constexpr int maxRecentLocalItems = 8;
    constexpr int maxRecentServerItems = 8;
    constexpr int maxNotificationsInMenu = 10;
    constexpr auto menuUpdateInterval = 1s;
    constexpr auto pauseCheckInterval = 30s;

    // heute nur die Uhrzeit, sonst Datum und Uhrzeit
    QString formatTime(const QDateTime &time)
    {
        const auto local = time.toLocalTime();
        if (local.date() == QDate::currentDate()) {
            return QLocale().toString(local.time(), QLocale::ShortFormat);
        }
        return QLocale().toString(local, QLocale::ShortFormat);
    }

    // Server ohne lesbare Zeitangabe (die Modelle setzen dann den Epochenbeginn): Zeit weglassen
    QString withTime(const QString &text, const QDateTime &time)
    {
        return time.toSecsSinceEpoch() > 0 ? QStringLiteral("%1 (%2)").arg(text, formatTime(time)) : text;
    }

    // „zuletzt geändert“ zeigt erfolgreich übertragene, umbenannte oder gelöschte Dateien, keine Ordner und Fehler
    bool shouldShowInRecent(const SyncFileItem &item)
    {
        if (item.isDirectory()) {
            return false;
        }
        switch (item._status) {
        case SyncFileItem::Success:
        case SyncFileItem::Conflict:
        case SyncFileItem::Restoration:
            break;
        default:
            return false;
        }
        switch (item.instruction()) {
        case CSYNC_INSTRUCTION_NEW:
        case CSYNC_INSTRUCTION_SYNC:
        case CSYNC_INSTRUCTION_CONFLICT:
        case CSYNC_INSTRUCTION_REMOVE:
        case CSYNC_INSTRUCTION_RENAME:
        case CSYNC_INSTRUCTION_TYPE_CHANGE:
            return true;
        default:
            return false;
        }
    }

    bool networkPausesSync()
    {
        auto *network = NetworkInformation::instance();
        return network->isBehindCaptivePortal() || (network->isMetered() && ConfigFile().pauseSyncWhenMetered());
    }

    QIcon folderIcon(Folder *folder)
    {
        SyncResult status = folder->syncResult();
        if (FolderMan::instance()->isUserSyncPaused() || folder->syncPaused() || networkPausesSync()) {
            status.setStatus(SyncResult::Paused);
        } else if (!folder->accountState() || !folder->accountState()->isConnected()) {
            status.setStatus(SyncResult::Offline);
        }
        return IconResources::getCoreIcon(QStringLiteral("states/%1").arg(status.iconNameForStatus()));
    }
}

// owncloud.online (Barrierefreiheit): lesbarer, übersetzter Status; der Tooltip des Tray-Symbols ist zugleich
// dessen zugänglicher Name, den Sprachausgaben (NVDA, Narrator, Orca) vorlesen
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
    , _recentActivity(new RecentActivityModel(maxRecentLocalItems, this))
    , _notifications(new ServerNotificationModel(this))
    , _serverActivity(new ServerActivityFetcher(_recentActivity, _notifications, this))
{
    connect(_tray, &QSystemTrayIcon::activated, this, &TrayMenuController::slotTrayClicked);

    // Änderungen sammeln: während einer Synchronisation kommen Fortschrittsmeldungen im Millisekundentakt
    _menuUpdateTimer.setSingleShot(true);
    _menuUpdateTimer.setInterval(menuUpdateInterval);
    connect(&_menuUpdateTimer, &QTimer::timeout, this, &TrayMenuController::updateMenu);

    _pauseCheckTimer.setInterval(pauseCheckInterval);
    connect(&_pauseCheckTimer, &QTimer::timeout, this, &TrayMenuController::checkPauseEnd);

    // using the main window as parent for memory management
    _menu = new QMenu(ocApp()->mainWindow());
    _menu->setTitle(Theme::instance()->appNameGUI());
    _menu->setToolTipsVisible(true);
    _tray->setContextMenu(_menu);
    connect(_menu, &QMenu::aboutToShow, this, [this] {
        checkPauseEnd();
        rebuildMenu();
        // Server-Aktivitäten fürs nächste Öffnen auffrischen (je Konto höchstens einmal pro Minute)
        _serverActivity->refreshActivities();
    });
    rebuildMenu();

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
    connect(folderMan, &FolderMan::folderListChanged, this, &TrayMenuController::scheduleMenuUpdate);

    connect(AccountManager::instance(), &AccountManager::accountAdded, this, &TrayMenuController::scheduleMenuUpdate);
    connect(AccountManager::instance(), &AccountManager::accountRemoved, this, &TrayMenuController::scheduleMenuUpdate);

    connect(ProgressDispatcher::instance(), &ProgressDispatcher::progressInfo, this, &TrayMenuController::onProgressInfo);
    connect(ProgressDispatcher::instance(), &ProgressDispatcher::itemCompleted, this, [this](Folder *folder, const SyncFileItemPtr &item) {
        if (!folder || !item || !folder->accountState() || !folder->accountState()->account() || !shouldShowInRecent(*item)) {
            return;
        }
        TrayActivity activity;
        activity.source = TrayActivity::Source::Local;
        activity.accountId = folder->accountState()->account()->uuid();
        activity.relativePath = item->destination();
        activity.subject = QFileInfo(activity.relativePath).fileName();
        activity.detail = Progress::asResultString(*item);
        activity.time = QDateTime::currentDateTime();
        activity.localPath = folder->path() + activity.relativePath;
        _recentActivity->addLocalActivity(activity);
    });

    for (QAbstractItemModel *model : {static_cast<QAbstractItemModel *>(_recentActivity), static_cast<QAbstractItemModel *>(_notifications)}) {
        connect(model, &QAbstractItemModel::modelReset, this, &TrayMenuController::scheduleMenuUpdate);
        connect(model, &QAbstractItemModel::rowsInserted, this, &TrayMenuController::scheduleMenuUpdate);
        connect(model, &QAbstractItemModel::rowsRemoved, this, &TrayMenuController::scheduleMenuUpdate);
    }

    connect(_serverActivity, &ServerActivityFetcher::newNotifications, this, [this](const QUuid &accountId, const QVector<ServerNotification> &fresh) {
        if (fresh.size() == 1) {
            const auto &notification = fresh.constFirst();
            slotShowOptionalTrayMessage(notification.subject, notification.message);
            return;
        }
        auto *accountState = AccountManager::instance()->accountState(accountId);
        const QString accountName = accountState && accountState->account() ? accountState->account()->displayNameWithHost() : QString();
        slotShowOptionalTrayMessage(tr("%n new notification(s)", "", static_cast<int>(fresh.size())),
            tr("For %1. You find them in the menu of the tray icon.").arg(accountName));
    });
    connect(_serverActivity, &ServerActivityFetcher::notificationRequestFailed, this,
        [this](const QString &message) { slotShowTrayMessage(Theme::instance()->appNameGUI(), message); });
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
    // die Pause aus dem Tray-Menü hält nur den Scheduler an; die Ordner selbst melden weiter ihren letzten Stand
    if (FolderMan::instance()->isUserSyncPaused()) {
        status.setStatus(SyncResult::Paused);
    }
    _tray->setIcon(getTrayStatusIcon(status));
    _tray->setToolTip(Theme::instance()->appNameGUI() + QStringLiteral(" - ") + overallStatusText());
    scheduleMenuUpdate();
}

void TrayMenuController::updateTrayIcon()
{
    slotComputeOverallSyncStatus();
}

QDateTime TrayMenuController::pauseEnd(PauseDuration duration, const QDateTime &now)
{
    switch (duration) {
    case PauseDuration::OneHour:
        return now.addSecs(60 * 60);
    case PauseDuration::TwoHours:
        return now.addSecs(2 * 60 * 60);
    case PauseDuration::FourHours:
        return now.addSecs(4 * 60 * 60);
    case PauseDuration::UntilTomorrow: {
        // setDate/setTime behalten die Zeitzone von now
        QDateTime end = now;
        end.setDate(now.date().addDays(1));
        end.setTime(QTime(0, 0));
        return end;
    }
    case PauseDuration::UntilResumed:
        break;
    }
    return {};
}

QString TrayMenuController::menuText(const QString &text, int maxLength)
{
    QString result = text.simplified();
    if (maxLength > 1 && result.size() > maxLength) {
        qsizetype head = (maxLength - 1) / 2;
        qsizetype tail = maxLength - 1 - head;
        // keine Ersatzzeichenpaare (etwa Emoji) zerschneiden
        if (head > 0 && result.at(head - 1).isHighSurrogate()) {
            --head;
        }
        if (tail > 0 && result.at(result.size() - tail).isLowSurrogate()) {
            --tail;
        }
        result = result.left(head) + QChar(0x2026) + result.right(tail);
    }
    result.replace(QLatin1Char('&'), QStringLiteral("&&"));
    return result;
}

void TrayMenuController::scheduleMenuUpdate()
{
    if (!_menuUpdateTimer.isActive()) {
        _menuUpdateTimer.start();
    }
}

void TrayMenuController::updateMenu()
{
    // der Tooltip zeigt auch den Fortschritt
    _tray->setToolTip(Theme::instance()->appNameGUI() + QStringLiteral(" - ") + overallStatusText());

    // Windows und macOS melden das Öffnen zuverlässig (aboutToShow baut dann neu auf). Linux-Trays
    // (StatusNotifierItem) tun das nicht immer: dort die Struktur gleich nachziehen, wenn sie sich geändert hat.
    if (Utility::isLinux() && !_menu->isVisible() && menuStructureKey() != _menuStructureKey) {
        rebuildMenu();
        return;
    }
    // ein offenes Menü nicht umbauen (geöffnete Untermenüs schlössen sich), nur die Texte auffrischen
    refreshMenuTexts();
}

QString TrayMenuController::menuStructureKey() const
{
    QStringList parts;
    for (auto *accountState : AccountManager::instance()->accounts()) {
        if (!accountState->account()) {
            continue;
        }
        const auto accountId = accountState->account()->uuid();
        parts << accountId.toString() << QString::number(accountState->isSignedOut());
        for (auto *folder : FolderMan::instance()->foldersForAccount(accountId)) {
            parts << QString::fromUtf8(folder->id()) << QString::number(folder->syncPaused());
        }
    }
    parts << QString::number(FolderMan::instance()->isUserSyncPaused()) << _syncPausedUntil.toString(Qt::ISODate);
    for (int row = 0; row < _recentActivity->rowCount(); ++row) {
        const auto &activity = _recentActivity->activity(row);
        parts << activity.localPath << activity.id << activity.time.toString(Qt::ISODateWithMs);
    }
    for (int row = 0; row < _notifications->rowCount(); ++row) {
        parts << _notifications->notification(row).id;
    }
    return parts.join(QLatin1Char('|'));
}

void TrayMenuController::refreshMenuTexts()
{
    if (_statusAction) {
        _statusAction->setText(menuText(overallStatusText(), 100));
    }
    for (const auto &entry : std::as_const(_accountMenus)) {
        if (entry.first && entry.second && entry.second->account()) {
            entry.first->setTitle(accountMenuTitle(entry.second));
        }
    }
    for (const auto &entry : std::as_const(_folderActions)) {
        if (entry.first && entry.second) {
            entry.first->setText(folderActionText(entry.second));
            entry.first->setIcon(folderIcon(entry.second));
        }
    }
}

QMenu *TrayMenuController::addSubMenu(QMenu *parent, const QString &title)
{
    auto *menu = new QMenu(title, parent);
    menu->setToolTipsVisible(true);
    _subMenus.append(menu);
    parent->addMenu(menu);
    return menu;
}

void TrayMenuController::rebuildMenu()
{
    _menuUpdateTimer.stop();
    // Untermenüs gehören nicht zu den Aktionen, die clear() löscht; deleteLater, weil ein Aufbau aus einer
    // Aktion heraus deren Menü noch braucht
    for (const auto &menu : std::as_const(_subMenus)) {
        if (menu) {
            menu->deleteLater();
        }
    }
    _subMenus.clear();
    _accountMenus.clear();
    _folderActions.clear();
    _menu->clear();
    _menuStructureKey = menuStructureKey();

    auto *app = ocApp();
    _menu->addAction(IconResources::getBrandingIcon(Theme::instance()->applicationIconName()), tr("Show %1").arg(Theme::instance()->appNameGUI()), app,
        &Application::ensureVisible);
    _menu->addSeparator();

    _statusAction = _menu->addAction(menuText(overallStatusText(), 100));
    _statusAction->setEnabled(false);

    const auto accounts = AccountManager::instance()->accounts();
    _menu->addSeparator();
    if (accounts.isEmpty()) {
        _menu->addAction(tr("Add account…"), this, &TrayMenuController::requestAddAccount);
    } else {
        for (auto *accountState : accounts) {
            if (accountState->account()) {
                addAccountMenu(accountState);
            }
        }
    }

    _menu->addSeparator();
    addRecentMenu();
    addNotificationsMenu();

    _menu->addSeparator();
    addPauseEntries();
    _menu->addSeparator();

    if (app->debugMode()) {
        addDebugMenu();
        _menu->addSeparator();
    }

    _menu->addAction(tr("Settings…"), this, &TrayMenuController::requestShowSettings);
    if (!Theme::instance()->helpUrl().isEmpty()) {
        _menu->addAction(tr("Help"), this, &TrayMenuController::requestShowHelp);
    }
    if (!Theme::instance()->about().isEmpty()) {
        _menu->addAction(tr("About %1").arg(Theme::instance()->appNameGUI()), this, &TrayMenuController::requestShowAbout);
    }
    _menu->addAction(tr("Quit %1").arg(Theme::instance()->appNameGUI()), this, &QApplication::quit);
}

void TrayMenuController::addAccountMenu(AccountState *accountState)
{
    auto *account = accountState->account();
    auto *menu = addSubMenu(_menu, accountMenuTitle(accountState));
    _accountMenus.append(qMakePair(QPointer<QMenu>(menu), QPointer<AccountState>(accountState)));

    auto *stateAction = menu->addAction(menuText(accountStatusText(accountState)));
    stateAction->setEnabled(false);
    const auto errors = accountState->connectionErrors();
    if (!errors.isEmpty()) {
        stateAction->setToolTip(errors.join(QLatin1Char('\n')));
    }

    auto folders = FolderMan::instance()->foldersForAccount(account->uuid());
    std::sort(folders.begin(), folders.end(), [](Folder *a, Folder *b) { return a->displayName().localeAwareCompare(b->displayName()) < 0; });
    bool onePaused = false;
    if (!folders.isEmpty()) {
        menu->addSeparator();
        for (auto *folder : std::as_const(folders)) {
            onePaused = onePaused || folder->syncPaused();
            auto *action = menu->addAction(folderIcon(folder), folderActionText(folder));
            action->setToolTip(folder->path());
            const QString path = folder->path();
            connect(action, &QAction::triggered, this, [path] { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); });
            _folderActions.append(qMakePair(QPointer<QAction>(action), QPointer<Folder>(folder)));
        }
    }

    menu->addSeparator();
    const QUrl url = account->url();
    menu->addAction(tr("Open in browser"), this, [url] { Utility::openBrowser(url, nullptr); });
    if (onePaused) {
        // einzeln pausierte Ordner (Hauptfenster); die zeitliche Pause steuert der Haupteintrag
        const QUuid accountId = account->uuid();
        menu->addAction(tr("Resume synchronization"), this, [this, accountId] { resumeAccountFolders(accountId); });
    }
    if (accountState->isSignedOut()) {
        menu->addAction(tr("Log in…"), accountState, &AccountState::signIn);
    } else {
        menu->addAction(tr("Log out"), accountState, &AccountState::signOutByUi);
    }
}

void TrayMenuController::addRecentMenu()
{
    auto *menu = addSubMenu(_menu, tr("Recently changed"));

    QList<int> serverRows;
    int localCount = 0;
    for (int row = 0; row < _recentActivity->rowCount(); ++row) {
        const auto &activity = _recentActivity->activity(row);
        if (activity.source == TrayActivity::Source::Server) {
            if (serverRows.size() < maxRecentServerItems) {
                serverRows.append(row);
            }
            continue;
        }
        if (localCount >= maxRecentLocalItems) {
            continue;
        }
        ++localCount;
        auto *action = menu->addAction(QStringLiteral("%1 (%2, %3)").arg(menuText(activity.subject, 50), menuText(activity.detail, 40), formatTime(activity.time)));
        action->setToolTip(activity.relativePath);
        // gelöschte oder inzwischen verschobene Dateien lassen sich nicht zeigen; Dateien werden nie geöffnet,
        // nur im Dateimanager markiert (kein versehentliches Ausführen)
        const QString path = activity.localPath;
        if (QFileInfo::exists(path)) {
            connect(action, &QAction::triggered, this, [path] { showInFileManager(path); });
        } else {
            action->setEnabled(false);
        }
    }
    if (localCount == 0) {
        menu->addAction(tr("No files synchronized recently"))->setEnabled(false);
    }

    if (!serverRows.isEmpty()) {
        menu->addSeparator();
        menu->addAction(tr("On the server"))->setEnabled(false);
        for (int row : std::as_const(serverRows)) {
            const auto &activity = _recentActivity->activity(row);
            auto *action = menu->addAction(withTime(menuText(activity.subject, 60), activity.time));
            if (!activity.detail.isEmpty()) {
                action->setToolTip(activity.detail);
            }
            const QUrl link = activity.link;
            if (ServerActivityFetcher::isWebLink(link)) {
                connect(action, &QAction::triggered, this, [link] { Utility::openBrowser(link, nullptr); });
            } else {
                action->setEnabled(false);
            }
        }
    }

    menu->addSeparator();
    menu->addAction(tr("Show all activity…"), this, &TrayMenuController::requestShowActivity);
}

void TrayMenuController::addNotificationsMenu()
{
    const int count = _notifications->rowCount();
    if (count == 0) {
        return;
    }
    auto *menu = addSubMenu(_menu, tr("Notifications (%1)").arg(count));
    for (int row = 0; row < std::min(count, maxNotificationsInMenu); ++row) {
        const auto &notification = _notifications->notification(row);
        auto *entry = addSubMenu(menu, menuText(notification.subject, 60));
        if (!notification.message.isEmpty()) {
            auto *message = entry->addAction(menuText(notification.message, 100));
            message->setEnabled(false);
            message->setToolTip(notification.message);
        }
        if (notification.time.toSecsSinceEpoch() > 0) {
            entry->addAction(formatTime(notification.time))->setEnabled(false);
        }
        entry->addSeparator();

        const QUuid accountId = notification.accountId;
        const QString id = notification.id;
        const QUrl link = notification.link;
        if (ServerActivityFetcher::isWebLink(link)) {
            entry->addAction(tr("Open in browser"), this, [link] { Utility::openBrowser(link, nullptr); });
        }
        for (const auto &action : notification.actions) {
            entry->addAction(menuText(action.label, 60), this, [this, accountId, id, action] { _serverActivity->runNotificationAction(accountId, id, action); });
        }
        entry->addAction(tr("Dismiss"), this, [this, accountId, id] { _serverActivity->dismissNotification(accountId, id); });
    }
    if (count > maxNotificationsInMenu) {
        menu->addSeparator();
        menu->addAction(tr("%n more on the server", "", count - maxNotificationsInMenu))->setEnabled(false);
    }
}

void TrayMenuController::addPauseEntries()
{
    if (FolderMan::instance()->isUserSyncPaused()) {
        const QString text = _syncPausedUntil.isValid() ? tr("Resume synchronization (paused until %1)").arg(formatTime(_syncPausedUntil))
                                                        : tr("Resume synchronization");
        _menu->addAction(text, this, &TrayMenuController::resumeSync);
        return;
    }
    auto *menu = addSubMenu(_menu, tr("Pause synchronization"));
    menu->addAction(tr("For 1 hour"), this, [this] { pauseSync(PauseDuration::OneHour); });
    menu->addAction(tr("For 2 hours"), this, [this] { pauseSync(PauseDuration::TwoHours); });
    menu->addAction(tr("For 4 hours"), this, [this] { pauseSync(PauseDuration::FourHours); });
    menu->addAction(tr("Until tomorrow"), this, [this] { pauseSync(PauseDuration::UntilTomorrow); });
    menu->addAction(tr("Until I resume"), this, [this] { pauseSync(PauseDuration::UntilResumed); });
}

void TrayMenuController::addDebugMenu()
{
    auto *debugMenu = addSubMenu(_menu, QStringLiteral("Debug actions"));
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
}

QString TrayMenuController::overallStatusText() const
{
    const auto accounts = AccountManager::instance()->accounts();
    if (accounts.isEmpty()) {
        return tr("No account configured");
    }
    if (FolderMan::instance()->isUserSyncPaused()) {
        return _syncPausedUntil.isValid() ? tr("Synchronization paused until %1").arg(formatTime(_syncPausedUntil)) : tr("Synchronization paused");
    }

    // es läuft immer höchstens eine Synchronisation; ihr Fortschritt hat Vorrang
    const auto folders = FolderMan::instance()->folders();
    for (auto *folder : folders) {
        if (folder->isSyncRunning()) {
            return folderStatusText(folder);
        }
    }

    qsizetype signedOut = 0;
    qsizetype disconnected = 0;
    for (auto *accountState : accounts) {
        if (accountState->isSignedOut()) {
            ++signedOut;
        } else if (!accountState->isConnected()) {
            ++disconnected;
        }
    }
    if (signedOut == accounts.size()) {
        return tr("Signed out");
    }
    if (signedOut + disconnected == accounts.size()) {
        return tr("Disconnected");
    }
    if (signedOut + disconnected > 0) {
        return tr("Disconnected from some accounts");
    }
    if (folders.isEmpty()) {
        return tr("No sync folders configured");
    }
    if (std::all_of(folders.cbegin(), folders.cend(), [](Folder *f) { return f->syncPaused(); })) {
        return tr("Sync paused");
    }

    TrayOverallStatusResult result;
    for (auto *folder : folders) {
        result.addResult(folder);
    }
    const auto &overall = result.overallStatus();
    if (overall.status() == SyncResult::Problem && overall.numNewConflictItems() > 0) {
        return tr("%n unresolved conflict(s)", "", overall.numNewConflictItems());
    }
    if (overall.status() == SyncResult::Success) {
        // nur eine plausible Zeit anzeigen: ein Ordner ohne abgeschlossenen Lauf liefert sonst ein Datum weit zurück
        const auto lastSync = result.lastSyncDone;
        if (lastSync.isValid() && lastSync.daysTo(QDateTime::currentDateTime()) < 7) {
            return tr("Up to date (%1)").arg(formatTime(lastSync));
        }
    }
    return syncResultText(overall);
}

QString TrayMenuController::accountStatusText(AccountState *accountState) const
{
    switch (accountState->state()) {
    case AccountState::SignedOut:
        return tr("Signed out");
    case AccountState::Disconnected:
        return tr("Disconnected");
    case AccountState::Connected:
        return tr("Connected");
    case AccountState::ServiceUnavailable:
        return tr("Server temporarily unavailable");
    case AccountState::MaintenanceMode:
        return tr("Server in maintenance mode");
    case AccountState::NetworkError:
        return tr("Network error");
    case AccountState::ConfigurationError:
        return tr("Server configuration error");
    case AccountState::AskingCredentials:
        return tr("Waiting for sign-in");
    case AccountState::Connecting:
        return tr("Connecting…");
    }
    return tr("Disconnected");
}

QString TrayMenuController::accountMenuTitle(AccountState *accountState) const
{
    QString status;
    if (!accountState->isConnected()) {
        status = accountStatusText(accountState);
    } else {
        const auto folders = FolderMan::instance()->foldersForAccount(accountState->account()->uuid());
        const auto running = std::find_if(folders.cbegin(), folders.cend(), [](Folder *f) { return f->isSyncRunning(); });
        if (running != folders.cend()) {
            status = folderStatusText(*running);
        } else if (FolderMan::instance()->isUserSyncPaused()) {
            status = tr("Sync paused");
        } else if (!folders.isEmpty()) {
            TrayOverallStatusResult result;
            for (auto *folder : folders) {
                result.addResult(folder);
            }
            status = syncResultText(result.overallStatus());
        } else {
            status = accountStatusText(accountState);
        }
    }
    return QStringLiteral("%1 (%2)").arg(menuText(accountState->account()->displayNameWithHost(), 50), status);
}

QString TrayMenuController::folderStatusText(Folder *folder) const
{
    if (FolderMan::instance()->isUserSyncPaused() || folder->syncPaused() || networkPausesSync()) {
        return tr("Sync paused");
    }
    if (!folder->accountState() || !folder->accountState()->isConnected()) {
        return tr("Offline");
    }
    if (!folder->isAvailable()) {
        return tr("Unavailable");
    }
    if (folder->isSyncRunning()) {
        const auto it = _folderProgress.constFind(folder->id());
        return it != _folderProgress.cend() ? progressText(it.value()) : tr("Syncing");
    }
    const auto result = folder->syncResult();
    if (result.status() == SyncResult::NotYetStarted) {
        return tr("Waiting to sync");
    }
    return syncResultText(result);
}

QString TrayMenuController::folderActionText(Folder *folder) const
{
    return tr("Open folder \"%1\" (%2)").arg(menuText(folder->displayName(), 40), folderStatusText(folder));
}

QString TrayMenuController::syncResultText(const SyncResult &result) const
{
    if (result.status() == SyncResult::Problem && result.hasUnresolvedConflicts()) {
        return tr("Unresolved conflicts");
    }
    return trayStatusToString(result.status());
}

QString TrayMenuController::progressText(const FolderProgress &progress) const
{
    if (!progress.propagating || progress.totalFiles <= 0) {
        return tr("Checking for changes");
    }
    const QLocale locale;
    if (progress.trustEta && progress.etaMsecs > 0) {
        return tr("Syncing %1 of %2 (%3 left)")
            .arg(locale.toString(progress.currentFile), locale.toString(progress.totalFiles),
                Utility::durationToDescriptiveString1(std::chrono::milliseconds(progress.etaMsecs)));
    }
    return tr("Syncing %1 of %2").arg(locale.toString(progress.currentFile), locale.toString(progress.totalFiles));
}

void TrayMenuController::onProgressInfo(Folder *folder, const ProgressInfo &progress)
{
    if (!folder) {
        return;
    }
    if (progress.status() == ProgressInfo::Done) {
        _folderProgress.remove(folder->id());
    } else {
        FolderProgress entry;
        entry.propagating = progress.status() == ProgressInfo::Propagation;
        if (entry.propagating) {
            entry.currentFile = progress.currentFile();
            entry.totalFiles = std::max(progress.totalFiles(), progress.currentFile());
            entry.trustEta = progress.trustEta();
            entry.etaMsecs = progress.totalProgress().estimatedEta;
        }
        _folderProgress.insert(folder->id(), entry);
    }
    scheduleMenuUpdate();
}

void TrayMenuController::pauseSync(PauseDuration duration)
{
    _syncPausedUntil = pauseEnd(duration, QDateTime::currentDateTime());
    if (_syncPausedUntil.isValid()) {
        _pauseCheckTimer.start();
    } else {
        _pauseCheckTimer.stop();
    }
    qCInfo(lcApplication) << "Sync paused by user until" << (_syncPausedUntil.isValid() ? _syncPausedUntil.toString(Qt::ISODate) : QStringLiteral("resumed"));
    FolderMan::instance()->setUserSyncPaused(true);
    slotComputeOverallSyncStatus();
}

void TrayMenuController::resumeSync()
{
    _pauseCheckTimer.stop();
    _syncPausedUntil = QDateTime();
    qCInfo(lcApplication) << "Sync resumed by user";
    FolderMan::instance()->setUserSyncPaused(false);
    slotComputeOverallSyncStatus();
}

void TrayMenuController::checkPauseEnd()
{
    if (FolderMan::instance()->isUserSyncPaused() && _syncPausedUntil.isValid() && QDateTime::currentDateTime() >= _syncPausedUntil) {
        qCInfo(lcApplication) << "Timed sync pause ended";
        resumeSync();
    }
}

void TrayMenuController::resumeAccountFolders(const QUuid &accountId)
{
    for (auto *folder : FolderMan::instance()->foldersForAccount(accountId)) {
        if (folder->syncPaused()) {
            folder->slotNextSyncFullLocalDiscovery(); // lokale Änderungen während der Pause nicht übersehen
            folder->setSyncPaused(false);
        }
    }
    slotComputeOverallSyncStatus();
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

void TrayMenuController::slotShowShareDialog(const QString &sharePath, const QString &localPath, ShareDialogStartPage startPage, const QString &errorMessage)
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
    _shareDialog = new ShareDialog(folder->accountState(), folder->webDavUrl(), sharePath, localPath, startPage, nullptr);
    if (!errorMessage.isEmpty()) {
        _shareDialog->showError(errorMessage);
    }
    // eigenständiges Fenster: erbt die Palette des Hauptfensters nicht, den Akzent deshalb selbst setzen
    ColorManager::applyAccent(_shareDialog);
    _shareDialog->show();
    _shareDialog->raise();
    _shareDialog->activateWindow();
}


} // end namespace
