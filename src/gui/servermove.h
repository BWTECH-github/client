/*
 * Copyright (C) by BW-Tech GmbH for owncloud.online.
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

#include "gui/owncloudguilib.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QSslCertificate>
#include <QSslError>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace OCC {

class AccountState;
class OAuth;

/**
 * @brief Bausteine für „Serveradresse ändern…“: ein Konto ohne Spaces (ownCloud 10/owncloud.online) zieht mit seinem
 * Server auf eine neue Adresse um, ohne dass Konto, Ordnerpaare, Journale oder lokale Dateien neu entstehen.
 *
 * Sicherheit: Die neue Adresse wird nur mit status.php geprüft, ohne Zugangsdaten. Weiterleitungen gelten nur auf
 * demselben Host (und von HTTP zu HTTPS). Danach meldet sich der Nutzer an der neuen Adresse neu an; die vorhandenen
 * Token gehen nie an den neuen Host. Erst wenn derselbe Benutzer angemeldet ist und die Datei-IDs zur bisherigen
 * Instanz passen, wird umgestellt – sonst könnte ein fremder Server mit gleichem Benutzernamen lokale Dateien als
 * „auf dem Server gelöscht“ erscheinen lassen.
 */
class OWNCLOUDGUI_EXPORT ServerMove
{
    Q_DECLARE_TR_FUNCTIONS(ServerMove)

public:
    /**
     * Wandelt eine Eingabe in eine Serveradresse: Ohne Schema wird „https://“ ergänzt, eine angehängte „status.php“ oder
     * „index.php“ und ein „/“ am Ende entfallen. Zugangsdaten, Anfrage und Fragment in der Adresse sind nicht erlaubt.
     * Bei einem Fehler ist das Ergebnis leer und @p error nennt den Grund.
     */
    static QUrl normalizeServerUrl(const QString &input, QString *error);

    /// Serveradresse zu einer status.php-Adresse („https://neu.example/oc/status.php“ → „https://neu.example/oc“), sonst leer
    static QUrl serverUrlFromStatusUrl(const QUrl &statusUrl);

    /// Darf einer Weiterleitung von @p from nach @p to gefolgt werden? Nur auf demselben Host und Port, nie von HTTPS zu HTTP.
    static bool isSafeRedirect(const QUrl &from, const QUrl &to);

    /// Instanz-ID aus einer ownCloud-10-Datei-ID („00000123oc1a2b3c4d5e“ → „oc1a2b3c4d5e“), leer, wenn keine erkennbar ist
    static QString instanceIdFromFileId(const QByteArray &fileId);

    /// Instanz-ID aus den Sync-Journalen der Ordnerpaare des Kontos; leer, solange nichts synchronisiert wurde
    static QString knownInstanceId(const QUuid &accountId);

    /// Datei-ID (oc:id) des angefragten Eintrags aus der Antwort auf ein PROPFIND mit Tiefe 0, leer, wenn sie fehlt
    static QByteArray fileIdFromPropfind(const QByteArray &xml);

    /// ersetzt in @p url den Anfang @p oldBase durch @p newBase; ohne Ergebnis, wenn @p url nicht unter @p oldBase liegt
    static std::optional<QUrl> rebaseUrl(const QUrl &url, const QUrl &oldBase, const QUrl &newBase);

    /**
     * Stellt das Konto auf @p newUrl um, wenn Server, Benutzer und Instanz geprüft sind: Ordnerpaare werden mit der neuen
     * WebDAV-Adresse neu aufgebaut (Journale, lokale Ordner und Einstellungen bleiben), die alten Zugangsdaten gelöscht und
     * die neuen gespeichert, danach verbindet sich das Konto neu.
     * @return eine Fehlermeldung, leer bei Erfolg
     */
    static QString applyNewServerUrl(AccountState *accountState, const QUrl &newUrl, const QString &accessToken, const QString &refreshToken);
};

/**
 * @brief Prüft eine Serveradresse über status.php, ohne Zugangsdaten mitzuschicken
 *
 * Folgt Weiterleitungen nur, wenn ServerMove::isSafeRedirect() sie erlaubt; eine Weiterleitung auf einen anderen Host
 * wird gemeldet (Result::foreignRedirect), aber nicht befolgt.
 */
class OWNCLOUDGUI_EXPORT ServerStatusCheck : public QObject
{
    Q_OBJECT
public:
    struct Result
    {
        bool ok = false;
        /// geprüfte Adresse nach erlaubten Weiterleitungen
        QUrl serverUrl;
        /// Versionsangabe des Servers für die Anzeige
        QString version;
        /// Meldung für den Nutzer, wenn ok falsch ist
        QString error;
        /// Ziel einer Weiterleitung auf einen anderen Host, der nicht gefolgt wurde
        QUrl foreignRedirect;
    };

    static constexpr int maxRedirects = 5;

    ServerStatusCheck(QNetworkAccessManager *accessManager, QObject *parent);

    void start(const QUrl &serverUrl);

Q_SIGNALS:
    void finished(const OCC::ServerStatusCheck::Result &result);

private:
    void request(const QUrl &serverUrl, int redirectsLeft);
    void handleReply(QNetworkReply *reply, const QUrl &serverUrl, int redirectsLeft);

    QPointer<QNetworkAccessManager> _accessManager;
};

/**
 * @brief Ablauf von „Serveradresse ändern…“: Adresse prüfen, an der neuen Adresse anmelden, Benutzer und Instanz
 * vergleichen, umstellen
 */
class OWNCLOUDGUI_EXPORT ServerMoveController : public QObject
{
    Q_OBJECT
public:
    /// liefert einen Netzwerkzugang ohne die Zugangsdaten des Kontos (in Tests einen gefälschten)
    using AccessManagerFactory = std::function<QNetworkAccessManager *(QObject *parent)>;
    /// fragt, ob den Zertifikaten aus @p errors für @p host vertraut werden soll (im Dialog: TlsErrorDialog)
    using TlsPrompt = std::function<bool(const QList<QSslError> &errors, const QString &host)>;

    ServerMoveController(AccountState *accountState, QObject *parent);
    ~ServerMoveController() override;

    void setAccessManagerFactory(const AccessManagerFactory &factory);
    /// ohne Rückfrage scheitert die Prüfung einer Adresse mit unbekanntem Zertifikat
    void setTlsPrompt(const TlsPrompt &prompt);

    /// prüft @p input und meldet den Nutzer danach im Browser an der neuen Adresse an
    void start(const QString &input);
    /// bricht ab, ohne etwas zu ändern
    void cancel();
    /// öffnet die Anmeldeseite erneut, solange die Anmeldung wartet
    void openBrowserAgain();

    bool isBusy() const;
    /// das Konto wird gerade umgestellt (nicht mehr abbrechbar)
    bool isApplying() const { return _applying; }

    /**
     * Schritt nach der Anmeldung an @p newUrl: Benutzer und Instanz prüfen (PROPFIND mit dem neuen Token), dann umstellen.
     * Wird sonst nach der Anmeldung im Browser aufgerufen; öffentlich für Tests.
     */
    void verifyAndApply(const QUrl &newUrl, const QString &accessToken, const QString &refreshToken);

Q_SIGNALS:
    /// Zwischenstand für die Anzeige
    void progress(const QString &message);
    /// die Anmeldeseite an der neuen Adresse, zum Kopieren, falls sich kein Browser öffnet
    void signInLinkChanged(const QUrl &link);
    void failed(const QString &message);
    void succeeded(const QUrl &newUrl);

public:
    /// so lange höchstens warten, bis laufende Synchronisationen des Kontos beendet sind; danach bleibt alles, wie es war
    static constexpr int syncStopTimeoutMs = 30 * 1000;

private:
    void startStatusCheck(const QUrl &url);
    void onStatusChecked(const ServerStatusCheck::Result &result);
    /// merkt sich unbekannte Zertifikate für die Rückfrage nach der gescheiterten Prüfung, bekannte lässt es zu
    void onSslErrors(QNetworkReply *reply, const QList<QSslError> &errors);
    void startSignIn(const QUrl &newUrl);
    void finishVerification(QNetworkReply *reply, const QUrl &newUrl, const QString &accessToken, const QString &refreshToken);
    /// stellt um, sobald kein Lauf des Kontos mehr läuft (geprüft alle 100 ms, ohne verschachtelte Ereignisschleife)
    void applyWhenSyncsStopped(const QUrl &newUrl, const QString &accessToken, const QString &refreshToken);
    /// beendet das Warten und gibt die angehaltene Synchronisation wieder frei
    void releaseSyncs();
    void fail(const QString &message);
    void reset();
    QNetworkAccessManager *accessManager();

    QPointer<AccountState> _accountState;
    AccessManagerFactory _accessManagerFactory;
    TlsPrompt _tlsPrompt;
    QPointer<QNetworkAccessManager> _accessManager;
    /// Zertifikate, denen das Konto schon vertraut, und die in diesem Dialog bestätigten
    QSet<QSslCertificate> _trustedCertificates;
    /// in diesem Dialog bestätigt; nach dem Umzug gelten sie für das Konto
    QSet<QSslCertificate> _confirmedCertificates;
    /// unbekannte Zertifikate der laufenden Prüfung
    QList<QSslError> _pendingTlsErrors;
    QString _pendingTlsHost;
    QUrl _statusTarget;
    QPointer<ServerStatusCheck> _statusCheck;
    QPointer<OAuth> _oauth;
    QPointer<QNetworkReply> _verificationReply;
    QPointer<QTimer> _syncStopPoll;
    QElapsedTimer _syncStopTime;
    // FolderMan::setSyncEnabled(false) gilt bis releaseSyncs()
    bool _syncsHeld = false;
    QUrl _newUrl;
    bool _applying = false;
};

}

Q_DECLARE_METATYPE(OCC::ServerStatusCheck::Result)
