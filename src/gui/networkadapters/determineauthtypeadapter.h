/*
 * Copyright (C) Lisa Reese <lisa.reese@kiteworks.com>
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

#include "creds/credentialssupport.h"
#include "serverdiagnostics.h"
#include <QObject>
#include <QUrl>

class QNetworkAccessManager;

namespace OCC {

struct DetermineAuthTypeResult
{
    QString error;
    AuthenticationType type = AuthenticationType::Unknown;
    /// erkannte Ursache, wenn error gesetzt ist
    ServerDiagnostics::Issue issue = ServerDiagnostics::Issue::None;
    bool success() const { return error.isEmpty(); }
};

/**
 * @brief The DetermineAuthTypeAdapter class determines how a server wants its users to sign in: OAuth (OAuth2 app or
 * OpenID Connect, sign-in in the browser) or Basic (user name and password, like the 6.0.3 client for servers without
 * the OAuth2 app).
 *
 * Ein PROPFIND ohne Zugangsdaten muss mit 401 abgelehnt werden. Verlangt der Server ein Bearer-Token, ist es OAuth.
 * Nimmt er nur Benutzername und Kennwort an, prüft der Adapter noch die OpenID-Konfiguration (ein Server mit OpenID
 * Connect meldet sich im Browser an, auch wenn sein WebDAV das nicht sagt) und meldet sonst Basic. Ohne erkennbares
 * Verfahren prüft er OpenID-Konfiguration und den Token-Endpunkt der App „OAuth2“, um eine fehlende App eindeutig zu
 * melden.
 */
class DetermineAuthTypeAdapter : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief DetermineAuthTypeAdapter
     * @param nam the network access manager which will run the request.
     * @param url the url for the request.
     * @param parent follows the typical QObject parenting scheme, but this param is rarely needed since an instance should normally be
     * created on the stack and will naturally destruct when it goes out of scope.
     */
    DetermineAuthTypeAdapter(QNetworkAccessManager *nam, const QUrl &url, QObject *parent = nullptr);

    /**
     * @brief getResult runs the request to determine authentication type synchronously
     * @return the relevant data extracted from the reply
     */
    DetermineAuthTypeResult getResult();

private:
    /// bietet der Server eine OpenID-Konfiguration an (/.well-known/openid-configuration)?
    bool offersOpenIdConnect();
    /// OAuth2AppMissing, wenn der Server weder OpenID Connect noch den Token-Endpunkt der App „OAuth2“ bedient
    ServerDiagnostics::Issue probeMissingOAuth();

    QNetworkAccessManager *_nam;
    QUrl _url;
};
}
