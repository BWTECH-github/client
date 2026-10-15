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
#include <QUuid>

namespace OCC {

CmdSession::CmdSession(const QUrl &serverUrl, const QString &user, const QString &localDir, const QString &remotePath,
    const CredentialsFactory &credentialsFactory, QObject *parent)
    : QObject(parent)
    , _serverUrl(serverUrl)
    , _user(user)
    , _localDir(localDir)
    , _remotePath(remotePath)
    , _credentialsFactory(credentialsFactory)
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

    auto *engine = new SyncEngine(account, account->davUrl(), _localDir, _remotePath, db, db);
    _engine = engine;
    engine->setSyncOptions(opt);
    SyncEngine::minimumFileAgeForUpload = std::chrono::seconds(0);

    connect(engine, &SyncEngine::finished, this, [this](bool ok) {
        if (ok) {
            qInfo() << "Sync finished successfully";
            finish(ExitSuccess);
        } else {
            qWarning() << "Sync failed";
            finish(ExitFailure);
        }
    });
    connect(engine, &SyncEngine::syncError, this, [](const QString &error, ErrorCategory) { qWarning() << "Sync error:" << error; });

    // lädt die System- und die Benutzer-Ausschlussliste (ConfigFile::setupDefaultExcludeFilePaths)
    engine->loadDefaultExcludes();
    engine->startSync();
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
