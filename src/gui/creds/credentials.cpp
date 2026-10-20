/*
 * Copyright (C) by Lisa Reese <lisa.reese@kiteworks.com>
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
#include "credentials.h"

#include "abstractnetworkjob.h"
#include "accessmanager.h"
#include "account.h"
#include "config/appconfig.h"
#include "creds/credentialmanager.h"
#include "gui/networkinformation.h"
#include "oauth.h"
#include "requestauthenticationcontroller.h"
#include "requestauthenticationwidget.h"

#include <QAuthenticator>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkInformation>
#include <QNetworkReply>
#include <QRandomGenerator>

#include <algorithm>
#include <chrono>

using namespace std::chrono;
using namespace std::chrono_literals;

Q_LOGGING_CATEGORY(lcCredentials, "gui.credentials", QtInfoMsg)

namespace {
const char authenticationFailedC[] = "owncloud-authentication-failed";

// ein eben ausgestelltes Access-Token, das der Server gleich ablehnt (etwa ein Webserver, der den Authorization-Header
// nicht weitergibt): nicht in einer Schleife erneuern, sondern wie einen Fehlschlag mit Pause behandeln
constexpr auto freshAccessTokenGrace = 30s;
// wieder im Netz: der nächste Versuch nach dieser Pause statt nach der vollen
constexpr auto networkBackRetryDelay = 2s;
// so lange wartet das neue Access-Token höchstens auf den Schlüsselbund
constexpr auto pendingTokensTimeout = 10s;
// nach so vielen gescheiterten Versuchen, das Refresh-Token zu speichern, gilt das neue Access-Token trotzdem
constexpr int storeAttemptsBeforeUse = 3;
// Lesen beim Start: so oft, wenn der Schlüsselbund noch nicht bereit ist (Autostart, gesperrt), dann neu anmelden
constexpr int keychainReadAttempts = 6;
// beim Beenden: so lange auf das Speichern des Refresh-Tokens warten
constexpr auto shutdownStoreTimeout = 5s;

// when creating new keys we should probably stick with this idea related to a creds "type" in case we need to add or remove
// other cred types in future. all of the updates to transition to oauth only would have been so much easier if the last round had actually implmented
// separate http and oauth creds instead of rolling all of it into the httpCreds "type".
QString refreshTokenKeyC()
{
    return QString("http/oauthtoken");
}

QByteArray bearer(const QString &accessToken)
{
    return QByteArrayLiteral("Bearer ") + accessToken.toUtf8();
}
}

namespace OCC {

class CredentialsAccessManager : public AccessManager
{
    Q_OBJECT
public:
    // this looks like trouble with the default nullptr on parent
    CredentialsAccessManager(const Credentials *cred, QObject *parent = nullptr)
        : AccessManager(parent)
        , _cred(cred)
    {
    }

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData) override
    {
        QNetworkRequest req(request);
        if (!req.attribute(DontAddCredentialsAttribute).toBool()) {
            if (_cred && !_cred->_accessToken.isEmpty()) {
                req.setRawHeader("Authorization", bearer(_cred->_accessToken));
            }
        }
        return AccessManager::createRequest(op, req, outgoingData);
    }

private:
    // The credentials object dies along with the account, while the QNAM might
    // outlive both.
    // Lisa todo: that statement is very concerning. investigate -> In fact the nam is parented by the creds by default so no, it
    // should not outlive the creds unless there are shenanigans wrt reparenting it.
    QPointer<const Credentials> _cred;
};

Credentials::Credentials(const QString &token, const QString &refreshToken, Account *account)
    : AbstractCredentials(account, account)
    , _accessToken(token)
    , _refreshToken(refreshToken)
    , _ready(false)
    , _openIdConfig(AppConfig().openIdConfig())
{
    if (!token.isEmpty() && !refreshToken.isEmpty()) {
        _ready = true;
        _sinceAccessToken.start();
    }

    _refreshRetryTimer.setSingleShot(true);
    connect(&_refreshRetryTimer, &QTimer::timeout, this, &Credentials::refreshAccessTokenInternal);

    _proactiveRefreshTimer.setSingleShot(true);
    _proactiveRefreshTimer.setTimerType(Qt::VeryCoarseTimer);
    connect(&_proactiveRefreshTimer, &QTimer::timeout, this, [this] {
        if (!refreshPending()) {
            qCInfo(lcCredentials) << "Refreshing the access token before it expires";
            refreshAccessTokenInternal();
        }
    });

    _pendingTokensTimer.setSingleShot(true);
    connect(&_pendingTokensTimer, &QTimer::timeout, this, [this] {
        qCWarning(lcCredentials) << "The keychain has not confirmed the new refresh token yet, using the new access token anyway";
        commitPendingTokens();
    });

    _storeRetryTimer.setSingleShot(true);
    connect(&_storeRetryTimer, &QTimer::timeout, this, &Credentials::writeRefreshToken);

    // wieder im Netz: nicht die volle Pause abwarten. Das ist nur der Übergang aus Disconnected; ein Wechsel zwischen
    // erreichbaren Zuständen (Windows bewertet die Verbindung neu: Online, Site, Local, Unknown) ist kein Anlass.
    auto *network = NetworkInformation::instance();
    if (const auto *qni = QNetworkInformation::instance()) {
        _reachability = qni->reachability();
    }
    connect(network, &NetworkInformation::reachabilityChanged, this, [this](NetworkInformation::Reachability reachability) {
        const bool back = _reachability == NetworkInformation::Reachability::Disconnected && reachability != NetworkInformation::Reachability::Disconnected;
        _reachability = reachability;
        if (back && _refreshRetryTimer.isActive() && networkAvailable()) {
            qCInfo(lcCredentials) << "Network is back, retrying the token refresh soon";
            bringRetryForward(networkBackRetryDelay);
        }
    });

    if (auto *app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, &Credentials::waitForStoredRefreshToken);
    }
}

Credentials::Credentials(Account *account)
    : Credentials({}, {}, account)
{
}

AccessManager *Credentials::createAccessManager() const
{
    AccessManager *am = new CredentialsAccessManager(this);

    connect(am, &QNetworkAccessManager::authenticationRequired, this, &Credentials::slotAuthentication);

    return am;
}

bool Credentials::ready() const
{
    // nach der Uhr des Rechners: sie läuft auch im Ruhezustand weiter, Zeitgeber nicht auf jedem System
    return _ready && (_accessTokenStaleAt == 0 || QDateTime::currentSecsSinceEpoch() < _accessTokenStaleAt);
}

void Credentials::fetchFromKeychain()
{
    _wasEverFetched = true;
    _fetchRequested = true;

    if (ready()) {
        emitFetched();
        return;
    }

    if (!_refreshToken.isEmpty()) {
        // This happens if the credentials are still loaded from the keychain, bur we are called
        // here because the auth is invalid, so this means we simply need to refresh the credentials
        blockJobs();
        if (!refreshPending()) {
            refreshAccessTokenInternal();
        }
        return;
    }

    fetchCredentialsFromKeychain();
}

void Credentials::handleKeychainError(const QString &message)
{
    qCWarning(lcCredentials) << message;

    _fetchErrorString = message;
    _accessToken.clear();
    _ready = false;
    emitFetched();
}

void Credentials::fetchCredentialsFromKeychain()
{
    if (!_account) {
        return;
    }
    // erst das Token, dann Anfragen
    blockJobs();
    readRefreshToken([this](KeychainResult result, const QString &refreshToken, const QString &error) {
        if (result == KeychainResult::Ok && !refreshToken.isEmpty()) {
            _keychainReadFailures = 0;
            _refreshToken = refreshToken;
            _storedRefreshToken = refreshToken;
            if (!refreshPending()) {
                refreshAccessTokenInternal();
            }
            return;
        }
        if (result == KeychainResult::Error && ++_keychainReadFailures < keychainReadAttempts) {
            // Der Schlüsselbund ist noch nicht bereit (Autostart, gesperrt): später erneut lesen, statt gleich eine neue
            // Anmeldung zu verlangen
            const auto delay = keychainRetryDelay(_keychainReadFailures);
            qCWarning(lcCredentials) << "Could not read the refresh token from the keychain, retrying in" << delay << ":" << error;
            QTimer::singleShot(delay, this, [this] {
                if (_refreshToken.isEmpty() && !_ready) {
                    fetchCredentialsFromKeychain();
                }
            });
            return;
        }
        _keychainReadFailures = 0;
        handleKeychainError(result == KeychainResult::Ok ? QStringLiteral("get credentials data is empty") : error);
    });
}

void Credentials::readRefreshToken(const ReadCallback &done)
{
    auto job = _account->credentialManager()->get(refreshTokenKeyC());
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

void Credentials::storeRefreshToken(const QString &refreshToken, const StoreCallback &done)
{
    auto job = _account->credentialManager()->set(refreshTokenKeyC(), refreshToken);
    // CredentialManager::set startet den Auftrag verzögert: die Verbindung kommt rechtzeitig
    connect(job, &QKeychain::Job::finished, this, [job, done] { done(job->error() == QKeychain::NoError, job->errorString()); });
}

void Credentials::removeRefreshToken()
{
    // i hoped we could change this to a new, more useful identifier but since the value is related to what is in the keychain, I don't
    // think this can be changed easily so I'm leaving it for now.
    _account->credentialManager()->clear("http");
}

std::chrono::milliseconds Credentials::defaultRefreshRetryDelay(int failedAttempts)
{
    const int exponent = std::clamp(failedAttempts - 1, 0, 4);
    return std::min<milliseconds>(30s * (1 << exponent), 5min);
}

std::chrono::milliseconds Credentials::refreshRetryDelay(int failedAttempts) const
{
    // ±10 %: nach einem Ausfall des Servers kommen nicht alle Clients im selben Augenblick wieder
    const auto delay = defaultRefreshRetryDelay(failedAttempts);
    const double factor = 0.9 + QRandomGenerator::global()->bounded(0.2);
    return milliseconds(static_cast<qint64>(static_cast<double>(delay.count()) * factor));
}

std::chrono::milliseconds Credentials::keychainRetryDelay(int failedAttempts) const
{
    // 1 s, 2 s, 4 s, … höchstens 5 min
    return std::min<milliseconds>(1s * (1 << std::clamp(failedAttempts - 1, 0, 9)), 5min);
}

std::chrono::milliseconds Credentials::explicitRetryInterval() const
{
    return 10s;
}

// Lisa todo: this is called periodically, find the trigger. it's coming mostly from the jobs :(
bool Credentials::stillValid(QNetworkReply *reply)
{
    // The function is called in order to determine whether we need to ask the user for a password
    // since we are using OAuth, we already started a refresh in slotAuthentication, at least in theory, ensure the auth is started.
    // If the refresh fails, we are going to Q_EMIT authenticationFailed ourselves
    if (reply->error() == QNetworkReply::AuthenticationRequiredError) {
        slotAuthentication(reply, nullptr);
    } else if (_ready && !_accessTokenWorked && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 0
        && reply->request().rawHeader("Authorization") == bearer(_accessToken)) {
        // der Server hat das aktuelle Access-Token angenommen
        _accessTokenWorked = true;
    }
    return true;
}

void Credentials::slotAuthentication(QNetworkReply *reply, QAuthenticator *authenticator)
{
    Q_UNUSED(authenticator)
    // QNAM meldet eine Ablehnung über authenticationRequired und die Aufträge noch einmal über stillValid(): einmal reicht
    if (reply->property(authenticationFailedC).toBool()) {
        return;
    }
    // Because of issue #4326, we need to set the login and password manually at every requests
    // Thus, if we reach this signal, those credentials were invalid and we terminate.
    qCWarning(lcCredentials) << "Stop request: Authentication failed for " << reply->url().toString() << reply->request().rawHeader("Original-Request-ID");
    reply->setProperty(authenticationFailedC, true);

    if (_refreshToken.isEmpty()) {
        // nichts zu erneuern: das Konto fragt den Nutzer
        return;
    }
    if (_ready && reply->request().rawHeader("Authorization") != bearer(_accessToken)) {
        // die Anfrage trug ein älteres Token; ihre Wiederholung bekommt das aktuelle
        return;
    }

    // Das aktuelle Access-Token gilt nicht mehr: alle Aufträge warten auf das neue, abgelehnte stehen mit in der Warteschlange
    _ready = false;
    blockJobs();
    if (refreshPending()) {
        return;
    }
    if (!_accessTokenWorked && _sinceAccessToken.isValid() && _sinceAccessToken.durationElapsed() < freshAccessTokenGrace) {
        handleRefreshError(networkReplyErrorString(*reply), false, {});
        return;
    }
    qCInfo(lcCredentials) << "Refreshing token";
    refreshAccessTokenInternal();
}

bool Credentials::refreshAccessToken()
{
    if (_refreshToken.isEmpty())
        return false;

    _fetchRequested = true;
    const bool jobsWereBlocked = _jobsBlocked;
    if (!ready()) {
        blockJobs();
    }
    if (_refreshRetryTimer.isActive()) {
        // Pause nach einem Fehlschlag. Die Kontoprüfung, die hierher führt, läuft regelmäßig und meldet nur, dass status.php
        // antwortet; das tat der Server womöglich auch beim letzten Versuch (Konto deaktiviert, oauth2-App aus). Sie verkürzt
        // die Pause deshalb nicht, das tun nur echte Anlässe (retryRefreshSoon(), wieder im Netz). Das Konto erfährt aber
        // erneut, warum es wartet; blockJobs() hat es eben schon gesagt, wenn es die Aufträge erst jetzt anhielt.
        if (jobsWereBlocked || !_jobsBlocked) {
            Q_EMIT authenticationDelayed(_refreshMaintenance, _refreshErrorString);
        }
        return true;
    }
    if (!refreshPending()) {
        refreshAccessTokenInternal();
    }
    return true;
}

void Credentials::retryRefreshSoon()
{
    if (_refreshRetryTimer.isActive()) {
        qCInfo(lcCredentials) << "The server answers again or a certificate was accepted, retrying the token refresh soon";
        bringRetryForward(0ms);
    }
}

void Credentials::bringRetryForward(std::chrono::milliseconds delay)
{
    // auch bei wiederholtem Anlass (Netz flattert, Server mal da, mal nicht) frühestens explicitRetryInterval() nach dem
    // letzten Versuch
    const auto sinceAttempt = _sinceRefreshAttempt.isValid() ? _sinceRefreshAttempt.durationElapsed() : nanoseconds::max();
    const auto spacing = sinceAttempt >= explicitRetryInterval() ? 0ms : duration_cast<milliseconds>(explicitRetryInterval() - sinceAttempt);
    const auto wait = std::max(delay, spacing);
    if (_refreshRetryTimer.isActive() && _refreshRetryTimer.remainingTimeAsDuration() > wait) {
        _refreshRetryTimer.start(wait);
    }
}

void Credentials::setAccessTokenExpiresAt(qint64 expiresAt)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (expiresAt <= 0 || expiresAt > now) {
        setAccessTokenLifetime(expiresAt > 0 ? expiresAt - now : 0);
        return;
    }
    // schon abgelaufen: vor der nächsten Anfrage erneuern (refreshIfExpired()), nicht erst nach einer Ablehnung
    _proactiveRefreshTimer.stop();
    _accessTokenStaleAt = now;
    _proactiveRefreshAt = now;
}

void Credentials::refreshIfExpired()
{
    if (!_ready || _refreshToken.isEmpty()) {
        return;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (_accessTokenStaleAt > 0 && now >= _accessTokenStaleAt) {
        // nach der Uhr abgelaufen, etwa nach dem Ruhezustand: erst erneuern, dann senden
        qCInfo(lcCredentials) << "The access token has expired, refreshing it before sending further requests";
        _ready = false;
        blockJobs();
        if (!refreshPending()) {
            refreshAccessTokenInternal();
        }
    } else if (_proactiveRefreshAt > 0 && now >= _proactiveRefreshAt && !refreshPending()) {
        // der Zeitgeber kam zu spät (Ruhezustand): vorbeugend erneuern, die Aufträge laufen mit dem gültigen Token weiter
        qCInfo(lcCredentials) << "Refreshing the access token before it expires";
        refreshAccessTokenInternal();
    }
}

bool Credentials::refreshPending() const
{
    return _oAuthJob || _pendingTokens || _refreshRetryTimer.isActive();
}

void Credentials::blockJobs()
{
    if (_jobsBlocked) {
        return;
    }
    _jobsBlocked = true;
    Q_EMIT authenticationStarted();
    if (_refreshRetryTimer.isActive()) {
        // eine Pause nach einem Fehlschlag läuft schon: das Konto erfährt, warum nichts mehr geht
        Q_EMIT authenticationDelayed(_refreshMaintenance, _refreshErrorString);
    }
}

void Credentials::emitFetched()
{
    _jobsBlocked = false;
    _fetchRequested = false;
    Q_EMIT fetched();
}

void Credentials::refreshAccessTokenInternal()
{
    if (!_account || _refreshToken.isEmpty() || _oAuthJob || _pendingTokens)
        return;

    _refreshRetryTimer.stop();
    _proactiveRefreshTimer.stop();
    _sinceRefreshAttempt.start();

    // parent with nam to ensure we reset when the nam is reset
    // todo: #22 - the parenting here is highly questionable, as is the use of the shared account ptr
    auto *job = new AccountBasedOAuth(_account, _openIdConfig, this);
    _oAuthJob = job;
    // nur das Ergebnis der laufenden Erneuerung zählt; eine abgebrochene meldet nichts mehr
    const auto finishJob = [this, job] {
        if (job != _oAuthJob) {
            return false;
        }
        _oAuthJob = nullptr;
        job->deleteLater();
        return true;
    };
    connect(job, &AccountBasedOAuth::refreshFinished, this, [this, finishJob](const QString &accessToken, const QString &refreshToken, qint64 expiresIn) {
        if (finishJob()) {
            handleRefreshSuccess(accessToken, refreshToken, expiresIn);
        }
    });
    connect(job, &AccountBasedOAuth::refreshRejected, this, [this, finishJob](const QString &message) {
        if (finishJob()) {
            handleRefreshRejected(message);
        }
    });
    connect(job, &AccountBasedOAuth::refreshFailed, this, [this, finishJob](const QString &message, bool maintenance, const QString &newRefreshToken) {
        if (finishJob()) {
            handleRefreshError(message, maintenance, newRefreshToken);
        }
    });

    job->refreshAuthentication(_refreshToken);
}

void Credentials::handleRefreshError(const QString &message, bool maintenance, const QString &newRefreshToken)
{
    if (!newRefreshToken.isEmpty() && newRefreshToken != _refreshToken) {
        // der Server hat trotzdem schon gewechselt: mit dem neuen weiter
        _refreshToken = newRefreshToken;
        writeRefreshToken();
    }
    ++_tokenRefreshRetriesCount;
    _refreshErrorString = message;
    _refreshMaintenance = maintenance;

    // ohne Netz ist jeder Versuch vergebens: dann meldet NetworkInformation die Rückkehr
    const auto delay = networkAvailable() ? refreshRetryDelay(_tokenRefreshRetriesCount) : std::max<milliseconds>(refreshRetryDelay(_tokenRefreshRetriesCount), 5min);
    qCWarning(lcCredentials) << "Refreshing the access token failed temporarily (" << _tokenRefreshRetriesCount << "in a row), retrying in" << delay << ":"
                             << (maintenance ? QStringLiteral("server in maintenance mode") : message);
    _refreshRetryTimer.start(delay);

    if (_jobsBlocked || _fetchRequested || !ready()) {
        // ohne gültiges Token stehen die Aufträge: dem Konto sagen, warum, statt eine neue Anmeldung zu verlangen
        Q_EMIT authenticationDelayed(maintenance, message);
    }
}

void Credentials::handleRefreshRejected(const QString &message)
{
    qCWarning(lcCredentials) << "The server no longer accepts the refresh token, a new sign-in is required:" << message;
    _fetchErrorString = message;
    finishFailedRefresh();
}

void Credentials::handleRefreshSuccess(const QString &accessToken, const QString &refreshToken, qint64 expiresIn)
{
    _tokenRefreshRetriesCount = 0;
    _refreshErrorString.clear();
    _refreshMaintenance = false;
    // RFC 6749, Abschnitt 6: ohne neues Refresh-Token gilt das bisherige weiter
    if (!refreshToken.isEmpty()) {
        _refreshToken = refreshToken;
    }
    _pendingTokens = PendingTokens{accessToken, expiresIn};
    if (_refreshToken == _storedRefreshToken) {
        commitPendingTokens();
        return;
    }
    // Erst in den Schlüsselbund, dann benutzen: Endet das Programm jetzt, liegt dort schon das gültige Refresh-Token. Den
    // alten hat der Server mit dem Tausch verworfen.
    _pendingTokensTimer.start(pendingTokensTimeout);
    writeRefreshToken();
}

void Credentials::commitPendingTokens()
{
    if (!_pendingTokens) {
        return;
    }
    const PendingTokens tokens = *_pendingTokens;
    _pendingTokens.reset();
    _pendingTokensTimer.stop();

    _accessToken = tokens.accessToken;
    _ready = true;
    _accessTokenWorked = false;
    _sinceAccessToken.start();
    setAccessTokenLifetime(tokens.expiresIn);
    qCInfo(lcCredentials) << "Access token refreshed" << (tokens.expiresIn > 0 ? QStringLiteral("valid for %1 s").arg(tokens.expiresIn) : QString());

    if (_fetchRequested || _jobsBlocked) {
        emitFetched();
    }
}

void Credentials::setAccessTokenLifetime(qint64 expiresIn)
{
    _proactiveRefreshTimer.stop();
    if (expiresIn <= 0) {
        // unbekannt: erneuert wird nach einer Ablehnung (401)
        _accessTokenStaleAt = 0;
        _proactiveRefreshAt = 0;
        return;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    // kurz vor dem Ablauf gilt es als abgelaufen (Laufzeit der Anfrage), bei kurzer Lebensdauer entsprechend früher
    _accessTokenStaleAt = now + expiresIn - std::min<qint64>(60, expiresIn / 10);
    const qint64 proactive = std::max<qint64>(1, expiresIn * 4 / 5);
    _proactiveRefreshAt = now + proactive;
    _proactiveRefreshTimer.start(seconds(proactive));
}

void Credentials::writeRefreshToken()
{
    _storeRetryTimer.stop();
    if (!_account || _storeRunning || _refreshToken.isEmpty() || _refreshToken == _storedRefreshToken) {
        return;
    }
    _storeRunning = true;
    const QString token = _refreshToken;
    storeRefreshToken(token, [this, token](bool ok, const QString &error) {
        _storeRunning = false;
        if (ok) {
            _storedRefreshToken = token;
            _storeFailures = 0;
            setKeychainError({});
            if (_refreshToken.isEmpty()) {
                // inzwischen abgemeldet: nichts im Schlüsselbund zurücklassen
                removeRefreshToken();
                _storedRefreshToken.clear();
            }
        } else {
            ++_storeFailures;
            qCWarning(lcCredentials) << "Could not store the refresh token in the keychain (attempt" << _storeFailures << "):" << error;
            if (_storeFailures >= storeAttemptsBeforeUse && !_refreshToken.isEmpty()) {
                // dauerhaft: Endet das Programm, bevor das Speichern doch noch klappt, verlangt der nächste Start eine neue
                // Anmeldung. Das soll der Nutzer vorher sehen, nicht erst im Log.
                setKeychainError(error);
            }
        }

        if (_pendingTokens && (_refreshToken == _storedRefreshToken || _storeFailures >= storeAttemptsBeforeUse)) {
            if (_refreshToken != _storedRefreshToken) {
                qCWarning(lcCredentials) << "Using the new access token although the keychain did not store the refresh token; storing it is retried";
            }
            commitPendingTokens();
        }

        if (!_refreshToken.isEmpty() && _refreshToken != _storedRefreshToken) {
            if (ok) {
                // inzwischen kam ein neueres
                writeRefreshToken();
            } else {
                _storeRetryTimer.start(keychainRetryDelay(_storeFailures));
            }
        }
        if (_storeWaitLoop) {
            _storeWaitLoop->quit();
        }
    });
}

void Credentials::setKeychainError(const QString &error)
{
    if (_keychainError == error) {
        return;
    }
    _keychainError = error;
    Q_EMIT keychainErrorChanged(error);
}

void Credentials::waitForStoredRefreshToken()
{
    if (_refreshToken.isEmpty() || _refreshToken == _storedRefreshToken) {
        return;
    }
    // Das Programm endet: ein noch nicht gespeichertes Refresh-Token wäre beim nächsten Start verloren. Während des Wartens
    // keine neue Erneuerung mehr beginnen.
    _proactiveRefreshTimer.stop();
    _refreshRetryTimer.stop();
    writeRefreshToken();
    if (!_storeRunning) {
        return;
    }
    qCInfo(lcCredentials) << "Waiting for the keychain to store the refresh token before quitting";
    QEventLoop loop;
    _storeWaitLoop = &loop;
    QTimer::singleShot(shutdownStoreTimeout, &loop, &QEventLoop::quit);
    loop.exec();
    _storeWaitLoop = nullptr;
}

void Credentials::finishFailedRefresh()
{
    forgetSensitiveData();
    // das Konto prüft die Verbindung und fragt dann den Nutzer (askFromUser())
    emitFetched();
}

bool Credentials::networkAvailable()
{
    auto qni = QNetworkInformation::instance();
    if (!qni)
        return true; // this can allegedly happen on Linux
    // nur ein sicher fehlendes Netz zählt: Unknown melden manche Systeme dauerhaft, dann käme auch keine Rückkehr ins Netz
    if (qni->reachability() == QNetworkInformation::Reachability::Disconnected)
        return false;
    if (qni->isBehindCaptivePortal())
        return false;
    return true;
}

void Credentials::askFromUser()
{
    // I think this can happen when the re-auth process has already quasi started and is waiting for user input, but
    // we get a prompt to log in again. I am not quite sure as it's very hard to reproduce.
    if (_requestAuth || !_account) {
        // let the existing instance ride
        return;
    }

    _requestAuth = new RequestAuthenticationController(this);
    connect(_requestAuth, &RequestAuthenticationController::requestAccountModal, this, &AbstractCredentials::requestAccountModal);
    // we should not connect to the failed signal as we are forcing the user to make the decision using the gui.
    // if they log in successfully, we get the success signal.
    // if the auth fails the gui stays until the user gets it right or clicks the stay logged out button.
    connect(_requestAuth, &RequestAuthenticationController::authenticationSucceeded, this, &Credentials::askFromUserSucceeded);
    connect(_requestAuth, &RequestAuthenticationController::requestLogout, this, &Credentials::askFromUserLogout);
    _requestAuth->startAuthentication(_account);
}

void Credentials::askFromUserSucceeded(const QString &token, const QString &refreshToken, qint64 expiresIn)
{
    // eine Erneuerung oder Pause von vorher zählt nicht mehr
    stopRefresh();
    _accessToken.clear();
    _ready = false;
    _refreshToken = refreshToken;
    // wie nach einer Erneuerung: erst das Refresh-Token in den Schlüsselbund, dann benutzen; mit der Lebensdauer aus dem
    // Code-Tausch wird vorbeugend erneuert, ohne sie erst nach einer Ablehnung (401)
    _fetchRequested = true;
    handleRefreshSuccess(token, refreshToken, expiresIn);

    if (_requestAuth) {
        delete _requestAuth;
        _requestAuth = nullptr;
    }
}

void Credentials::replaceTokens(const QString &accessToken, const QString &refreshToken, qint64 accessTokenExpiresAt)
{
    _fetchErrorString.clear();
    // sonst läse checkConnectivity() erst den Schlüsselbund, während der neue Token noch geschrieben wird
    _wasEverFetched = true;
    // restliche Lebensdauer; schon abgelaufen: bald erneuern (die Lebensdauer 0 hieße „unbekannt“)
    const qint64 expiresIn = accessTokenExpiresAt > 0 ? std::max<qint64>(1, accessTokenExpiresAt - QDateTime::currentSecsSinceEpoch()) : 0;
    // askFromUserSucceeded() verwirft über stopRefresh() eine Erneuerung gegen die alte Adresse samt Pausen: ihr Ergebnis
    // darf die neuen Token nicht überschreiben
    askFromUserSucceeded(accessToken, refreshToken, expiresIn);
}

void Credentials::askFromUserLogout()
{
    Q_EMIT requestLogout();
    if (_requestAuth) {
        delete _requestAuth;
        _requestAuth = nullptr;
    }
}

void Credentials::stopRefresh()
{
    if (_oAuthJob) {
        _oAuthJob->disconnect(this);
        _oAuthJob->deleteLater();
        _oAuthJob = nullptr;
    }
    _refreshRetryTimer.stop();
    _proactiveRefreshTimer.stop();
    _pendingTokensTimer.stop();
    _pendingTokens.reset();
    _tokenRefreshRetriesCount = 0;
    _refreshErrorString.clear();
    _refreshMaintenance = false;
}

void Credentials::invalidateToken()
{
    qCWarning(lcCredentials) << "Invalidating the credentials";

    _accessToken.clear();
    _ready = false;
    setAccessTokenLifetime(0);

    if (!_account) {
        return;
    }

    // clear the session cookie.
    _account->clearCookieJar();

    if (!_refreshToken.isEmpty()) {
        // Only invalidate the access_token but keep the _refreshToken in the keychain
        // (when coming from forgetSensitiveData, the _refreshToken is cleared)
        return;
    }

    removeRefreshToken();
    _storedRefreshToken.clear();

    // let QNAM forget about the previous credentials it may have been using
    // This needs to be done later in the event loop because we might be called (directly or
    // indirectly) from QNetworkAccessManagerPrivate::authenticationRequired, which itself
    // is a called from a BlockingQueuedConnection from the Qt HTTP thread. And clearing the
    // cache needs to synchronize again with the HTTP thread.
    QTimer::singleShot(0, _account, &Account::clearAMCache);
}

void Credentials::forgetSensitiveData()
{
    stopRefresh();
    // need to be done before invalidateToken, so it actually deletes the refresh_token from the keychain
    // to do, make that more explicit
    _refreshToken.clear();
    // ein noch nicht gespeichertes Refresh-Token zählt nicht mehr, der Hinweis darauf auch nicht
    _storeRetryTimer.stop();
    _storeFailures = 0;
    setKeychainError({});
    invalidateToken();
    _fetchRequested = false;
    if (_jobsBlocked) {
        // angehaltene Aufträge bekommen kein Token mehr
        _jobsBlocked = false;
        Q_EMIT authenticationFailed();
    }
}

void Credentials::persist()
{
    // write secrets to the keychain
    writeRefreshToken();
}

} // namespace OCC


#include "credentials.moc"
