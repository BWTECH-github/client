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
#include <QTimer>

#include <chrono>
#include <memory>

class QLockFile;

namespace OCC {

class OAuthTokenFile;

/**
 * OAuth2-Anmeldung von owncloudcmd mit einer Token-Datei (siehe OAuthTokenFile).
 *
 * Wie beim Desktop-Client (gui/creds/credentials.cpp) trägt jede Anfrage das Access-Token
 * als Bearer. Läuft es ab (vor dem Lauf und vor jeder Anfrage geprüft) oder lehnt der Server
 * es mit 401 ab, wird es mit dem Refresh-Token am Token-Endpunkt erneuert: Client-Kennung,
 * client_secret_basic und grant_type=refresh_token wie in AccountBasedOAuth::refreshAuthentication,
 * ohne Cookies. Es läuft höchstens eine Erneuerung; solange hält das Konto seine Aufträge an
 * (authenticationStarted, siehe Account) und wiederholt die abgelehnten danach mit dem neuen
 * Token. Die neuen Tokens landen in der Datei, bevor sie benutzt werden, weil die oauth2-App das
 * alte Refresh-Token beim Erneuern verwirft.
 *
 * Scheitert das Erneuern vorübergehend (Netz, Zeitüberschreitung, 5xx, Wartung, unerwartete
 * Antwort), folgen nach Pausen weitere Versuche; das Refresh-Token bleibt. Erst wenn auch die
 * scheitern oder der Server die Anfrage mit einem anderen Fehler als invalid_grant ablehnt,
 * endet der Lauf (authenticationFailed() mit errorString()). signInRequired() ist nur bei
 * invalid_grant gesetzt: dann hilft nur eine neue Anmeldung.
 *
 * Die GUI-Klassen selbst lassen sich nicht wiederverwenden: sie hängen an der GUI-Bibliothek
 * (Browser, Theme-Seiten, Schlüsselbund), die der Kommandozeilen-Client nicht mitbringt.
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
    void refreshIfExpired() override;

    /** Authorization-Header für eine Anfrage an url; leer für fremde Server. */
    QByteArray authorizationFor(const QUrl &url) const;

    QString errorString() const { return _errorString; }
    bool signInRequired() const { return _signInRequired; }

    /** So viele Versuche, wenn das Erneuern vorübergehend scheitert; dann endet der Lauf. */
    static constexpr int refreshAttempts = 6;
    /** Pause vor dem n-ten erneuten Versuch: 5 s, 15 s, 30 s, 60 s, 120 s. */
    static std::chrono::milliseconds defaultRefreshRetryDelay(int failedAttempts);

protected:
    // die Tests verkürzen die Pausen
    virtual std::chrono::milliseconds refreshRetryDelay(int failedAttempts) const;

private:
    /** Vorlauf vor dem Ablauf, ab dem erneuert wird: ein Fünftel der Lebensdauer, höchstens 60 s. */
    std::chrono::seconds refreshMargin() const;
    void startRefresh();
    void sendRefreshRequest();
    void handleRefreshReply(QNetworkReply *reply);
    void retryLater(const QString &message);
    bool saveTokens(QString *error);
    void refreshDone();
    void releaseLock();
    void fail(const QString &message, bool signInRequired);

    std::shared_ptr<OAuthTokenFile> _tokens;
    // sperrt die Token-Datei gegen gleichzeitiges Erneuern durch andere owncloudcmd-Läufe
    std::unique_ptr<QLockFile> _lock;
    QElapsedTimer _sinceRefresh;
    QTimer _retryTimer;
    int _failedRefreshes = 0;
    QString _errorString;
    bool _refreshing = false;
    bool _failed = false;
    bool _signInRequired = false;
    // der Server hat das aktuelle Access-Token schon einmal angenommen
    bool _accessTokenWorked = false;
    // erneuerte Tokens, die nicht in die Datei geschrieben werden konnten
    bool _unsavedTokens = false;
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
    BearerAccessManager(const HttpOAuthCredentials *credentials, QObject *parent)
        : Base(parent)
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
