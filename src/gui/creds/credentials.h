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

#pragma once

#include "creds/abstractcredentials.h"
#include "creds/credentialssupport.h"
#include "gui/owncloudguilib.h"
#include "oauth.h"

#include <QElapsedTimer>
#include <QMap>
#include <QNetworkInformation>
#include <QSslCertificate>
#include <QSslKey>
#include <QNetworkRequest>
#include <QTimer>

#include <chrono>
#include <functional>
#include <optional>

class QEventLoop;
class QNetworkReply;
class QAuthenticator;

namespace OCC {
class OAuth;
class RequestAuthenticationController;

/**
 * OAuth2-Anmeldung eines Kontos: das Access-Token geht mit jeder Anfrage mit, das Refresh-Token liegt im Schlüsselbund.
 *
 * Erneuert wird mit dem Refresh-Token, ohne den Nutzer zu fragen: vorbeugend nach 80 % der Lebensdauer (expires_in),
 * vor einer Anfrage, deren Token nach der Uhr schon abgelaufen ist (nach dem Ruhezustand), nach einer Ablehnung (401)
 * und auf Wunsch des Kontos (refreshAccessToken()). Es läuft immer höchstens eine Erneuerung. Taugt das aktuelle Token
 * nicht mehr, hält das Konto seine Aufträge an (authenticationStarted()) und schickt sie mit dem neuen erneut
 * (fetched()); eine vorbeugende Erneuerung hält nichts an.
 *
 * Scheitert eine Erneuerung vorübergehend (Netz, Zeitüberschreitung, Serverfehler, Wartung, unerwartete Antwort, ein
 * deaktiviertes Konto oder abgeschalteter Anmeldedienst), bleibt das Refresh-Token, und nach einer wachsenden Pause
 * (30 s bis 5 min) folgt der nächste Versuch (authenticationDelayed() mit der Ursache). Die regelmäßige Kontoprüfung
 * verkürzt die Pause nicht; nur echte Anlässe ziehen den nächsten Versuch vor, frühestens 10 s nach dem letzten: wieder
 * im Netz (nach Disconnected), der Server antwortet nach einem Ausfall oder einer Wartung wieder, ein Zertifikat ist bestätigt
 * (retryRefreshSoon()). Nur wenn der Server das Refresh-Token ablehnt (invalid_grant), muss sich der Nutzer neu anmelden
 * (fetched() ohne Token, das Konto fragt dann über askFromUser()).
 *
 * Ein neues Refresh-Token kommt zuerst in den Schlüsselbund, erst danach wird das neue Access-Token benutzt. Scheitert
 * das Speichern, bleibt es im Speicher und wird nach wenigen Versuchen trotzdem benutzt; das Speichern wird wiederholt,
 * beim Beenden des Programms ein letztes Mal abgewartet. Ab dann meldet keychainErrorChanged() den Fehler, damit das
 * Konto ihn anzeigt, bis das Speichern klappt.
 */
class OWNCLOUDGUI_EXPORT Credentials : public AbstractCredentials
{
    Q_OBJECT
    // ick
    friend class CredentialsAccessManager;

public:
    /// Don't add credentials if this is set on a QNetworkRequest

    explicit Credentials(const QString &token, const QString &refreshToken, Account *account);

    explicit Credentials(Account *account);
    // this access manager is monitored for authenticationRequired signal
    // it also becomes the access manager member in the account. Try to invert this so the account
    // creates it and connects the creds slot to the _am.
    // note this is used by the checkServerJobFactory where a new nam is "needed" for every job due to options to clear cookies
    // and other factors that mean we don't want to use the account's nam directly.
    AccessManager *createAccessManager() const override;

    /** Ein Access-Token liegt vor und ist nach der Uhr nicht abgelaufen. */
    bool ready() const override;

    // this is invoked by AccountState::checkConnectivity. Totally uncool.
    // todo: DC-112 at least rename this to fetchCredentials() or something - also in base class. the "keychain" naming is an
    // impl detail we don't need to expose
    void fetchFromKeychain() override;

    void askFromUser() override;
    bool stillValid(QNetworkReply *reply) override;
    void persist() override;

    void invalidateToken() override;
    void forgetSensitiveData() override;

    /* If we don't have a valid refresh token, return false.
     * otherwise:
     *   if the refresh routine is running, return true
     *   if it is not running, start the refresh and return true.
     * Das Konto bekommt in jedem Fall fetched() oder authenticationDelayed(). Wartet nach einem Fehlschlag gerade eine
     * Pause, bleibt es dabei, und authenticationDelayed() nennt erneut die Ursache; vorziehen lässt sie sich nur mit
     * retryRefreshSoon().
     */
    bool refreshAccessToken() override;

    void refreshIfExpired() override;

    /** Wartet nach einem Fehlschlag eine Pause, folgt der nächste Versuch bald (frühestens 10 s nach dem letzten). */
    void retryRefreshSoon() override;

    /**
     * Ablauf des Access-Tokens aus dem Konstruktor nach der Uhr des Rechners (Sekunden seit 1970, 0: unbekannt), etwa aus
     * dem Code-Tausch des Assistenten, nach dem noch weitere Seiten kamen. Danach wird wie nach einer Erneuerung
     * vorbeugend erneuert; ist der Zeitpunkt schon vorbei, vor der nächsten Anfrage.
     */
    void setAccessTokenExpiresAt(qint64 expiresAt);

    /**
     * Übernimmt Token einer Anmeldung, die außerhalb dieses Objekts lief (Serverumzug: Anmeldung an der neuen Adresse).
     * Bricht eine laufende Erneuerung gegen die alte Adresse samt Pausen ab und schließt eine offene Neuanmeldung, wie
     * nach einer erfolgreichen Anmeldung: Das Refresh-Token kommt zuerst unter der aktuellen Kontoadresse in den
     * Schlüsselbund, erst danach gilt das Access-Token (fetched()). Mit @p accessTokenExpiresAt (wie bei
     * setAccessTokenExpiresAt(), 0: unbekannt) wird danach vorbeugend erneuert wie nach jeder Anmeldung.
     */
    void replaceTokens(const QString &accessToken, const QString &refreshToken, qint64 accessTokenExpiresAt = 0);

    /** Pause vor dem n-ten Versuch nach vorübergehend gescheiterten Erneuerungen: 30 s, 60 s, 120 s, 240 s, dann 5 min. */
    static std::chrono::milliseconds defaultRefreshRetryDelay(int failedAttempts);

protected:
    enum class KeychainResult { Ok, NotFound, Error };
    using ReadCallback = std::function<void(KeychainResult result, const QString &refreshToken, const QString &error)>;
    using StoreCallback = std::function<void(bool ok, const QString &error)>;

    // Zugriffe auf den Schlüsselbund; die Tests ersetzen sie
    virtual void readRefreshToken(const ReadCallback &done);
    virtual void storeRefreshToken(const QString &refreshToken, const StoreCallback &done);
    virtual void removeRefreshToken();

    // Pausen vor dem nächsten Versuch nach n Fehlschlägen in Folge (Erneuern; Lesen und Schreiben im Schlüsselbund); die
    // Tests verkürzen sie
    virtual std::chrono::milliseconds refreshRetryDelay(int failedAttempts) const;
    virtual std::chrono::milliseconds keychainRetryDelay(int failedAttempts) const;
    // ein echter Anlass (retryRefreshSoon(), wieder im Netz) zieht den nächsten Versuch frühestens so lange nach dem letzten vor
    virtual std::chrono::milliseconds explicitRetryInterval() const;

    bool networkAvailable();

    void handleRefreshSuccess(const QString &accessToken, const QString &refreshToken, qint64 expiresIn);
    void handleRefreshRejected(const QString &message);
    void handleRefreshError(const QString &message, bool maintenance, const QString &newRefreshToken);
    void finishFailedRefresh();
    void handleKeychainError(const QString &message);

    void slotAuthentication(QNetworkReply *reply, QAuthenticator *authenticator);
    void fetchCredentialsFromKeychain();
    void refreshAccessTokenInternal();

    // expiresIn: Lebensdauer des Access-Tokens aus dem Code-Tausch in Sekunden, 0: unbekannt
    void askFromUserSucceeded(const QString &token, const QString &refreshToken, qint64 expiresIn);
    void askFromUserLogout();

    /** Läuft gerade eine Erneuerung, wartet sie auf den Schlüsselbund oder auf den nächsten Versuch? */
    bool refreshPending() const;
    /** Bricht eine Erneuerung samt Pausen ab; ihr Ergebnis zählt nicht mehr. */
    void stopRefresh();
    /** Wartet eine Pause, kommt der nächste Versuch nach delay, frühestens explicitRetryInterval() nach dem letzten. */
    void bringRetryForward(std::chrono::milliseconds delay);
    void setKeychainError(const QString &error);
    /** Das aktuelle Access-Token taugt nicht: die Aufträge des Kontos anhalten, bis fetched() oder authenticationFailed() folgt. */
    void blockJobs();
    void emitFetched();
    void setAccessTokenLifetime(qint64 expiresIn);
    void commitPendingTokens();
    void writeRefreshToken();
    void waitForStoredRefreshToken();

    QString _accessToken;
    QString _refreshToken;
    // das Refresh-Token, das der Schlüsselbund bestätigt hat
    QString _storedRefreshToken;

    // Ablauf des Access-Tokens nach der Uhr des Rechners (Sekunden seit 1970, 0: unbekannt): ab _accessTokenStaleAt gilt es
    // als abgelaufen, ab _proactiveRefreshAt wird es vorbeugend erneuert
    qint64 _accessTokenStaleAt = 0;
    qint64 _proactiveRefreshAt = 0;
    QElapsedTimer _sinceAccessToken;
    // der Server hat das aktuelle Access-Token schon einmal angenommen: eine spätere Ablehnung heißt abgelaufen oder
    // widerrufen, nicht „kommt beim Server nie an“
    bool _accessTokenWorked = false;

    QString _fetchErrorString;
    bool _ready = false;
    const OpenIdConfig _openIdConfig;
    // vorübergehend gescheiterte Erneuerungen in Folge
    int _tokenRefreshRetriesCount = 0;

    QPointer<AccountBasedOAuth> _oAuthJob;
    QTimer _refreshRetryTimer;
    QTimer _proactiveRefreshTimer;
    QElapsedTimer _sinceRefreshAttempt;
    // zuletzt gemeldete Erreichbarkeit: nur der Übergang aus Disconnected zieht den nächsten Versuch vor
    QNetworkInformation::Reachability _reachability = QNetworkInformation::Reachability::Unknown;
    QString _refreshErrorString;
    bool _refreshMaintenance = false;
    // authenticationStarted() ist gesendet, fetched() oder authenticationFailed() steht aus
    bool _jobsBlocked = false;
    // das Konto wartet auf fetched()
    bool _fetchRequested = false;

    // vom Server erhalten, warten darauf, dass der Schlüsselbund das neue Refresh-Token bestätigt
    struct PendingTokens
    {
        QString accessToken;
        qint64 expiresIn = 0;
    };
    std::optional<PendingTokens> _pendingTokens;
    QTimer _pendingTokensTimer;

    bool _storeRunning = false;
    int _storeFailures = 0;
    // nach wiederholten Fehlschlägen die Meldung des Schlüsselbunds, sonst leer (keychainErrorChanged())
    QString _keychainError;
    QTimer _storeRetryTimer;
    QEventLoop *_storeWaitLoop = nullptr;
    int _keychainReadFailures = 0;

    RequestAuthenticationController *_requestAuth = nullptr;
};


} // namespace OCC
