/*
 * Copyright (C) Lisa Reese (lisa.reese@kiteworks.com)
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

#include <QByteArray>
#include <QNetworkRequest>
#include <QString>

namespace OCC {

inline constexpr QNetworkRequest::Attribute DontAddCredentialsAttribute = QNetworkRequest::User;

/**
 * Anmeldeart eines Kontos: OAuth2/OpenID Connect (Anmeldung im Browser, Bearer-Token) oder Benutzername und Kennwort
 * (HTTP Basic, wie der 6.0.3-Client für Server ohne die App „OAuth2“).
 */
enum class AuthenticationType { Unknown = 0, OAuth, Basic };

/** Wert des Authorization-Kopfes für Benutzername und Kennwort: „Basic “ + Base64(user:password) */
inline QByteArray basicAuthorizationHeader(const QString &user, const QString &password)
{
    return QByteArrayLiteral("Basic ") + (user + QLatin1Char(':') + password).toUtf8().toBase64();
}

/**
 * Anmeldeart aus dem WWW-Authenticate-Kopf einer Ablehnung (401): Verlangt der Server irgendwo ein Bearer-Token, gilt
 * OAuth, sonst Basic, wenn er Benutzername und Kennwort annimmt (auch „Basic realm=…, Bearer realm=…“ ist OAuth). Ohne
 * erkennbares Verfahren (leer, nur Negotiate/NTLM/Digest) Unknown.
 */
inline AuthenticationType authenticationTypeFromChallenge(const QByteArray &wwwAuthenticate)
{
    // die Verfahren stehen durch Komma oder Leerzeichen getrennt vor ihren Parametern, Groß- und Kleinschreibung ist egal
    const auto offersScheme = [](const QByteArray &header, const QByteArray &scheme) {
        for (qsizetype pos = header.indexOf(scheme); pos >= 0; pos = header.indexOf(scheme, pos + 1)) {
            const bool startsToken = pos == 0 || header.at(pos - 1) == ' ' || header.at(pos - 1) == ',' || header.at(pos - 1) == '\t';
            const qsizetype end = pos + scheme.size();
            const bool endsToken = end == header.size() || header.at(end) == ' ' || header.at(end) == ',' || header.at(end) == '\t';
            if (startsToken && endsToken) {
                return true;
            }
        }
        return false;
    };
    const QByteArray header = wwwAuthenticate.trimmed().toLower();
    if (offersScheme(header, QByteArrayLiteral("bearer"))) {
        return AuthenticationType::OAuth;
    }
    if (offersScheme(header, QByteArrayLiteral("basic"))) {
        return AuthenticationType::Basic;
    }
    return AuthenticationType::Unknown;
}
}
