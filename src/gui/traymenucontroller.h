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
#include "syncresult.h"

#include <QDateTime>
#include <QMenu>
#include <QObject>
#include <QPointer>
#include <QSystemTrayIcon>

class QTimer;

namespace OCC {

class Account;
class Folder;

class ShareDialog;

/**
 * @brief The ownCloudGui class
 * @ingroup gui
 */
class OWNCLOUDGUI_EXPORT TrayMenuController : public QObject
{
    Q_OBJECT
public:
    explicit TrayMenuController(QObject *parent);
    ~TrayMenuController() override;

    void useMonoIconsChanged(bool useMono);
    void updateTrayIcon();

Q_SIGNALS:
    void requestShowAbout();
    void requestShowHelp();

public Q_SLOTS:
    void setupTrayContextMenu();

    /** Globally pause syncing for the given number of minutes (0 = until manually
        resumed). Stops the scheduler without touching per-folder paused state. */
    void pauseSyncFor(int minutes);
    /** Resume globally paused syncing and trigger a sync right away. */
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
     */
    void slotShowShareDialog(const QString &sharePath, const QString &localPath);

private:
    QIcon getTrayStatusIcon(const SyncResult &status) const;

    /** Refresh the tray menu pause/resume entries to reflect the current pause state. */
    void updatePauseSyncMenuState();

    QSystemTrayIcon *_tray;
    QPointer<ShareDialog> _shareDialog;

    // Tray "Pause synchronization" feature
    QMenu *_pauseSyncMenu = nullptr;
    QPointer<QAction> _resumeSyncAction;
    QTimer *_pauseSyncTimer = nullptr;
    QDateTime _syncPausedUntil; //!< valid only for a timed pause; invalid while running or paused indefinitely
    bool _syncPaused = false;
};

} // namespace OCC
