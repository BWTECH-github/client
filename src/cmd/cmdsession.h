/*
 * Copyright (C) 2026 by BW-Tech GmbH (owncloud.online)
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

#include "common/syncjournaldb.h"
#include "syncengine.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <functional>

namespace OCC {

class AbstractCredentials;
class Account;

/**
 * Ein Lauf von owncloudcmd: Anmeldedaten bereitstellen (abgelaufene OAuth2-Tokens vorab
 * erneuern), Server prüfen, Capabilities und die kanonische Benutzerkennung holen, dann
 * den lokalen Ordner einmal synchronisieren. finished() trägt den Exit-Code des Prozesses.
 */
class CmdSession : public QObject
{
    Q_OBJECT
public:
    enum ExitCode : int {
        ExitSuccess = 0,
        ExitFailure = 1,
        /** Der Server nimmt die OAuth2-Anmeldung nicht mehr an: neu anmelden. */
        ExitSignInRequired = 2,
    };

    /** Erzeugt die Anmeldedaten eines Kontos; erneut aufgerufen, wenn das Konto für die kanonische Kennung neu entsteht. */
    using CredentialsFactory = std::function<AbstractCredentials *(Account *account)>;

    CmdSession(const QUrl &serverUrl, const QString &user, const QString &localDir, const QString &remotePath, const CredentialsFactory &credentialsFactory,
        QObject *parent = nullptr);
    ~CmdSession() override;

    void start();

Q_SIGNALS:
    void finished(int exitCode);

private:
    Account *createAccount(const QString &davUser);
    void checkServer();
    void fetchCapabilities();
    void fetchUserId(const QVariantMap &capabilities);
    void startSync(Account *account);
    void finish(int exitCode);

    QUrl _serverUrl;
    QString _user;
    QString _localDir;
    QString _remotePath;
    CredentialsFactory _credentialsFactory;
    Account *_account = nullptr;
    QPointer<SyncJournalDb> _db;
    QPointer<SyncEngine> _engine;
    bool _finished = false;
};

} // namespace OCC
