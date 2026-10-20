/*
 * Copyright (C) Lisa Reese <lisa.reese@kiteworks.com>
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

#include <QObject>
#include <QPointer>

#include <QHash>
#include <QUuid>

class QToolBar;
class QAction;
class QActionGroup;

namespace OCC {

class AccountState;
class AccountManager;
class Account;
class MainWindow;
class AccountViewController;
class ColorManager;
class LegacyClientImportController;

class AccountsGuiController : public QObject
{
    Q_OBJECT

public:
    AccountsGuiController(AccountManager *accountMgr, MainWindow *window, ColorManager *colorManager, QObject *parent);
    void runAccountWizard();

    /** owncloud.online: Konten und Sync-Ordner des offiziellen Desktop-Clients übernehmen (Menü) */
    void runLegacyImport();


private:
    QPointer<AccountManager> _accountMgr;
    QPointer<MainWindow> _window;
    QHash<QUuid, QAction *> _actionForAccount;
    QHash<QUuid, AccountViewController *> _viewControllerForAccount;
    ColorManager *_colorManager;

    void onAccountAdded(AccountState *state);
    void onAccountRemoved(AccountState *state);
    void onLastAccountRemoved();

    void onAccountAvatarChanged();

    void runFolderWizard(QUuid accountId);
    void handleAccountSetupError(const QString &error);

    void startModal(QUuid accountId);
    void endModal(QUuid accountId);

    void setupAccountPlaceholder();
    void removeAccountPlaceholder();

    /**
     * Läuft schon ein Kontoassistent, der Umstieg vom offiziellen Client oder ein anderer modaler Dialog, holt das diesen nach
     * vorn und liefert true. Zwei Assistenten zugleich schlügen denselben Sync-Ordner vor, der zweite bräche beim
     * „Fertigstellen“ ab (Q_ASSERT in Utility::markDirectoryAsSyncRoot).
     */
    bool raiseRunningDialog();

    /** erster Start ohne Konto: Umstieg vom offiziellen Client anbieten, sonst der Assistent */
    void runFirstStart();
    /** false, wenn nichts übernommen wird */
    bool startLegacyImport(bool firstStart);
    // laufender Umstieg; höchstens einer zugleich
    QPointer<LegacyClientImportController> _legacyImport;
};
}
