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
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <functional>
#include <optional>

namespace OCC {

class AbstractCredentials;
class Account;

/**
 * Ein Lauf von owncloudcmd: Anmeldedaten bereitstellen (abgelaufene OAuth2-Tokens vorab
 * erneuern), Server prüfen, Capabilities und die kanonische Benutzerkennung holen, dann
 * den lokalen Ordner synchronisieren. finished() trägt den Exit-Code des Prozesses.
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
        /** Der Lauf hätte alle bzw. sehr viele Dateien gelöscht und wurde nicht bestätigt: nichts geändert. */
        ExitRemoveAllNotConfirmed = 3,
    };

    /** Erzeugt die Anmeldedaten eines Kontos; erneut aufgerufen, wenn das Konto für die kanonische Kennung neu entsteht. */
    using CredentialsFactory = std::function<AbstractCredentials *(Account *account)>;

    /** Rückfrage vor dem Löschen aller bzw. sehr vieler Dateien; true = löschen. */
    using ConfirmRemoveAll = std::function<bool(const QString &message)>;

    /** Einstellungen des Laufs (Optionen wie im 6.0.3-Client) */
    struct Settings
    {
        bool ignoreHiddenFiles = true;
        /** zusätzliche Ausschlussliste (--exclude) */
        QString excludeFile;
        /** nicht zu synchronisierende Serverordner (--unsyncedfolders); ohne Wert bleibt die Liste im Journal */
        std::optional<QSet<QString>> unsyncedFolders;
        /** so oft höchstens erneut, wenn der Lauf einen weiteren verlangt */
        int maxSyncRetries = 3;
        /** Byte/s, 0 = unbegrenzt */
        qint64 uploadLimit = 0;
        qint64 downloadLimit = 0;
        /** TLS-Fehler des Servers übergehen (--trust) */
        bool trustSsl = false;
        /** ohne Funktion (--non-interactive) endet der Lauf ohne Löschen mit ExitRemoveAllNotConfirmed */
        ConfirmRemoveAll confirmRemoveAll;
    };

    CmdSession(const QUrl &serverUrl, const QString &user, const QString &localDir, const QString &remotePath, const CredentialsFactory &credentialsFactory,
        QObject *parent = nullptr);
    CmdSession(const QUrl &serverUrl, const QString &user, const QString &localDir, const QString &remotePath, const CredentialsFactory &credentialsFactory,
        const Settings &settings, QObject *parent = nullptr);
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
    void onSyncFinished(bool ok);
    void finish(int exitCode);

    QUrl _serverUrl;
    QString _user;
    QString _localDir;
    QString _remotePath;
    CredentialsFactory _credentialsFactory;
    Settings _settings;
    Account *_account = nullptr;
    QPointer<SyncJournalDb> _db;
    QPointer<SyncEngine> _engine;
    bool _finished = false;
    int _restarts = 0;
    // Meldung des Löschschutzes im laufenden Lauf
    QString _removeAllMessage;
};

} // namespace OCC
