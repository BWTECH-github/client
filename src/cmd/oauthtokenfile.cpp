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

#include "oauthtokenfile.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>

#include <optional>

namespace OCC {

namespace {
    const QString serverUrlC = QStringLiteral("server_url");
    const QString accessTokenC = QStringLiteral("access_token");
    const QString refreshTokenC = QStringLiteral("refresh_token");
    const QString tokenTypeC = QStringLiteral("token_type");
    const QString expiresAtC = QStringLiteral("expires_at");
    const QString expiresInC = QStringLiteral("expires_in");
    const QString userIdC = QStringLiteral("user_id");
    const QString tokenEndpointC = QStringLiteral("token_endpoint");
    const QString clientIdC = QStringLiteral("client_id");
    const QString clientSecretC = QStringLiteral("client_secret");

    // Token-Endpunkt der oauth2-App; den nimmt auch der Desktop-Client, wenn der Server
    // kein .well-known/openid-configuration anbietet (gui/creds/oauth.cpp)
    const QString oauth2AppTokenPathC = QStringLiteral("/index.php/apps/oauth2/api/v1/token");

    int effectivePort(const QUrl &url)
    {
        return url.port(url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0 ? 443 : 80);
    }

    QString comparablePath(const QUrl &url)
    {
        QString path = url.adjusted(QUrl::NormalizePathSegments).path();
        while (path.endsWith(QLatin1Char('/'))) {
            path.chop(1);
        }
        return path;
    }

    // Sekundenangaben kommen je nach Server als Zahl oder als Zeichenkette
    std::optional<qint64> secondsValue(const QJsonValue &value)
    {
        if (value.isDouble()) {
            return value.toInteger();
        }
        if (value.isString()) {
            bool ok = false;
            const qint64 result = value.toString().toLongLong(&ok);
            if (ok) {
                return result;
            }
        }
        return std::nullopt;
    }
}

std::shared_ptr<OAuthTokenFile> OAuthTokenFile::load(const QString &path, QString *error)
{
    auto file = std::make_shared<OAuthTokenFile>();
    file->_path = QFileInfo(path).absoluteFilePath();
    if (!file->read(&file->_json, &file->_modified, error)) {
        return nullptr;
    }
    if (!file->serverUrl().isValid() || file->serverUrl().host().isEmpty()) {
        *error = QStringLiteral("%1 has no valid server_url").arg(file->_path);
        return nullptr;
    }
    if (file->accessToken().isEmpty() && file->refreshToken().isEmpty()) {
        *error = QStringLiteral("%1 contains neither an access_token nor a refresh_token").arg(file->_path);
        return nullptr;
    }
    // Erneuerte Tokens müssen zurück in die Datei: die oauth2-App verwirft das alte
    // Refresh-Token beim Erneuern, ohne Schreibrecht wäre die Anmeldung danach verloren
    const QFileInfo info(file->_path);
    if (!info.isWritable() || !QFileInfo(info.absolutePath()).isWritable()) {
        *error = QStringLiteral("%1 or its folder is not writable; owncloudcmd stores refreshed tokens there").arg(file->_path);
        return nullptr;
    }
    return file;
}

bool OAuthTokenFile::read(QJsonObject *json, QDateTime *modified, QString *error) const
{
    QFile file(_path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("cannot read %1: %2").arg(_path, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = QStringLiteral("%1 is not valid JSON: %2").arg(_path, parseError.errorString());
        return false;
    }
    if (!document.isObject()) {
        *error = QStringLiteral("%1 does not contain a JSON object").arg(_path);
        return false;
    }
    *json = document.object();
    *modified = QFileInfo(_path).lastModified();
    return true;
}

QUrl OAuthTokenFile::serverUrl() const
{
    return QUrl::fromUserInput(_json.value(serverUrlC).toString());
}

bool OAuthTokenFile::belongsTo(const QUrl &serverUrl) const
{
    const QUrl own = this->serverUrl();
    return own.scheme().compare(serverUrl.scheme(), Qt::CaseInsensitive) == 0 && own.host().compare(serverUrl.host(), Qt::CaseInsensitive) == 0
        && effectivePort(own) == effectivePort(serverUrl) && comparablePath(own) == comparablePath(serverUrl);
}

QString OAuthTokenFile::accessToken() const
{
    return _json.value(accessTokenC).toString();
}

QString OAuthTokenFile::refreshToken() const
{
    return _json.value(refreshTokenC).toString();
}

QString OAuthTokenFile::userId() const
{
    return _json.value(userIdC).toString();
}

QUrl OAuthTokenFile::tokenEndpoint() const
{
    const QString endpoint = _json.value(tokenEndpointC).toString();
    if (!endpoint.isEmpty()) {
        return QUrl(endpoint);
    }
    QUrl url = serverUrl();
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    url.setPath(path + oauth2AppTokenPathC);
    return url;
}

QString OAuthTokenFile::clientId() const
{
    return _json.value(clientIdC).toString();
}

QString OAuthTokenFile::clientSecret() const
{
    return _json.value(clientSecretC).toString();
}

QDateTime OAuthTokenFile::expiresAt() const
{
    if (const auto at = secondsValue(_json.value(expiresAtC))) {
        return QDateTime::fromSecsSinceEpoch(*at);
    }
    if (const auto in = secondsValue(_json.value(expiresInC)); in && _modified.isValid()) {
        return _modified.addSecs(*in);
    }
    return {};
}

bool OAuthTokenFile::needsRefresh(std::chrono::seconds margin) const
{
    if (accessToken().isEmpty()) {
        return true;
    }
    const QDateTime expiry = expiresAt();
    return expiry.isValid() && QDateTime::currentDateTimeUtc().addSecs(margin.count()) >= expiry;
}

void OAuthTokenFile::updateFromTokenResponse(const QJsonObject &response)
{
    _json.insert(accessTokenC, response.value(accessTokenC).toString());
    _json.insert(tokenTypeC, QStringLiteral("Bearer"));
    // Ohne neues Refresh-Token gilt das bisherige weiter (RFC 6749, Abschnitt 6)
    if (response.value(refreshTokenC).isString()) {
        _json.insert(refreshTokenC, response.value(refreshTokenC).toString());
    }
    if (const auto expiresIn = secondsValue(response.value(expiresInC))) {
        _json.insert(expiresInC, *expiresIn);
        _json.insert(expiresAtC, QDateTime::currentSecsSinceEpoch() + *expiresIn);
    } else {
        _json.remove(expiresInC);
        _json.remove(expiresAtC);
    }
    if (response.value(userIdC).isString()) {
        _json.insert(userIdC, response.value(userIdC).toString());
    }
}

OAuthTokenFile::ReloadResult OAuthTokenFile::reloadIfChanged(QString *error)
{
    QJsonObject json;
    QDateTime modified;
    if (!read(&json, &modified, error)) {
        return ReloadResult::Failed;
    }
    if (json.value(refreshTokenC) == _json.value(refreshTokenC)) {
        return ReloadResult::Unchanged;
    }
    if (QUrl::fromUserInput(json.value(serverUrlC).toString()) != serverUrl()) {
        *error = QStringLiteral("%1 now belongs to another server").arg(_path);
        return ReloadResult::Failed;
    }
    _json = json;
    _modified = modified;
    return ReloadResult::Changed;
}

bool OAuthTokenFile::save(QString *error)
{
    QSaveFile file(_path);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = file.errorString();
        return false;
    }
    // Zugangsdaten: schon die temporäre Datei ist nur für den Eigentümer lesbar
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(_json).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        *error = file.errorString();
        return false;
    }
    _modified = QFileInfo(_path).lastModified();
    return true;
}

} // namespace OCC
