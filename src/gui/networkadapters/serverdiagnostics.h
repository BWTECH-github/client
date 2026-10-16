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

#include <QCoreApplication>
#include <QNetworkReply>
#include <QUrl>

namespace OCC {

/**
 * @brief Wertet die Antworten aus, die der Einrichtungsassistent beim Prüfen der Serveradresse erhält.
 *
 * Jede eindeutig erkennbare Ursache bekommt eine verständliche Meldung, statt dass ein technischer
 * Netzwerkfehler durchgereicht wird. Die Zuordnung stützt sich auf HTTP-Status und Antwortinhalt,
 * nie auf den Wortlaut einer Fehlermeldung des Servers.
 */
class ServerDiagnostics final
{
    Q_DECLARE_TR_FUNCTIONS(ServerDiagnostics)

public:
    enum class Issue {
        None,
        /// DNS, Verbindung abgelehnt, Zeitüberschreitung, Proxy
        Unreachable,
        /// Zertifikat ungültig oder abgelehnt, TLS-Handshake gescheitert
        Tls,
        /// Wartungsmodus, ausstehendes Update oder 502/503/504
        Maintenance,
        /// unter der Adresse antwortet kein owncloud.online-Server
        NotOwncloudServer,
        /// owncloud.online-Server ohne eingeschaltete App „OAuth2“
        OAuth2AppMissing,
        /// keine der obigen Ursachen sicher erkennbar
        Other
    };

    struct ServerStatus
    {
        /// Antwort ist ein status.php-Dokument eines owncloud.online-Servers
        bool valid = false;
        bool maintenance = false;
    };

    ServerDiagnostics() = delete;

    /**
     * Ordnet eine gescheiterte Anfrage zu. @p httpStatus ist 0, wenn keine HTTP-Antwort ankam;
     * dann entscheidet allein der Transportfehler.
     */
    static Issue classifyFailure(QNetworkReply::NetworkError error, int httpStatus);

    static ServerStatus parseStatus(const QByteArray &body);

    /// true, wenn @p body ein OpenID-Connect-Discovery-Dokument mit Autorisierungs- und Token-Endpunkt ist
    static bool isOpenIdConfiguration(const QByteArray &body);

    /**
     * Wertet die Antwort des Token-Endpunkts auf eine Anfrage ohne grant_type aus: Die App „OAuth2“ antwortet
     * mit einem JSON-Fehlerobjekt, ohne die App kommt eine Umleitung zur Anmeldeseite, HTML oder 404.
     * @return OAuth2AppMissing, wenn der Server antwortet, aber nicht die App; sonst Other
     */
    static Issue classifyOAuth2TokenReply(int httpStatus, const QByteArray &body);

    /// Meldung zur Ursache; Other und None bekommen den allgemeinen Text „kein kompatibler Server“
    static QString message(Issue issue, const QUrl &url);
};

}
