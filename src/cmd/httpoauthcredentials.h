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

#include "accessmanager.h"
#include "creds/abstractcredentials.h"
#include "creds/credentialssupport.h"

#include <QElapsedTimer>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

#include <memory>
#include <utility>

class QLockFile;

namespace OCC {

class OAuthTokenFile;

/**
 * OAuth2-Anmeldung von owncloudcmd mit einer Token-Datei (siehe OAuthTokenFile).
 *
 * Wie beim Desktop-Client (gui/creds/credentials.cpp) trägt jede Anfrage das Access-Token
 * als Bearer. Ist es abgelaufen oder lehnt der Server es mit 401 ab, wird es mit dem
 * Refresh-Token am Token-Endpunkt erneuert: Client-Kennung, client_secret_basic und
 * grant_type=refresh_token wie in AccountBasedOAuth::refreshAuthentication. Solange das
 * läuft, hält das Konto seine Aufträge an (authenticationStarted, siehe Account) und
 * wiederholt die abgelehnten danach mit dem neuen Token. Die neuen Tokens landen sofort
 * wieder in der Datei, weil die oauth2-App das alte Refresh-Token beim Erneuern verwirft.
 *
 * Die GUI-Klassen selbst lassen sich nicht wiederverwenden: sie hängen an der GUI-Bibliothek
 * (Browser, Theme-Seiten, Schlüsselbund), die der Kommandozeilen-Client nicht mitbringt.
 *
 * Scheitert das Erneuern, folgt authenticationFailed() mit errorString(); signInRequired()
 * sagt, ob nur eine neue Anmeldung hilft.
 */
class HttpOAuthCredentials : public AbstractCredentials
{
    Q_OBJECT
public:
    HttpOAuthCredentials(Account *account, const std::shared_ptr<OAuthTokenFile> &tokens);
    ~HttpOAuthCredentials() override;

    AccessManager *createAccessManager() const override;
    bool ready() const override;
    void fetchFromKeychain() override;
    void askFromUser() override;
    bool stillValid(QNetworkReply *reply) override;
    void persist() override { }
    void invalidateToken() override { }
    void forgetSensitiveData() override { }
    bool refreshAccessToken() override;

    /** Authorization-Header für eine Anfrage an url; leer für fremde Server. */
    QByteArray authorizationFor(const QUrl &url) const;

    QString errorString() const { return _errorString; }
    bool signInRequired() const { return _signInRequired; }

private:
    void sendRefreshRequest();
    void handleRefreshReply(QNetworkReply *reply);
    void refreshDone();
    void releaseLock();
    void fail(const QString &message, bool signInRequired);

    std::shared_ptr<OAuthTokenFile> _tokens;
    // sperrt die Token-Datei gegen gleichzeitiges Erneuern durch andere owncloudcmd-Läufe
    std::unique_ptr<QLockFile> _lock;
    QElapsedTimer _sinceRefresh;
    QString _errorString;
    bool _refreshing = false;
    bool _failed = false;
    bool _signInRequired = false;
};

/**
 * Setzt das Access-Token als "Authorization: Bearer" in jede Anfrage an den Server des
 * Kontos, wie CredentialsAccessManager in der GUI. Anfragen an andere Hosts und solche mit
 * DontAddCredentialsAttribute bleiben ohne Token. Base ist der erweiterte Access-Manager;
 * die Tests setzen hier FakeAM ein.
 */
template <typename Base>
class BearerAccessManager : public Base
{
public:
    template <typename... Args>
    explicit BearerAccessManager(const HttpOAuthCredentials *credentials, Args &&...args)
        : Base(std::forward<Args>(args)...)
        , _credentials(credentials)
    {
    }

protected:
    QNetworkReply *createRequest(QNetworkAccessManager::Operation op, const QNetworkRequest &request, QIODevice *outgoingData) override
    {
        QNetworkRequest authorized(request);
        if (_credentials && !authorized.attribute(DontAddCredentialsAttribute).toBool()) {
            const QByteArray header = _credentials->authorizationFor(authorized.url());
            if (!header.isEmpty()) {
                authorized.setRawHeader(QByteArrayLiteral("Authorization"), header);
            }
        }
        return Base::createRequest(op, authorized, outgoingData);
    }

private:
    QPointer<const HttpOAuthCredentials> _credentials;
};

} // namespace OCC
