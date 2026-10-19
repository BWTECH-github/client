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

#pragma once

#include "gui/owncloudguilib.h"
#include "gui/shareapi.h"
#include "syncresult.h"

#include <QDateTime>
#include <QHash>
#include <QMenu>
#include <QObject>
#include <QPointer>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVector>

namespace OCC {

class Account;
class AccountState;
class Folder;
class ProgressInfo;
class RecentActivityModel;
class ServerActivityFetcher;
class ServerNotificationModel;

class ShareDialog;

/**
 * @brief Tray-Symbol und Tray-Menü
 * @ingroup gui
 *
 * owncloud.online: Menü wie im Client 6.0.3, erweitert – Gesamtstatus, je Konto ein Untermenü mit Status,
 * Ordnern und Browser-Verweis, zuletzt geänderte Dateien (lokal und vom Server), Server-Benachrichtigungen,
 * zeitlich begrenzte Pause. Das Menü wird beim Öffnen neu aufgebaut; solange es offen ist, werden nur die
 * Texte aufgefrischt.
 */
class OWNCLOUDGUI_EXPORT TrayMenuController : public QObject
{
    Q_OBJECT
public:
    enum class PauseDuration { OneHour, TwoHours, FourHours, UntilTomorrow, UntilResumed };
    Q_ENUM(PauseDuration)

    explicit TrayMenuController(QObject *parent);
    ~TrayMenuController() override;

    void useMonoIconsChanged(bool useMono);
    void updateTrayIcon();

    /** Ende einer Pause ab now; ungültig für „bis zum Fortsetzen“, „bis morgen“ endet um Mitternacht */
    static QDateTime pauseEnd(PauseDuration duration, const QDateTime &now);

    /**
     * Einzeiliger Menütext: Leerraum zusammengefasst, zu lange Texte in der Mitte gekürzt und & maskiert
     * (sonst eine Zugriffstaste).
     */
    static QString menuText(const QString &text, int maxLength = 80);

Q_SIGNALS:
    void requestShowAbout();
    void requestShowHelp();
    void requestShowSettings();
    void requestShowActivity();
    void requestAddAccount();

public Q_SLOTS:
    /** Synchronisation global anhalten (bricht laufende ab), bei begrenzter Dauer automatisch fortsetzen */
    void pauseSync(OCC::TrayMenuController::PauseDuration duration);
    /** Pause aufheben und alle Ordner neu einplanen */
    void resumeSync();

    void slotComputeOverallSyncStatus();
    void slotShowTrayMessage(const QString &title, const QString &msg, const QIcon &icon = {});
    void slotShowOptionalTrayMessage(const QString &title, const QString &msg, const QIcon &icon = {});
    void slotSyncStateChange(Folder *);
    void slotTrayClicked(QSystemTrayIcon::ActivationReason reason);
    void slotTrayMessageIfServerUnsupported(Account *account);

    /**
     * Open a share dialog for a file or folder.
     *
     * sharePath is the full remote path to the item,
     * localPath is the absolute local path to it (so not relative
     * to the folder).
     */
    void slotShowShareInBrowser(const QString &sharePath, const QString &localPath);

    /**
     * Teilen aus dem Explorer-Kontextmenü: nativer Dialog für öffentliche Links, wenn der Server die
     * OCS-Teilen-API anbietet, sonst die Teilen-Ansicht des Servers im Browser (owncloud.online/oC10;
     * slotShowShareInBrowser wirkt nur mit oCIS-sharing_roles).
     * errorMessage erscheint im Dialog, z.B. wenn ein Link nicht ohne Rückfrage angelegt werden konnte.
     */
    void slotShowShareDialog(const QString &sharePath, const QString &localPath,
        OCC::ShareDialogStartPage startPage = OCC::ShareDialogStartPage::UsersAndGroups, const QString &errorMessage = QString());

private:
    /** letzter Fortschrittsstand einer laufenden Synchronisation */
    struct FolderProgress
    {
        bool propagating = false;
        qint64 currentFile = 0;
        qint64 totalFiles = 0;
        bool trustEta = false;
        quint64 etaMsecs = 0;
    };

    QIcon getTrayStatusIcon(const SyncResult &status) const;

    void scheduleMenuUpdate();
    void updateMenu();
    void rebuildMenu();
    void refreshMenuTexts();
    QString menuStructureKey() const;

    void addAccountMenu(AccountState *accountState);
    void addRecentMenu();
    void addNotificationsMenu();
    void addPauseEntries();
    void addDebugMenu();
    QMenu *addSubMenu(QMenu *parent, const QString &title);

    QString overallStatusText() const;
    QString accountStatusText(AccountState *accountState) const;
    QString accountMenuTitle(AccountState *accountState) const;
    QString folderStatusText(Folder *folder) const;
    QString folderActionText(Folder *folder) const;
    QString syncResultText(const SyncResult &result) const;
    QString progressText(const FolderProgress &progress) const;

    void onProgressInfo(Folder *folder, const ProgressInfo &progress);
    void checkPauseEnd();
    void setAccountSyncPaused(const QUuid &accountId, bool paused);

    QSystemTrayIcon *_tray;
    QMenu *_menu = nullptr;
    QPointer<ShareDialog> _shareDialog;

    // vom letzten Aufbau: Untermenüs (zum Aufräumen) und Einträge, deren Text sich ändert
    QList<QPointer<QMenu>> _subMenus;
    QPointer<QAction> _statusAction;
    QVector<QPair<QPointer<QMenu>, QPointer<AccountState>>> _accountMenus;
    QVector<QPair<QPointer<QAction>, QPointer<Folder>>> _folderActions;
    QString _menuStructureKey;
    QTimer _menuUpdateTimer;

    QHash<QByteArray, FolderProgress> _folderProgress; //!< nach Folder::id()
    RecentActivityModel *_recentActivity;
    ServerNotificationModel *_notifications;
    ServerActivityFetcher *_serverActivity;

    // Pause aus dem Tray-Menü: geprüft wird gegen die Uhrzeit, weil ein langer QTimer den Ruhezustand nicht mitzählt
    QTimer _pauseCheckTimer;
    QDateTime _syncPausedUntil; //!< nur bei zeitlich begrenzter Pause gültig
};

} // namespace OCC
