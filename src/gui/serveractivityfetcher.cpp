/*
 * Copyright (C) by BW-Tech GmbH for owncloud.online.
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

#include "serveractivityfetcher.h"

#include "account.h"
#include "accountmanager.h"
#include "accountstate.h"
#include "capabilities.h"
#include "guiutility.h"

#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QUrlQuery>

#include <algorithm>
#include <chrono>

using namespace std::chrono_literals;

namespace OCC {

Q_LOGGING_CATEGORY(lcServerActivity, "gui.serveractivity", QtInfoMsg)

namespace {
    const QString notificationsPath = QStringLiteral("ocs/v2.php/apps/notifications/api/v1/notifications");
    const QString activityPath = QStringLiteral("ocs/v2.php/cloud/activity");

    constexpr auto pollInterval = 5min;
    constexpr qint64 activityThrottleSecs = 60;
    constexpr qint64 retryAfterFailureSecs = 3600;

    bool canQuery(AccountState *accountState)
    {
        return accountState && accountState->account() && accountState->isConnected();
    }

    QJsonArray ocsData(const JsonApiJob *job)
    {
        return job->data().value(QLatin1String("ocs")).toObject().value(QLatin1String("data")).toArray();
    }

    // Benachrichtigungs-IDs landen im URL-Pfad: nur einfache Zeichen, kein / . % oder ?
    bool isPlainId(const QString &id)
    {
        return !id.isEmpty() && std::all_of(id.cbegin(), id.cend(), [](QChar c) { return c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'); });
    }
}

ServerActivityFetcher::ServerActivityFetcher(RecentActivityModel *activities, ServerNotificationModel *notifications, QObject *parent)
    : QObject(parent)
    , _activities(activities)
    , _notifications(notifications)
{
    _pollTimer.setInterval(pollInterval);
    connect(&_pollTimer, &QTimer::timeout, this, [this] {
        refreshNotifications();
        refreshActivities();
    });
    _pollTimer.start();

    auto *manager = AccountManager::instance();
    connect(manager, &AccountManager::accountAdded, this, &ServerActivityFetcher::onAccountAdded);
    connect(manager, &AccountManager::accountRemoved, this, &ServerActivityFetcher::onAccountRemoved);
    for (auto *accountState : manager->accounts()) {
        onAccountAdded(accountState);
    }
}

void ServerActivityFetcher::onAccountAdded(AccountState *accountState)
{
    if (!accountState) {
        return;
    }
    // nach dem (Wieder-)Verbinden sofort abfragen, nicht erst nach dem nächsten Intervall
    connect(accountState, &AccountState::stateChanged, this, [this, accountState](AccountState::State state) {
        if (state != AccountState::Connected || !accountState->account()) {
            return;
        }
        // eine neue Verbindung kann eine geänderte Serverkonfiguration bedeuten: Sperren nach Fehlern aufheben
        auto &data = _accountData[accountState->account()->uuid()];
        data.activityPausedUntil = {};
        data.notificationsPausedUntil = {};
        fetchNotifications(accountState);
        fetchActivities(accountState, true);
    });
    if (canQuery(accountState)) {
        fetchNotifications(accountState);
        fetchActivities(accountState, true);
    }
}

void ServerActivityFetcher::onAccountRemoved(AccountState *accountState)
{
    if (!accountState || !accountState->account()) {
        return;
    }
    const auto accountId = accountState->account()->uuid();
    _accountData.remove(accountId);
    _activities->removeAccount(accountId);
    _notifications->removeAccount(accountId);
}

void ServerActivityFetcher::refreshActivities()
{
    for (auto *accountState : AccountManager::instance()->accounts()) {
        fetchActivities(accountState, false);
    }
}

void ServerActivityFetcher::refreshNotifications()
{
    for (auto *accountState : AccountManager::instance()->accounts()) {
        fetchNotifications(accountState);
    }
}

void ServerActivityFetcher::fetchActivities(AccountState *accountState, bool force)
{
    if (!canQuery(accountState)) {
        return;
    }
    auto *account = accountState->account();
    const auto accountId = account->uuid();
    auto &data = _accountData[accountId];
    const auto now = QDateTime::currentDateTimeUtc();
    if (data.activityJob || (data.activityPausedUntil.isValid() && now < data.activityPausedUntil)) {
        return;
    }
    if (!force && data.lastActivityFetch.isValid() && data.lastActivityFetch.secsTo(now) < activityThrottleSecs) {
        return;
    }
    data.lastActivityFetch = now;

    // die Antwort braucht nur die jüngsten Einträge. Der Job gehört dem AccountState und endet mit ihm, nie dem Konto selbst:
    // AbstractNetworkJob verlangt account != parent, mit FORCE_ASSERTS (Beta) brach das Programm beim Verbinden ab
    auto *job = new JsonApiJob(account, activityPath, {{QStringLiteral("start"), QStringLiteral("0")}, {QStringLiteral("count"), QStringLiteral("20")}},
        QNetworkRequest(), accountState);
    // ein 401 dieses optionalen Endpunkts darf keine Neuanmeldung auslösen
    job->setForceIgnoreCredentialFailure(true);
    data.activityJob = job;
    connect(job, &JsonApiJob::finishedSignal, this, [this, job, accountId] {
        auto it = _accountData.find(accountId);
        if (it == _accountData.end()) {
            return;
        }
        const int httpStatus = job->httpStatusCode();
        if (httpStatus == 0) {
            // Netzwerkfehler: beim nächsten Intervall erneut
            return;
        }
        if (httpStatus != 200 || !job->ocsSuccess()) {
            // Aktivitäten-App fehlt oder ist abgeschaltet: eine Stunde Ruhe statt bei jedem Öffnen des Menüs
            qCInfo(lcServerActivity) << "Activities not available for account" << accountId << "HTTP" << httpStatus << "OCS" << job->ocsStatus();
            it->activityPausedUntil = QDateTime::currentDateTimeUtc().addSecs(retryAfterFailureSecs);
            return;
        }
        _activities->setServerActivities(accountId, RecentActivityModel::parseServerActivities(ocsData(job), accountId));
    });
    job->start();
}

void ServerActivityFetcher::fetchNotifications(AccountState *accountState)
{
    if (!canQuery(accountState)) {
        return;
    }
    auto *account = accountState->account();
    if (!account->hasCapabilities() || !account->capabilities().notificationsAvailable()) {
        return;
    }
    const auto accountId = account->uuid();
    auto &data = _accountData[accountId];
    if (data.notificationJob || (data.notificationsPausedUntil.isValid() && QDateTime::currentDateTimeUtc() < data.notificationsPausedUntil)) {
        return;
    }

    auto *job = new JsonApiJob(account, notificationsPath, {}, QNetworkRequest(), accountState);
    job->setForceIgnoreCredentialFailure(true);
    data.notificationJob = job;
    connect(job, &JsonApiJob::finishedSignal, this, [this, job, accountId] {
        auto it = _accountData.find(accountId);
        if (it == _accountData.end()) {
            return;
        }
        const int httpStatus = job->httpStatusCode();
        if (httpStatus == 204) {
            // der Server meldet, dass keine App Benachrichtigungen verschickt: seltener fragen
            it->notificationsPausedUntil = QDateTime::currentDateTimeUtc().addSecs(retryAfterFailureSecs);
            _notifications->setNotifications(accountId, {});
            return;
        }
        if (httpStatus != 200 || !job->ocsSuccess()) {
            qCWarning(lcServerActivity) << "Fetching notifications failed for account" << accountId << "HTTP" << httpStatus << "OCS" << job->ocsStatus();
            return;
        }
        const auto fresh = _notifications->setNotifications(accountId, ServerNotificationModel::parseNotifications(ocsData(job), accountId));
        if (!fresh.isEmpty()) {
            Q_EMIT newNotifications(accountId, fresh);
        }
    });
    job->start();
}

void ServerActivityFetcher::dismissNotification(const QUuid &accountId, const QString &id)
{
    auto *accountState = AccountManager::instance()->accountState(accountId);
    if (!canQuery(accountState) || !isPlainId(id)) {
        Q_EMIT notificationRequestFailed(tr("The notification could not be dismissed because the server is not reachable."));
        return;
    }
    auto *account = accountState->account();
    auto *job = new JsonApiJob(account, notificationsPath + QLatin1Char('/') + id, QByteArrayLiteral("DELETE"), {}, QNetworkRequest(), accountState);
    job->setForceIgnoreCredentialFailure(true);
    connect(job, &JsonApiJob::finishedSignal, this, [this, job, accountId, id] {
        const int httpStatus = job->httpStatusCode();
        // 404: auf dem Server schon erledigt (etwa im Browser) – dann ebenso aus der Liste nehmen
        if ((httpStatus == 200 && job->ocsSuccess()) || httpStatus == 404) {
            _notifications->removeNotification(accountId, id);
            return;
        }
        qCWarning(lcServerActivity) << "Dismissing notification" << id << "failed: HTTP" << httpStatus << "OCS" << job->ocsStatus();
        Q_EMIT notificationRequestFailed(tr("The notification could not be dismissed."));
    });
    job->start();
}

void ServerActivityFetcher::runNotificationAction(const QUuid &accountId, const QString &id, const ServerNotificationAction &action)
{
    if (action.verb == QByteArrayLiteral("WEB")) {
        if (isWebLink(action.link)) {
            Utility::openBrowser(action.link, nullptr);
        }
        return;
    }
    static const QList<QByteArray> allowedVerbs = {QByteArrayLiteral("GET"), QByteArrayLiteral("POST"), QByteArrayLiteral("PUT"), QByteArrayLiteral("DELETE")};
    if (!allowedVerbs.contains(action.verb)) {
        qCWarning(lcServerActivity) << "Ignoring notification action with unknown type" << action.verb;
        return;
    }
    auto *accountState = AccountManager::instance()->accountState(accountId);
    if (!canQuery(accountState)) {
        Q_EMIT notificationRequestFailed(tr("The action could not be carried out because the server is not reachable."));
        return;
    }
    auto *account = accountState->account();
    QString path;
    QList<QPair<QString, QString>> query;
    if (!apiPathForLink(account->url(), action.link, &path, &query)) {
        qCWarning(lcServerActivity) << "Refusing notification action outside the account's server:" << action.link;
        Q_EMIT notificationRequestFailed(tr("The action points to another server and was not carried out."));
        return;
    }
    auto *job = new JsonApiJob(account, path, action.verb, query, QNetworkRequest(), accountState);
    job->setForceIgnoreCredentialFailure(true);
    connect(job, &JsonApiJob::finishedSignal, this, [this, job, accountId, id] {
        const int httpStatus = job->httpStatusCode();
        // OCS v1 antwortet auch bei Fehlern mit HTTP 200; dann entscheidet der OCS-Status
        const bool isOcs = job->data().contains(QLatin1String("ocs"));
        const bool ok = httpStatus >= 200 && httpStatus < 300 && (!isOcs || job->ocsSuccess());
        if (!ok) {
            qCWarning(lcServerActivity) << "Notification action failed: HTTP" << httpStatus << "OCS" << job->ocsStatus();
            Q_EMIT notificationRequestFailed(tr("The action could not be carried out."));
            return;
        }
        _notifications->removeNotification(accountId, id);
        // die Aktion kann weitere Benachrichtigungen erledigt haben: Stand vom Server holen
        fetchNotifications(AccountManager::instance()->accountState(accountId));
    });
    job->start();
}

bool ServerActivityFetcher::apiPathForLink(const QUrl &accountUrl, const QUrl &link, QString *path, QList<QPair<QString, QString>> *query)
{
    const QUrl base = accountUrl.adjusted(QUrl::NormalizePathSegments);
    const QString scheme = base.scheme();
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http")) {
        return false;
    }
    // relative Links gelten relativ zur Server-Adresse
    const QUrl target = (link.isRelative() ? base.resolved(link) : link).adjusted(QUrl::NormalizePathSegments);
    const int defaultPort = scheme == QLatin1String("https") ? 443 : 80;
    if (!target.isValid() || target.scheme() != scheme || target.host().compare(base.host(), Qt::CaseInsensitive) != 0
        || target.port(defaultPort) != base.port(defaultPort) || !target.userInfo().isEmpty()) {
        return false;
    }
    QString basePath = base.path();
    if (!basePath.endsWith(QLatin1Char('/'))) {
        basePath.append(QLatin1Char('/'));
    }
    const QString targetPath = target.path();
    if (!targetPath.startsWith(basePath)) {
        return false;
    }
    const QString relativePath = targetPath.mid(basePath.size());
    if (relativePath.isEmpty() || relativePath.split(QLatin1Char('/')).contains(QStringLiteral(".."))) {
        return false;
    }
    *path = relativePath;
    query->clear();
    const auto items = QUrlQuery(target).queryItems(QUrl::FullyDecoded);
    for (const auto &item : items) {
        query->append({item.first, item.second});
    }
    return true;
}

bool ServerActivityFetcher::isWebLink(const QUrl &url)
{
    return url.isValid() && !url.host().isEmpty() && (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"));
}

} // namespace OCC
