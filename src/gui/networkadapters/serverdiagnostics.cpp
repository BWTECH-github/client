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

#include "serverdiagnostics.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace OCC {

namespace {
    QJsonObject parseJsonObject(const QByteArray &body)
    {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(body, &error);
        return error.error == QJsonParseError::NoError ? document.object() : QJsonObject();
    }
}

ServerDiagnostics::Issue ServerDiagnostics::classifyFailure(QNetworkReply::NetworkError error, int httpStatus)
{
    if (httpStatus > 0) {
        switch (httpStatus) {
        case 502:
        case 503:
        case 504:
            return Issue::Maintenance;
        case 404:
        case 410:
            return Issue::NotOwncloudServer;
        default:
            return Issue::Other;
        }
    }

    switch (error) {
    case QNetworkReply::SslHandshakeFailedError:
        return Issue::Tls;
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    // eine abgelaufene Transfer-Zeitbegrenzung bricht die Anfrage mit OperationCanceledError ab
    case QNetworkReply::OperationCanceledError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::ProxyAuthenticationRequiredError:
    case QNetworkReply::UnknownProxyError:
        return Issue::Unreachable;
    default:
        return Issue::Other;
    }
}

ServerDiagnostics::ServerStatus ServerDiagnostics::parseStatus(const QByteArray &body)
{
    ServerStatus status;
    const QJsonObject object = parseJsonObject(body);
    // status.php von owncloud.online (wie oC10 und oCIS) enthält immer "installed" und "version"
    if (!object.value(QStringLiteral("installed")).isBool() || !object.value(QStringLiteral("version")).isString()) {
        return status;
    }
    status.valid = true;
    // während eines ausstehenden Updates antwortet der Server wie im Wartungsmodus mit 503
    status.maintenance = object.value(QStringLiteral("maintenance")).toBool() || object.value(QStringLiteral("needsDbUpgrade")).toBool();
    return status;
}

bool ServerDiagnostics::isOpenIdConfiguration(const QByteArray &body)
{
    const QJsonObject object = parseJsonObject(body);
    return !object.value(QStringLiteral("authorization_endpoint")).toString().isEmpty() && !object.value(QStringLiteral("token_endpoint")).toString().isEmpty();
}

ServerDiagnostics::Issue ServerDiagnostics::classifyOAuth2TokenReply(int httpStatus, const QByteArray &body)
{
    // die App antwortet auf eine unvollständige Anfrage immer mit {"error": "..."} (RFC 6749, Abschnitt 5.2)
    if (parseJsonObject(body).value(QStringLiteral("error")).isString()) {
        return Issue::Other;
    }
    // keine HTTP-Antwort oder ein Serverfehler sagen nichts darüber aus, ob die App fehlt
    if (httpStatus <= 0 || httpStatus >= 500) {
        return Issue::Other;
    }
    return Issue::OAuth2AppMissing;
}

QString ServerDiagnostics::message(Issue issue, const QUrl &url)
{
    const QString address = url.toDisplayString();
    switch (issue) {
    case Issue::Unreachable:
        return tr("The server at %1 could not be reached. Please check the address and your internet connection.").arg(address);
    case Issue::Tls:
        return tr("No secure connection to %1 could be established: the server's certificate is invalid or was not accepted. If the problem persists, "
                  "please contact the server's administrator.")
            .arg(address);
    case Issue::Maintenance:
        return tr("The server at %1 is in maintenance mode or temporarily unavailable. Please try again later.").arg(address);
    case Issue::NotOwncloudServer:
        return tr("No owncloud.online server was found at %1. Please check the address.").arg(address);
    case Issue::OAuth2AppMissing:
        return tr("The \"OAuth2\" app is not installed or not enabled on this server. Please ask your administrator to install and enable it from the "
                  "Marketplace.")
            + QStringLiteral("\n\n")
            + tr("Note for administrators: the desktop client signs in through the \"OAuth2\" app. Install it from the Marketplace and enable it for all "
                 "users (command line: occ app:enable oauth2).");
    case Issue::None:
    case Issue::Other:
        break;
    }
    return tr("Could not detect compatible server at %1").arg(address);
}

}
