/*
 * Copyright (C) by Klaas Freitag <freitag@kde.org>
 * Copyright (C) by Krzesimir Nowak <krzesimir@endocode.com>
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
#include "creds/httpcredentials.h"

#include "accessmanager.h"
#include "account.h"
#include "creds/credentialmanager.h"

#include <QAuthenticator>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QTimer>

#include <algorithm>

using namespace std::chrono;
using namespace std::chrono_literals;

Q_LOGGING_CATEGORY(lcHttpCredentials, "sync.credentials.http", QtInfoMsg)

namespace {
const char authenticationFailedC[] = "owncloud-authentication-failed";

// Lesen beim Start: so oft, wenn der Schlüsselbund noch nicht bereit ist (Autostart, gesperrt), dann neu anmelden
constexpr int keychainReadAttempts = 6;
// nach so vielen gescheiterten Versuchen, das Kennwort zu speichern, wird der Fehler dem Konto gemeldet
constexpr int storeAttemptsBeforeWarning = 3;

// derselbe Schlüssel wie im 6.0.3-Client: ein Konto aus dessen Konfiguration findet sein Kennwort wieder
QString passwordKeyC()
{
    return QStringLiteral("http/password");
}
}

namespace OCC {

class HttpCredentialsAccessManager : public AccessManager
{
    Q_OBJECT
public:
    HttpCredentialsAccessManager(const HttpCredentials *credentials, QObject *parent = nullptr)
        : AccessManager(parent)
        , _credentials(credentials)
    {
    }

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData) override
    {
        return AccessManager::createRequest(op, _credentials ? _credentials->withAuthorization(request) : request, outgoingData);
    }

private:
    // die Zugangsdaten sterben mit dem Konto, der Netzwerkzugang kann sie überleben (CheckServerJob)
    QPointer<const HttpCredentials> _credentials;
};

HttpCredentials::HttpCredentials(Account *account, const QString &loginUser, const QString &password)
    : AbstractCredentials(account, account)
    , _loginUser(loginUser)
    , _password(password)
    , _ready(!loginUser.isEmpty() && !password.isEmpty())
{
}

AccessManager *HttpCredentials::createAccessManager() const
{
    AccessManager *am = new HttpCredentialsAccessManager(this);
    connect(am, &QNetworkAccessManager::authenticationRequired, this, &HttpCredentials::slotAuthentication);
    return am;
}

QNetworkRequest HttpCredentials::withAuthorization(const QNetworkRequest &request) const
{
    if (request.attribute(DontAddCredentialsAttribute).toBool() || _password.isEmpty()) {
        return request;
    }
    QNetworkRequest authorized(request);
    authorized.setRawHeader(QByteArrayLiteral("Authorization"), basicAuthorizationHeader(_loginUser, _password));
    return authorized;
}

bool HttpCredentials::ready() const
{
    return _ready;
}

void HttpCredentials::fetchFromKeychain()
{
    _wasEverFetched = true;
    if (_ready) {
        Q_EMIT fetched();
        return;
    }
    if (_loginUser.isEmpty()) {
        qCWarning(lcHttpCredentials) << "No login user for" << (_account ? _account->url().toString() : QString()) << "- nothing to read from the keychain";
        _password.clear();
        Q_EMIT fetched();
        return;
    }
    fetchPasswordFromKeychain();
}

void HttpCredentials::fetchPasswordFromKeychain()
{
    if (!_account) {
        return;
    }
    readPassword([this](KeychainResult result, const QString &password, const QString &error) {
        if (result == KeychainResult::Ok && !password.isEmpty()) {
            _keychainReadFailures = 0;
            _password = password;
            _storedPassword = password;
            _ready = true;
            Q_EMIT fetched();
            return;
        }
        if (result == KeychainResult::Error && ++_keychainReadFailures < keychainReadAttempts) {
            // Der Schlüsselbund ist noch nicht bereit (Autostart, gesperrt): später erneut lesen, statt gleich eine neue
            // Anmeldung zu verlangen
            const auto delay = keychainRetryDelay(_keychainReadFailures);
            qCWarning(lcHttpCredentials) << "Could not read the password from the keychain, retrying in" << delay << ":" << error;
            QTimer::singleShot(delay, this, [this] {
                if (!_ready) {
                    fetchPasswordFromKeychain();
                }
            });
            return;
        }
        _keychainReadFailures = 0;
        qCWarning(lcHttpCredentials) << "No password in the keychain for" << _account->url().toString()
                                     << (error.isEmpty() ? QStringLiteral("(no entry)") : error);
        _password.clear();
        _ready = false;
        Q_EMIT fetched();
    });
}

void HttpCredentials::readPassword(const ReadCallback &done)
{
    auto job = _account->credentialManager()->get(passwordKeyC());
    if (!job) {
        done(KeychainResult::NotFound, {}, QStringLiteral("get credentials job is null - most likely the key does not exist in the credentials manager"));
        return;
    }
    connect(job, &CredentialJob::finished, this, [job, done] {
        if (job->error() == QKeychain::NoError) {
            done(KeychainResult::Ok, job->data().toString(), {});
        } else {
            done(job->error() == QKeychain::EntryNotFound ? KeychainResult::NotFound : KeychainResult::Error, {}, job->errorString());
        }
    });
}

void HttpCredentials::storePassword(const QString &password, const StoreCallback &done)
{
    auto job = _account->credentialManager()->set(passwordKeyC(), password);
    // CredentialManager::set startet den Auftrag verzögert: die Verbindung kommt rechtzeitig
    connect(job, &QKeychain::Job::finished, this, [job, done] { done(job->error() == QKeychain::NoError, job->errorString()); });
}

void HttpCredentials::removePassword()
{
    // derselbe Bereich wie in 6.0.3 („http“): auch ein dort gespeichertes Kennwort verschwindet
    _account->credentialManager()->clear(QStringLiteral("http"));
}

std::chrono::milliseconds HttpCredentials::keychainRetryDelay(int failedAttempts) const
{
    // 1 s, 2 s, 4 s, … höchstens 5 min
    return std::min<milliseconds>(1s * (1 << std::clamp(failedAttempts - 1, 0, 9)), 5min);
}

bool HttpCredentials::stillValid(QNetworkReply *reply)
{
    return reply->error() != QNetworkReply::AuthenticationRequiredError
        // QNAM bricht die Anfrage ab, wenn slotAuthentication() keine Zugangsdaten nachreicht
        && (reply->error() != QNetworkReply::OperationCanceledError || !reply->property(authenticationFailedC).toBool());
}

void HttpCredentials::slotAuthentication(QNetworkReply *reply, QAuthenticator *authenticator)
{
    Q_UNUSED(authenticator)
    if (!_ready) {
        return;
    }
    // Der Authorization-Kopf geht mit jeder Anfrage mit. Fragt QNAM trotzdem nach Zugangsdaten, hat der Server sie
    // abgelehnt: nichts nachreichen, die Anfrage scheitert, stillValid() meldet es dem Konto.
    qCWarning(lcHttpCredentials) << "Stop request: Authentication failed for" << reply->url().toString() << reply->request().rawHeader("Original-Request-ID");
    reply->setProperty(authenticationFailedC, true);
}

void HttpCredentials::acceptPassword(const QString &password)
{
    // sonst läse checkConnectivity() erst den Schlüsselbund, während das Kennwort noch geschrieben wird
    _wasEverFetched = true;
    _password = password;
    _ready = !_loginUser.isEmpty() && !_password.isEmpty();
    persist();
    Q_EMIT fetched();
}

void HttpCredentials::replacePassword(const QString &password)
{
    cancelAskFromUser();
    acceptPassword(password);
}

void HttpCredentials::persist()
{
    if (_loginUser.isEmpty()) {
        // nie angemeldet: nichts zu speichern
        return;
    }
    writePassword();
}

void HttpCredentials::writePassword()
{
    if (!_account || _storeRunning || _password.isEmpty() || _password == _storedPassword) {
        return;
    }
    _storeRunning = true;
    const QString password = _password;
    storePassword(password, [this, password](bool ok, const QString &error) {
        _storeRunning = false;
        if (ok) {
            _storedPassword = password;
            _storeFailures = 0;
            setKeychainError({});
        } else {
            ++_storeFailures;
            qCWarning(lcHttpCredentials) << "Could not store the password in the keychain (attempt" << _storeFailures << "):" << error;
            if (_storeFailures >= storeAttemptsBeforeWarning && !_password.isEmpty()) {
                // dauerhaft: Nach einem Neustart verlangt der Client eine neue Anmeldung. Das soll der Nutzer vorher sehen.
                setKeychainError(error);
            }
        }
        if (_password.isEmpty()) {
            // inzwischen abgemeldet: nichts im Schlüsselbund zurücklassen
            if (_account) {
                removePassword();
            }
            _storedPassword.clear();
            return;
        }
        if (_password != _storedPassword) {
            if (ok) {
                // inzwischen kam ein neueres
                writePassword();
            } else {
                QTimer::singleShot(keychainRetryDelay(_storeFailures), this, &HttpCredentials::writePassword);
            }
        }
    });
}

void HttpCredentials::setKeychainError(const QString &error)
{
    if (_keychainError == error) {
        return;
    }
    _keychainError = error;
    Q_EMIT keychainErrorChanged(error);
}

void HttpCredentials::invalidateToken()
{
    qCWarning(lcHttpCredentials) << "Invalidating the credentials";
    _password.clear();
    _ready = false;

    if (!_account) {
        return;
    }
    // die Sitzung des Servers (Cookie) gilt ohne Kennwort nicht weiter
    _account->clearCookieJar();
    // QNAM soll die abgelehnten Zugangsdaten vergessen; später in der Ereignisschleife, weil der Aufruf (mittelbar) aus
    // QNetworkAccessManagerPrivate::authenticationRequired kommen kann, einer blockierenden Verbindung aus dem HTTP-Thread
    QTimer::singleShot(0, _account, &Account::clearAMCache);
}

void HttpCredentials::forgetSensitiveData()
{
    cancelAskFromUser();
    invalidateToken();
    _storeFailures = 0;
    setKeychainError({});
    if (_account && !_storeRunning) {
        removePassword();
    }
    // läuft noch ein Speichern, löscht dessen Abschluss den Eintrag (writePassword())
    _storedPassword.clear();
}

} // namespace OCC

#include "httpcredentials.moc"
