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

#include "httpoauthcredentials.h"

#include "account.h"
#include "config/appconfig.h"
#include "oauthtokenfile.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QLockFile>
#include <QLoggingCategory>

using namespace std::chrono_literals;

Q_LOGGING_CATEGORY(lcCmdOAuth, "owncloudcmd.oauth", QtInfoMsg)

namespace OCC {

namespace {
    // Läuft das Access-Token innerhalb dieser Zeit ab, wird es vor dem Lauf erneuert
    constexpr auto expiryMargin = 60s;
    // Lehnt der Server ein eben erst ausgestelltes Token ab, hilft weiteres Erneuern nicht
    constexpr qint64 freshTokenGraceMs = 30 * 1000;
    constexpr int refreshTimeoutMs = 30 * 1000;
    // länger, als das Erneuern eines anderen Laufs dauern kann (refreshTimeoutMs)
    constexpr int lockTimeoutMs = 40 * 1000;

    int effectivePort(const QUrl &url)
    {
        return url.port(url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 ? 443 : 80);
    }

    bool sameOrigin(const QUrl &a, const QUrl &b)
    {
        return a.scheme().compare(b.scheme(), Qt::CaseInsensitive) == 0 && a.host().compare(b.host(), Qt::CaseInsensitive) == 0
            && effectivePort(a) == effectivePort(b);
    }

    // application/x-www-form-urlencoded: alles außer den nicht reservierten Zeichen
    // kodieren; QUrlQuery ließe "+" stehen, das der Server als Leerzeichen liest
    QByteArray formEncode(const QList<std::pair<QByteArray, QString>> &fields)
    {
        QByteArray body;
        for (const auto &[key, value] : fields) {
            if (!body.isEmpty()) {
                body.append('&');
            }
            body.append(key);
            body.append('=');
            body.append(QUrl::toPercentEncoding(value));
        }
        return body;
    }
}

HttpOAuthCredentials::HttpOAuthCredentials(Account *account, const std::shared_ptr<OAuthTokenFile> &tokens)
    : AbstractCredentials(account, account)
    , _tokens(tokens)
{
}

HttpOAuthCredentials::~HttpOAuthCredentials() = default;

AccessManager *HttpOAuthCredentials::createAccessManager() const
{
    // gehört den Credentials, wie bei den anderen Anmeldearten (siehe Account::setCredentials)
    return new BearerAccessManager<AccessManager>(this, const_cast<HttpOAuthCredentials *>(this));
}

bool HttpOAuthCredentials::ready() const
{
    return !_failed && !_tokens->accessToken().isEmpty();
}

QByteArray HttpOAuthCredentials::authorizationFor(const QUrl &url) const
{
    if (!_account || _tokens->accessToken().isEmpty() || !sameOrigin(url, _account->url())) {
        return {};
    }
    return QByteArrayLiteral("Bearer ") + _tokens->accessToken().toUtf8();
}

void HttpOAuthCredentials::fetchFromKeychain()
{
    _wasEverFetched = true;
    if (!_tokens->needsRefresh(expiryMargin)) {
        Q_EMIT fetched();
        return;
    }
    if (_tokens->refreshToken().isEmpty()) {
        fail(tr("The access token has expired and the token file has no refresh token. Sign in again to create a new token file."), true);
        return;
    }
    qCInfo(lcCmdOAuth) << "The access token has expired or expires soon, refreshing it";
    refreshAccessToken();
}

void HttpOAuthCredentials::askFromUser()
{
    fail(tr("owncloudcmd cannot sign in interactively. Sign in again to create a new token file."), true);
}

bool HttpOAuthCredentials::stillValid(QNetworkReply *reply)
{
    if (!reply || reply->error() != QNetworkReply::AuthenticationRequiredError || _failed || _refreshing) {
        return true;
    }
    // Die Anfrage trug kein oder ein älteres Token; ihre Wiederholung bekommt das aktuelle
    const QByteArray sent = reply->request().rawHeader(QByteArrayLiteral("Authorization"));
    if (sent.isEmpty() || sent != authorizationFor(reply->request().url())) {
        return true;
    }
    if (_sinceRefresh.isValid() && _sinceRefresh.elapsed() < freshTokenGraceMs) {
        fail(tr("The server rejected the access token it has just issued (HTTP 401). Check that the web server passes the Authorization "
                "header on to owncloud.online."),
            false);
        return true;
    }
    if (_tokens->refreshToken().isEmpty()) {
        fail(tr("The server rejected the access token (HTTP 401) and the token file has no refresh token. Sign in again to create a new token "
                "file."),
            true);
        return true;
    }
    qCInfo(lcCmdOAuth) << "The server rejected the access token, refreshing it";
    refreshAccessToken();
    return true;
}

bool HttpOAuthCredentials::refreshAccessToken()
{
    if (_failed || _tokens->refreshToken().isEmpty()) {
        return false;
    }
    if (_refreshing) {
        return true;
    }
    _refreshing = true;
    // Account hält seine Aufträge an, bis fetched() oder authenticationFailed() folgt
    Q_EMIT authenticationStarted();

    const QString lockPath = _tokens->path() + QStringLiteral(".lock");
    _lock = std::make_unique<QLockFile>(lockPath);
    if (!_lock->tryLock(lockTimeoutMs)) {
        fail(tr("The token file %1 is locked by another owncloudcmd run.").arg(_tokens->path()), false);
        return true;
    }
    QString error;
    switch (_tokens->reloadIfChanged(&error)) {
    case OAuthTokenFile::ReloadResult::Failed:
        fail(tr("Could not read the token file again: %1").arg(error), false);
        return true;
    case OAuthTokenFile::ReloadResult::Changed:
        // Ein anderer Lauf hat inzwischen erneuert; sein Refresh-Token ersetzt unseres, das
        // die oauth2-App dabei verworfen hat
        if (!_tokens->needsRefresh(expiryMargin)) {
            qCInfo(lcCmdOAuth) << "Using the tokens another owncloudcmd run has just stored";
            refreshDone();
            return true;
        }
        break;
    case OAuthTokenFile::ReloadResult::Unchanged:
        break;
    }
    sendRefreshRequest();
    return true;
}

void HttpOAuthCredentials::sendRefreshRequest()
{
    if (!_account) {
        fail(tr("Cannot refresh the access token without an account."), false);
        return;
    }
    // derselbe OAuth2-Client wie beim Desktop-Client, sofern die Token-Datei keinen anderen nennt
    const OpenIdConfig desktopClient = AppConfig().openIdConfig();
    const QString clientId = _tokens->clientId().isEmpty() ? desktopClient.clientId() : _tokens->clientId();
    QString clientSecret = _tokens->clientSecret();
    if (clientSecret.isEmpty() && clientId == desktopClient.clientId()) {
        clientSecret = desktopClient.clientSecret();
    }

    QNetworkRequest request(_tokens->tokenEndpoint());
    request.setAttribute(DontAddCredentialsAttribute, true);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
    request.setTransferTimeout(refreshTimeoutMs);
    QList<std::pair<QByteArray, QString>> form{
        {QByteArrayLiteral("grant_type"), QStringLiteral("refresh_token")}, {QByteArrayLiteral("refresh_token"), _tokens->refreshToken()}};
    if (!clientSecret.isEmpty()) {
        // client_secret_basic wie OAuth::postTokenRequest beim Desktop-Client
        request.setRawHeader(
            QByteArrayLiteral("Authorization"), QByteArrayLiteral("Basic ") + QStringLiteral("%1:%2").arg(clientId, clientSecret).toUtf8().toBase64());
    } else {
        // öffentlicher Client ohne Geheimnis: die Kennung steht im Formular
        form.emplaceBack(QByteArrayLiteral("client_id"), clientId);
    }
    QNetworkReply *reply = _account->accessManager()->post(request, formEncode(form));
    connect(reply, &QNetworkReply::finished, this, [this, reply] { handleRefreshReply(reply); });
}

void HttpOAuthCredentials::handleRefreshReply(QNetworkReply *reply)
{
    reply->deleteLater();
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QJsonParseError parseError;
    const QJsonObject json = QJsonDocument::fromJson(reply->readAll(), &parseError).object();

    const QString error = json.value(QStringLiteral("error")).toString();
    if (!error.isEmpty()) {
        const QString description = json.value(QStringLiteral("error_description")).toString();
        const QString detail = description.isEmpty() ? error : QStringLiteral("%1: %2").arg(error, description);
        // wie AccountBasedOAuth::refreshAuthentication: dann gilt das Refresh-Token nicht mehr
        if (error == QLatin1String("invalid_grant") || error == QLatin1String("invalid_request")) {
            fail(tr("The server no longer accepts the refresh token (%1). The sign-in has expired or was revoked; sign in again to create a "
                    "new token file.")
                     .arg(detail),
                true);
        } else {
            fail(tr("The server refused to refresh the access token (%1).").arg(detail), false);
        }
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        fail(tr("Refreshing the access token failed: %1 (HTTP %2).").arg(reply->errorString()).arg(httpStatus), false);
        return;
    }
    const QString tokenType = json.value(QStringLiteral("token_type")).toString();
    if (parseError.error != QJsonParseError::NoError || json.value(QStringLiteral("access_token")).toString().isEmpty()
        || (!tokenType.isEmpty() && tokenType.compare(QLatin1String("bearer"), Qt::CaseInsensitive) != 0)) {
        fail(tr("The token endpoint answered without a usable access token (HTTP %1).").arg(httpStatus), false);
        return;
    }

    _tokens->updateFromTokenResponse(json);
    QString saveError;
    if (!_tokens->save(&saveError)) {
        // das alte Refresh-Token hat der Server schon verworfen
        fail(tr("Could not store the refreshed tokens in %1: %2").arg(_tokens->path(), saveError), true);
        return;
    }
    qCInfo(lcCmdOAuth) << "Refreshed the access token, valid until" << _tokens->expiresAt().toString(Qt::ISODate);
    refreshDone();
}

void HttpOAuthCredentials::refreshDone()
{
    releaseLock();
    _refreshing = false;
    _sinceRefresh.start();
    Q_EMIT fetched();
}

void HttpOAuthCredentials::releaseLock()
{
    // QLockFile gibt die Sperre beim Abbau frei
    _lock.reset();
}

void HttpOAuthCredentials::fail(const QString &message, bool signInRequired)
{
    releaseLock();
    _refreshing = false;
    if (_failed) {
        return;
    }
    _failed = true;
    _signInRequired = signInRequired;
    _errorString = message;
    Q_EMIT authenticationFailed();
}

} // namespace OCC
