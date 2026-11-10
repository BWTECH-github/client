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

#pragma once

#include "creds/abstractcredentials.h"
#include "creds/credentialssupport.h"
#include "owncloudlib.h"

#include <QNetworkRequest>
#include <QPointer>
#include <QString>

#include <chrono>
#include <functional>

class QAuthenticator;
class QNetworkReply;

namespace OCC {

/**
 * Anmeldung mit Benutzername und Kennwort (HTTP Basic), wie sie der 6.0.3-Client für Server ohne die App „OAuth2“
 * bot: Der Authorization-Kopf geht mit jeder Anfrage mit, das Kennwort liegt im Schlüsselbund (Schlüssel „http/password“
 * wie in 6.0.3, damit ein Konto aus dieser Konfiguration sein Kennwort behält).
 *
 * Lehnt der Server das Kennwort ab (401), gilt die Anfrage als nicht mehr berechtigt (stillValid()); das Konto
 * verwirft das Kennwort im Speicher (invalidateToken()) und fragt den Nutzer (askFromUser(), in HttpCredentialsGui ein
 * Dialog). Der Schlüsselbund behält das Kennwort bis zum Abmelden (forgetSensitiveData()), damit ein vorübergehender
 * 401 (etwa ein LDAP-Aussetzer) es nicht löscht. Es gibt nichts zu erneuern: refreshAccessToken() ist immer false.
 * Nur die Ablehnung einer Anfrage mit dem aktuellen Kennwort zählt (sentWithCurrentPassword()): Eine Anfrage, die vor
 * dem Lesen des Schlüsselbunds (beim Start) oder mit einem älteren Kennwort ging, wiederholt ihr Auftrag mit dem aktuellen.
 *
 * Verlangt der Server für das Konto die Zwei-Faktor-Anmeldung, lehnt er jedes Kennwort ab, auch das richtige; an WebDAV
 * sagt er es (401 mit dem Hinweis „password login forbidden“). passwordLoginForbidden() merkt sich das bis zur nächsten
 * angenommenen Anfrage, damit der Dialog nicht „Kennwort falsch“ meldet, sondern die Auswege nennt: ein App-Passwort aus
 * der Web-Oberfläche, oder das Konto neu hinzufügen, dann meldet es sich im Browser an.
 *
 * Beim Start wird der Schlüsselbund wie bei OAuth2 mehrmals gelesen, wenn er noch nicht bereit ist (Autostart,
 * gesperrt), bevor eine neue Anmeldung verlangt wird. Ein Fehlschlag beim Speichern wird wiederholt und über
 * keychainErrorChanged() gemeldet.
 */
class OWNCLOUDSYNC_EXPORT HttpCredentials : public AbstractCredentials
{
    Q_OBJECT

public:
    /** @p password darf leer sein: dann ist das Konto erst nach fetchFromKeychain() oder einer Anmeldung bereit */
    HttpCredentials(Account *account, const QString &loginUser, const QString &password);

    AccessManager *createAccessManager() const override;
    AuthenticationType authenticationType() const override { return AuthenticationType::Basic; }
    QString loginUser() const override { return _loginUser; }

    /** Ein Kennwort liegt vor. */
    bool ready() const override;
    void fetchFromKeychain() override;
    bool stillValid(QNetworkReply *reply) override;
    /** schreibt das Kennwort in den Schlüsselbund (nicht die Konfiguration: das macht AccountManager::saveAccount) */
    void persist() override;
    /** verwirft Kennwort im Speicher und die Cookies; der Schlüsselbund bleibt */
    void invalidateToken() override;
    /** wie invalidateToken(), zusätzlich das Kennwort aus dem Schlüsselbund */
    void forgetSensitiveData() override;
    bool refreshAccessToken() override { return false; }

    /**
     * Der Server nimmt für dieses Konto kein Kennwort an, nur ein App-Passwort (Zwei-Faktor-Anmeldung, 401 „password
     * login forbidden“ an WebDAV); gilt, bis eine Anfrage mit dem aktuellen Kennwort durchgeht oder das Konto abgemeldet wird.
     */
    bool passwordLoginForbidden() const { return _passwordLoginForbidden; }

    /**
     * Die Anfrage mit dem Authorization-Kopf dieser Zugangsdaten, außer bei DontAddCredentialsAttribute oder ohne
     * Kennwort; so sendet der Netzwerkzugang des Kontos (createAccessManager()) jede Anfrage.
     */
    QNetworkRequest withAuthorization(const QNetworkRequest &request) const;

    /**
     * Übernimmt das Kennwort einer Anmeldung, die außerhalb dieses Objekts lief (Serverumzug: Anmeldung an der neuen
     * Adresse): speichert es unter der aktuellen Kontoadresse und meldet fetched(); eine offene Anmeldeaufforderung
     * schließt sich.
     */
    void replacePassword(const QString &password);

protected:
    enum class KeychainResult { Ok, NotFound, Error };
    using ReadCallback = std::function<void(KeychainResult result, const QString &password, const QString &error)>;
    using StoreCallback = std::function<void(bool ok, const QString &error)>;

    // Zugriffe auf den Schlüsselbund; die Tests ersetzen sie
    virtual void readPassword(const ReadCallback &done);
    virtual void storePassword(const QString &password, const StoreCallback &done);
    virtual void removePassword();
    // Pause vor dem nächsten Versuch nach n Fehlschlägen in Folge (Lesen und Schreiben im Schlüsselbund); die Tests verkürzen sie
    virtual std::chrono::milliseconds keychainRetryDelay(int failedAttempts) const;
    /** schließt eine offene Anmeldeaufforderung ohne Ergebnis (Abmelden, Serverumzug); HttpCredentialsGui hat sie */
    virtual void cancelAskFromUser() { }

    /** Das Kennwort aus der Anmeldeaufforderung oder dem Umzug gilt ab jetzt: speichern, dann fetched() */
    void acceptPassword(const QString &password);

    void slotAuthentication(QNetworkReply *reply, QAuthenticator *authenticator);
    // trug die Anfrage das Kennwort, das jetzt gilt? Nur dann sagt ein 401 etwas über das Kennwort
    bool sentWithCurrentPassword(QNetworkReply *reply) const;
    void fetchPasswordFromKeychain();
    void writePassword();
    void setKeychainError(const QString &error);

    QString _loginUser;
    QString _password;
    // das Kennwort, das der Schlüsselbund bestätigt hat
    QString _storedPassword;
    bool _ready = false;
    bool _passwordLoginForbidden = false;
    int _keychainReadFailures = 0;
    bool _storeRunning = false;
    int _storeFailures = 0;
    // nach wiederholten Fehlschlägen die Meldung des Schlüsselbunds, sonst leer (keychainErrorChanged())
    QString _keychainError;
};

} // namespace OCC
