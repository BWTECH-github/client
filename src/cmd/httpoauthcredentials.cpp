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
#include <QThread>

#include <algorithm>

using namespace std::chrono;
using namespace std::chrono_literals;

Q_LOGGING_CATEGORY(lcCmdOAuth, "owncloudcmd.oauth", QtInfoMsg)

namespace OCC {

namespace {
    // Lehnt der Server ein eben erst ausgestelltes, noch nie angenommenes Token ab, hilft weiteres Erneuern nicht
    constexpr auto freshTokenGrace = 30s;

    // Anfragen an den Token-Endpunkt nicht nach 30 s abbrechen: Der Server tauscht die Tokens womöglich trotzdem aus (eine
    // Bremse gegen Anmeldeversuche wartet bis zu 30 s, eine gesperrte Datenbank länger), die Antwort ginge verloren und mit
    // ihr das neue Refresh-Token. Länger aber auch nicht: Die oauth2-App ab 0.10.0 nimmt das vorige Refresh-Token noch
    // 180 s an, die Wiederholung muss da hineinpassen.
    milliseconds refreshTimeout()
    {
        return 120s;
    }

    // länger, als das Erneuern eines anderen Laufs dauern kann
    milliseconds lockTimeout()
    {
        return refreshTimeout() + 30s;
    }

    int effectivePort(const QUrl &url)
    {
        return url.port(url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 ? 443 : 80);
    }

    bool sameOrigin(const QUrl &a, const QUrl &b)
    {
        return a.scheme().compare(b.scheme(), Qt::CaseInsensitive) == 0 && a.host().compare(b.host(), Qt::CaseInsensitive) == 0
            && effectivePort(a) == effectivePort(b);
    }

    // Fehlertexte des Servers landen im Terminal: ohne Steuerzeichen und in begrenzter Länge
    QString printable(const QString &text)
    {
        QString result;
        for (const QChar c : text) {
            if (c.isPrint()) {
                result.append(c);
            }
        }
        return result.left(300);
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
    _retryTimer.setSingleShot(true);
    connect(&_retryTimer, &QTimer::timeout, this, &HttpOAuthCredentials::startRefresh);
}

HttpOAuthCredentials::~HttpOAuthCredentials()
{
    if (_unsavedTokens) {
        // letzter Versuch: ohne die Datei braucht der nächste Lauf eine neue Anmeldung
        QString error;
        if (!_tokens->save(&error)) {
            qCCritical(lcCmdOAuth).noquote() << tr("Could not store the refreshed tokens in %1: %2").arg(_tokens->path(), error);
        }
    }
}

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

std::chrono::seconds HttpOAuthCredentials::refreshMargin() const
{
    const seconds lifetime = _tokens->lifetime();
    return lifetime > 0s ? std::clamp<seconds>(lifetime / 5, 1s, 60s) : 60s;
}

std::chrono::milliseconds HttpOAuthCredentials::defaultRefreshRetryDelay(int failedAttempts)
{
    static const milliseconds delays[] = {5s, 15s, 30s, 60s, 120s};
    return delays[std::clamp(failedAttempts - 1, 0, 4)];
}

std::chrono::milliseconds HttpOAuthCredentials::refreshRetryDelay(int failedAttempts) const
{
    return defaultRefreshRetryDelay(failedAttempts);
}

void HttpOAuthCredentials::fetchFromKeychain()
{
    _wasEverFetched = true;
    if (!_tokens->needsRefresh(refreshMargin())) {
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
    if (!reply || _failed) {
        return true;
    }
    const QByteArray sent = reply->request().rawHeader(QByteArrayLiteral("Authorization"));
    const bool currentToken = !sent.isEmpty() && sent == authorizationFor(reply->request().url());
    if (reply->error() != QNetworkReply::AuthenticationRequiredError) {
        if (currentToken && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 0) {
            // der Server hat das aktuelle Access-Token angenommen
            _accessTokenWorked = true;
        }
        return true;
    }
    // Die Anfrage trug kein oder ein älteres Token, oder die Erneuerung läuft schon: ihre Wiederholung bekommt das aktuelle
    if (_refreshing || !currentToken) {
        return true;
    }
    if (!_accessTokenWorked && _sinceRefresh.isValid() && _sinceRefresh.durationElapsed() < freshTokenGrace) {
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
    _failedRefreshes = 0;
    // Account hält seine Aufträge an, bis fetched() oder authenticationFailed() folgt
    Q_EMIT authenticationStarted();
    startRefresh();
    return true;
}

void HttpOAuthCredentials::refreshIfExpired()
{
    // vor jeder Anfrage: ein Token, das bald abläuft, erst erneuern, statt eine Ablehnung abzuwarten
    if (_failed || _refreshing || _tokens->refreshToken().isEmpty() || !_tokens->needsRefresh(refreshMargin())) {
        return;
    }
    qCInfo(lcCmdOAuth) << "The access token expires soon, refreshing it before the next request";
    refreshAccessToken();
}

void HttpOAuthCredentials::startRefresh()
{
    _lock = std::make_unique<QLockFile>(_tokens->path() + QStringLiteral(".lock"));
    // Ein anderer Lauf kann länger erneuern als die 30 s, nach denen QLockFile eine Sperre sonst für verwaist hält; die Sperre
    // eines beendeten Prozesses erkennt QLockFile weiterhin
    _lock->setStaleLockTime(0);
    if (!_lock->tryLock(static_cast<int>(lockTimeout().count()))) {
        fail(tr("The token file %1 is locked by another owncloudcmd run.").arg(_tokens->path()), false);
        return;
    }
    if (_unsavedTokens) {
        // Die Datei hält noch das verworfene Refresh-Token: nicht von dort lesen, erst unseres hineinschreiben
        QString saveError;
        _unsavedTokens = !saveTokens(&saveError);
    }
    if (!_unsavedTokens) {
        QString error;
        switch (_tokens->reloadIfChanged(&error)) {
        case OAuthTokenFile::ReloadResult::Failed:
            fail(tr("Could not read the token file again: %1").arg(error), false);
            return;
        case OAuthTokenFile::ReloadResult::Changed:
            // Ein anderer Lauf hat inzwischen erneuert; sein Refresh-Token ersetzt unseres, das
            // die oauth2-App dabei verworfen hat
            if (!_tokens->needsRefresh(refreshMargin())) {
                qCInfo(lcCmdOAuth) << "Using the tokens another owncloudcmd run has just stored";
                refreshDone();
                return;
            }
            break;
        case OAuthTokenFile::ReloadResult::Unchanged:
            break;
        }
    }
    sendRefreshRequest();
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
    // ohne die Cookies des Laufs: mit angemeldeter Sitzung wertet der Kern Client-Kennung und -Geheimnis als Anmeldeversuch
    // eines Benutzers („Login failed“) und bremst weitere aus
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    // eine Weiterleitung (etwa auf die Anmeldeseite, wenn die oauth2-App fehlt) ist keine Antwort des Token-Endpunkts
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
    request.setTransferTimeout(static_cast<int>(refreshTimeout().count()));
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

    const QString error = printable(json.value(QStringLiteral("error")).toString());
    if (!error.isEmpty()) {
        const QString description = printable(json.value(QStringLiteral("error_description")).toString());
        const QString detail = description.isEmpty() ? error : QStringLiteral("%1: %2").arg(error, description);
        if (error == QLatin1String("invalid_grant")) {
            // wie AccountBasedOAuth::refreshAuthentication: nur dann gilt das Refresh-Token nicht mehr
            fail(tr("The server no longer accepts the refresh token (%1). The sign-in has expired or was revoked; sign in again to create a "
                    "new token file.")
                     .arg(detail),
                true);
        } else if (httpStatus >= 500 || httpStatus == 429) {
            retryLater(tr("The server refused to refresh the access token (%1).").arg(detail));
        } else {
            // Einrichtung oder Konto (invalid_client, unauthorized_client, invalid_request): kein Grund für eine neue
            // Anmeldung, das Refresh-Token bleibt; ein sofortiger neuer Versuch hilft aber auch nicht
            fail(tr("The server refused to refresh the access token (%1).").arg(detail), false);
        }
        return;
    }

    const QString tokenType = json.value(QStringLiteral("token_type")).toString();
    if (reply->error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300 || parseError.error != QJsonParseError::NoError
        || json.value(QStringLiteral("access_token")).toString().isEmpty()
        || (!tokenType.isEmpty() && tokenType.compare(QLatin1String("bearer"), Qt::CaseInsensitive) != 0)) {
        // Netz, Zeitüberschreitung, 5xx, Proxy, HTML statt JSON (Wartung, oauth2-App aus): das Refresh-Token gilt weiter
        const QString newRefreshToken = json.value(QStringLiteral("refresh_token")).toString();
        if (!newRefreshToken.isEmpty() && newRefreshToken != _tokens->refreshToken()) {
            // der Server hat trotzdem schon gewechselt: das neue behalten
            _tokens->updateFromTokenResponse(json);
            QString saveError;
            _unsavedTokens = !saveTokens(&saveError);
        }
        if (reply->error() != QNetworkReply::NoError) {
            retryLater(tr("Refreshing the access token failed: %1 (HTTP %2).").arg(reply->errorString()).arg(httpStatus));
        } else {
            retryLater(tr("The token endpoint answered without a usable access token (HTTP %1).").arg(httpStatus));
        }
        return;
    }

    _tokens->updateFromTokenResponse(json);
    QString saveError;
    if (saveTokens(&saveError)) {
        _unsavedTokens = false;
    } else {
        // Das alte Refresh-Token hat der Server schon verworfen: mit den neuen Tokens weiterarbeiten, damit der Lauf zu
        // Ende geht, und das Speichern später wiederholen
        _unsavedTokens = true;
        qCCritical(lcCmdOAuth).noquote() << tr("Could not store the refreshed tokens in %1: %2").arg(_tokens->path(), saveError);
    }
    qCInfo(lcCmdOAuth) << "Refreshed the access token, valid until" << _tokens->expiresAt().toString(Qt::ISODate);
    refreshDone();
}

void HttpOAuthCredentials::retryLater(const QString &message)
{
    // andere Läufe dürfen in der Pause erneuern; ihr Ergebnis übernimmt der nächste Versuch
    releaseLock();
    ++_failedRefreshes;
    if (_failedRefreshes >= refreshAttempts) {
        fail(message, false);
        return;
    }
    const auto delay = refreshRetryDelay(_failedRefreshes);
    qCWarning(lcCmdOAuth).noquote() << message << "Retrying in" << duration_cast<milliseconds>(delay).count() << "ms";
    _retryTimer.start(delay);
}

bool HttpOAuthCredentials::saveTokens(QString *error)
{
    // kurz wiederholen: ein Virenscanner oder eine Sicherung kann die Datei einen Moment sperren
    for (int attempt = 1;; ++attempt) {
        if (_tokens->save(error)) {
            return true;
        }
        if (attempt >= 5) {
            return false;
        }
        QThread::msleep(static_cast<unsigned long>(200 * attempt));
    }
}

void HttpOAuthCredentials::refreshDone()
{
    releaseLock();
    _refreshing = false;
    _failedRefreshes = 0;
    _accessTokenWorked = false;
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
    _retryTimer.stop();
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
