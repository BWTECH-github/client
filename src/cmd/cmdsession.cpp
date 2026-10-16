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

#include "cmdsession.h"

#include "account.h"
#include "common/vfs.h"
#include "creds/abstractcredentials.h"
#include "networkjobs/checkserverjobfactory.h"
#include "networkjobs/jsonjob.h"

#include "httpoauthcredentials.h"

#include <QJsonObject>
#include <QNetworkReply>
#include <QSslError>
#include <QTimer>
#include <QUuid>

#include <utility>

namespace OCC {

CmdSession::CmdSession(const QUrl &serverUrl, const QString &user, const QString &localDir, const QString &remotePath,
    const CredentialsFactory &credentialsFactory, QObject *parent)
    : CmdSession(serverUrl, user, localDir, remotePath, credentialsFactory, Settings{}, parent)
{
}

CmdSession::CmdSession(const QUrl &serverUrl, const QString &user, const QString &localDir, const QString &remotePath,
    const CredentialsFactory &credentialsFactory, const Settings &settings, QObject *parent)
    : QObject(parent)
    , _serverUrl(serverUrl)
    , _user(user)
    , _localDir(localDir)
    , _remotePath(remotePath)
    , _credentialsFactory(credentialsFactory)
    , _settings(settings)
{
}

CmdSession::~CmdSession()
{
    // Die Sync-Engine bricht beim Abbau noch über das Konto ab: vor den Konten (Kinder dieser Sitzung) abbauen
    delete _engine.data();
    delete _db.data();
}

void CmdSession::start()
{
    _account = createAccount(_user);
    // Basic meldet sich sofort zurück, OAuth2 erneuert ein abgelaufenes Access-Token vorher
    connect(_account->credentials(), &AbstractCredentials::fetched, this, &CmdSession::checkServer, Qt::SingleShotConnection);
    _account->credentials()->fetchFromKeychain();
}

Account *CmdSession::createAccount(const QString &davUser)
{
    auto *account = new Account(QUuid::createUuid(), davUser, _serverUrl, this);
    AbstractCredentials *credentials = _credentialsFactory(account);
    account->setCredentials(credentials);
    // wie 6.0.3: ungültige Zertifikate nur mit --trust, sonst den Grund nennen
    connect(account->accessManager(), &QNetworkAccessManager::sslErrors, this, [this](QNetworkReply *reply, const QList<QSslError> &errors) {
        if (_settings.trustSsl) {
            reply->ignoreSslErrors(errors);
            return;
        }
        for (const auto &error : errors) {
            qCritical().noquote() << "TLS error:" << error.errorString();
        }
        qCritical() << "If you trust the certificate of the server, use the --trust option.";
    });
    connect(credentials, &AbstractCredentials::authenticationFailed, this, [this, credentials] {
        const auto *oauth = qobject_cast<HttpOAuthCredentials *>(credentials);
        if (oauth) {
            qCritical().noquote() << "Authentication failed:" << oauth->errorString();
        } else {
            qCritical() << "Authentication failed";
        }
        finish(oauth && oauth->signInRequired() ? ExitSignInRequired : ExitFailure);
    });
    return account;
}

void CmdSession::checkServer()
{
    auto *checkJob = CheckServerJobFactory::createFromAccount(_account, false).startJob(_account->url(), this);
    connect(checkJob, &CoreJob::finished, this, [this, checkJob] {
        if (!checkJob->success()) {
            qCritical() << "Could not reach the server:" << checkJob->errorMessage();
            finish(ExitFailure);
            return;
        }
        fetchCapabilities();
    });
}

void CmdSession::fetchCapabilities()
{
    auto *capsJob = new JsonApiJob(_account, QStringLiteral("ocs/v2.php/cloud/capabilities"), {}, {}, this);
    connect(capsJob, &JsonApiJob::finishedSignal, this, [this, capsJob] {
        if (capsJob->reply()->error() != QNetworkReply::NoError) {
            qCritical() << "Failed to fetch capabilities:" << capsJob->reply()->errorString();
            finish(ExitFailure);
            return;
        }
        const QJsonObject caps = capsJob->data()
                                     .value(QStringLiteral("ocs"))
                                     .toObject()
                                     .value(QStringLiteral("data"))
                                     .toObject()
                                     .value(QStringLiteral("capabilities"))
                                     .toObject();
        fetchUserId(caps.toVariantMap());
    });
    capsJob->start();
}

void CmdSession::fetchUserId(const QVariantMap &capabilities)
{
    // The login string passed to -u may be an email alias or differ in casing
    // from the real user id. davUser drives the WebDAV path, so using the login
    // verbatim would target the wrong path (empty collection / 404, silent
    // no-op). Resolve the canonical id via ocs/cloud/user, like the GUI does.
    // With OAuth2 there may be no login at all: the token decides who we are.
    auto *userJob = new JsonApiJob(_account, QStringLiteral("ocs/v2.php/cloud/user"), {}, {}, this);
    connect(userJob, &JsonApiJob::finishedSignal, this, [this, userJob, capabilities] {
        QString davUser = _account->davUser();
        if (userJob->reply()->error() == QNetworkReply::NoError) {
            const QString id = userJob->data()
                                   .value(QStringLiteral("ocs"))
                                   .toObject()
                                   .value(QStringLiteral("data"))
                                   .toObject()
                                   .value(QStringLiteral("id"))
                                   .toString();
            if (!id.isEmpty()) {
                davUser = id;
            }
        }
        if (davUser.isEmpty()) {
            qCritical() << "Could not determine the user id:" << userJob->reply()->errorString();
            finish(ExitFailure);
            return;
        }

        Account *syncAccount = _account;
        if (davUser != _account->davUser()) {
            // davUser is only settable via the constructor; rebuild the account with the
            // canonical id. The credentials factory keeps the original sign-in (Basic login
            // or the same OAuth2 token file).
            qInfo() << "Using canonical WebDAV user id" << davUser << "(login was" << _account->davUser() << ")";
            syncAccount = createAccount(davUser);
        }
        syncAccount->setCapabilities({syncAccount->url(), capabilities});
        startSync(syncAccount);
    });
    userJob->start();
}

void CmdSession::startSync(Account *account)
{
    const QString dbPath = _localDir + SyncJournalDb::makeDbName(_localDir);
    auto *db = new SyncJournalDb(dbPath, this);
    _db = db;

    // owncloudcmd lädt immer alles herunter (Vfs::Off). SyncOptions hält die VFS seit DC-257 nur noch als
    // QPointer, sie gehört deshalb der Datenbank und lebt so lange wie Engine und Journal.
    Vfs *vfs = VfsPluginManager::instance().createVfsFromPlugin(Vfs::Off, db);
    if (!vfs) {
        qCritical() << "Could not load the vfs plugin for mode off";
        finish(ExitFailure);
        return;
    }
    SyncOptions opt(vfs);
    opt.fillFromEnvironmentVariables();
    opt.verifyChunkSizes();
    // owncloudcmd hat keine Netzwerk-Einstellungen: die Grenzen der Befehlszeile gelten, 0 = unbegrenzt
    opt._uploadLimit = _settings.uploadLimit;
    opt._downloadLimit = _settings.downloadLimit;

    // --unsyncedfolders: geänderte Einträge müssen vom Server neu gelesen werden (wie SelectiveSyncDialog)
    if (_settings.unsyncedFolders) {
        bool ok = false;
        const auto previous = db->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, ok);
        if (!ok) {
            qCritical() << "Could not read the selective sync list from" << dbPath;
            finish(ExitFailure);
            return;
        }
        const auto &current = *_settings.unsyncedFolders;
        for (const auto &path : (previous - current) + (current - previous)) {
            db->schedulePathForRemoteDiscovery(path);
        }
        db->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, current);
    }

    auto *engine = new SyncEngine(account, account->davUrl(), _localDir, _remotePath, db, db);
    _engine = engine;
    engine->setSyncOptions(opt);
    engine->setIgnoreHiddenFiles(_settings.ignoreHiddenFiles);
    // Löschschutz: alle bzw. sehr viele Dateien nur nach Rückfrage löschen
    engine->setPromptRemoveAllFiles(true);
    SyncEngine::minimumFileAgeForUpload = std::chrono::seconds(0);

    connect(engine, &SyncEngine::finished, this, &CmdSession::onSyncFinished);
    connect(engine, &SyncEngine::syncError, this, [](const QString &error, ErrorCategory) { qWarning() << "Sync error:" << error; });
    connect(engine, &SyncEngine::aboutToRemoveAllFiles, this, [this](SyncFileItem::Direction direction, bool allFiles, int removedFiles, int knownFiles) {
        const QString what = allFiles ? QStringLiteral("All files") : QStringLiteral("%1 of %2 files").arg(removedFiles).arg(knownFiles);
        _removeAllMessage = direction == SyncFileItem::Down
            ? QStringLiteral("%1 in the remote folder %2 were deleted on the server. The sync would delete them in the local folder %3 as well.")
                  .arg(what, _remotePath, _localDir)
            : QStringLiteral("%1 in the local folder %2 were deleted. The sync would delete them on the server as well (remote folder %3).")
                  .arg(what, _localDir, _remotePath);
    });

    // lädt die System- und die Benutzer-Ausschlussliste (ConfigFile::setupDefaultExcludeFilePaths)
    if (!_settings.excludeFile.isEmpty()) {
        engine->addExcludeList(_settings.excludeFile);
    }
    if (!engine->loadDefaultExcludes()) {
        qCritical() << "Cannot load the exclude lists";
        finish(ExitFailure);
        return;
    }
    engine->startSync();
}

void CmdSession::onSyncFinished(bool ok)
{
    if (!_removeAllMessage.isEmpty()) {
        // der Lauf hat nichts geändert; löschen nur nach ausdrücklicher Bestätigung
        const QString message = std::exchange(_removeAllMessage, QString());
        if (!_settings.confirmRemoveAll) {
            qCritical().noquote() << message;
            qCritical() << "Nothing was changed. Run owncloudcmd without --non-interactive to confirm the deletion.";
            finish(ExitRemoveAllNotConfirmed);
            return;
        }
        if (!_settings.confirmRemoveAll(message)) {
            qCritical() << "Nothing was deleted.";
            finish(ExitRemoveAllNotConfirmed);
            return;
        }
        qInfo() << "Deletion confirmed, syncing again";
        _engine->allowRemoveAllFilesOnce();
        QTimer::singleShot(0, _engine.data(), &SyncEngine::startSync);
        return;
    }
    if (!ok) {
        qWarning() << "Sync failed";
        finish(ExitFailure);
        return;
    }
    // wie 6.0.3: Änderungen während des Laufs mit weiteren Läufen nachziehen
    if (_engine->isAnotherSyncNeeded() && _restarts < _settings.maxSyncRetries) {
        ++_restarts;
        qInfo() << "Another sync is needed, starting sync" << _restarts + 1;
        QTimer::singleShot(0, _engine.data(), &SyncEngine::startSync);
        return;
    }
    if (_engine->isAnotherSyncNeeded()) {
        qWarning() << "Another sync is needed, but the retry limit" << _settings.maxSyncRetries << "is reached";
    }
    qInfo() << "Sync finished successfully";
    finish(ExitSuccess);
}

void CmdSession::finish(int exitCode)
{
    // nach einem Anmeldefehler kann die Engine noch mit "Sync failed" nachziehen: der erste Grund zählt
    if (_finished) {
        return;
    }
    _finished = true;
    Q_EMIT finished(exitCode);
}

} // namespace OCC
