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

#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <chrono>
#include <memory>

namespace OCC {

/**
 * Token-Datei von owncloudcmd (--oauth-token-file): ein JSON-Objekt, wie es das
 * Anmeldeskript test/e2e/oauth/login.py schreibt oder ein Token-Endpunkt liefert.
 *
 *   server_url      Server, für den die Tokens gelten (Pflicht)
 *   access_token    wird als "Authorization: Bearer" gesendet
 *   refresh_token   ohne dieses endet ein Lauf, sobald das Access-Token abläuft
 *   expires_at      Ablauf des Access-Tokens in Sekunden seit 1970 (optional)
 *   expires_in      Lebensdauer in Sekunden; ohne expires_at ab der Änderungszeit der Datei
 *   user_id         Benutzer, für den die Tokens ausgestellt wurden (optional)
 *   token_endpoint  Token-Endpunkt zum Erneuern (optional, sonst der der oauth2-App)
 *   client_id, client_secret  OAuth2-Client (optional, sonst der des Desktop-Clients)
 *
 * Unbekannte Felder bleiben erhalten, wenn owncloudcmd erneuerte Tokens zurückschreibt.
 */
class OAuthTokenFile
{
public:
    enum class ReloadResult { Unchanged, Changed, Failed };

    /** Liest und prüft die Datei; nullptr und *error bei einem Fehler. */
    static std::shared_ptr<OAuthTokenFile> load(const QString &path, QString *error);

    QString path() const { return _path; }
    QUrl serverUrl() const;
    /** Ob die Tokens zu diesem Server gehören (Schema, Host, Port und Pfad). */
    bool belongsTo(const QUrl &serverUrl) const;

    QString accessToken() const;
    QString refreshToken() const;
    QString userId() const;
    QUrl tokenEndpoint() const;
    QString clientId() const;
    QString clientSecret() const;
    /** Ablauf des Access-Tokens; ungültig, wenn unbekannt. */
    QDateTime expiresAt() const;
    /** Lebensdauer des Access-Tokens laut Token-Endpunkt (expires_in); 0, wenn unbekannt. */
    std::chrono::seconds lifetime() const;
    /** Ob das Access-Token fehlt, abgelaufen ist oder innerhalb von margin abläuft. */
    bool needsRefresh(std::chrono::seconds margin) const;

    /** Übernimmt die Antwort eines Token-Endpunkts (access_token, refresh_token, expires_in, user_id). */
    void updateFromTokenResponse(const QJsonObject &response);

    /** Übernimmt den Stand auf der Platte, wenn ein anderer Prozess dort inzwischen neue Tokens abgelegt hat. */
    ReloadResult reloadIfChanged(QString *error);

    /** Schreibt die Datei atomar und nur für den Eigentümer lesbar. */
    bool save(QString *error);

private:
    bool read(QJsonObject *json, QDateTime *modified, QString *error) const;

    QString _path;
    QJsonObject _json;
    // Änderungszeit beim Lesen: Bezugspunkt für expires_in ohne expires_at
    QDateTime _modified;
};

} // namespace OCC
