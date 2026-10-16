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

#include "determineauthtypeadapter.h"

#include "abstractcorejob.h"
#include "common/utility.h"
#include "creds/credentialssupport.h"

#include <QEventLoop>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace OCC {

Q_LOGGING_CATEGORY(lcDetermineAuthTypeAdapter, "gui.networkadapters.determineauthtypeadapter");

namespace {
    // dieselben Pfade wie in creds/oauth.cpp, das ohne OpenID-Konfiguration auf die App „OAuth2“ zurückfällt
    const QString openIdConfigurationPathC = QStringLiteral("/.well-known/openid-configuration");
    const QString oauth2TokenPathC = QStringLiteral("/index.php/apps/oauth2/api/v1/token");

    void waitForFinished(QNetworkReply *reply)
    {
        if (reply->isFinished()) {
            return;
        }
        QEventLoop waitLoop;
        QObject::connect(reply, &QNetworkReply::finished, &waitLoop, &QEventLoop::quit);
        waitLoop.exec();
    }
}

DetermineAuthTypeAdapter::DetermineAuthTypeAdapter(QNetworkAccessManager *nam, const QUrl &url, QObject *parent)
    : QObject(parent)
    , _nam(nam)
    , _url(url)
{
}

DetermineAuthTypeResult DetermineAuthTypeAdapter::getResult()
{
    DetermineAuthTypeResult res;
    if (!_nam)
        return res;

    // we explicitly use a legacy dav path here
    QNetworkRequest req = AbstractCoreJobFactory::makeRequest(Utility::concatUrlPath(_url, QStringLiteral("remote.php/webdav/")));

    req.setAttribute(DontAddCredentialsAttribute, true);
    req.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);

    QNetworkReply *reply = _nam->sendCustomRequest(req, "PROPFIND");
    waitForFinished(reply);

    if (reply->error() == QNetworkReply::AuthenticationRequiredError) {
        // the header key is case insensitive per debug testing
        const QByteArray rawHeader = reply->rawHeader(QByteArrayLiteral("www-authenticate")).toLower();

        if (rawHeader.contains(QByteArrayLiteral("bearer "))) {
            res.type = AuthenticationType::OAuth;
        } else {
            if (rawHeader.isEmpty()) {
                qCWarning(lcDetermineAuthTypeAdapter) << "Did not receive WWW-Authenticate reply to auth-test PROPFIND";
            } else {
                qCWarning(lcDetermineAuthTypeAdapter) << "Did not receive bearer in WWW-Authenticate reply to auth-test PROPFIND";
            }
            res.issue = probeMissingOAuth();
            res.error =
                res.issue == ServerDiagnostics::Issue::OAuth2AppMissing ? ServerDiagnostics::message(res.issue, _url) : tr("Server does not support OAuth");
        }
    } else if (reply->error() == QNetworkReply::NoError) {
        res.issue = ServerDiagnostics::Issue::Other;
        res.error = tr("Server did not ask for authorization");
    } else {
        res.issue = ServerDiagnostics::classifyFailure(reply->error(), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        res.error = res.issue == ServerDiagnostics::Issue::Other ? tr("Failed to determine authentication type: %1").arg(reply->errorString())
                                                                 : ServerDiagnostics::message(res.issue, _url);
    }

    delete reply;
    return res;
}

ServerDiagnostics::Issue DetermineAuthTypeAdapter::probeMissingOAuth()
{
    // Wer OpenID Connect anbietet, braucht die App „OAuth2“ nicht; dann liegt die Ursache woanders
    QNetworkRequest openIdRequest = AbstractCoreJobFactory::makeRequest(Utility::concatUrlPath(_url, openIdConfigurationPathC));
    openIdRequest.setAttribute(DontAddCredentialsAttribute, true);
    QNetworkReply *openIdReply = _nam->get(openIdRequest);
    waitForFinished(openIdReply);
    const bool offersOpenIdConnect = openIdReply->error() == QNetworkReply::NoError && ServerDiagnostics::isOpenIdConfiguration(openIdReply->readAll());
    delete openIdReply;
    if (offersOpenIdConnect) {
        qCWarning(lcDetermineAuthTypeAdapter) << "Server offers OpenID Connect but did not ask for a bearer token";
        return ServerDiagnostics::Issue::Other;
    }

    // Eine Anfrage ohne grant_type beantwortet die App mit einem JSON-Fehler. Ohne die App leitet der Server zur
    // Anmeldeseite um; dieser Umleitung nicht folgen, sonst käme die Anmeldeseite mit 200 zurück.
    QNetworkRequest tokenRequest = AbstractCoreJobFactory::makeRequest(Utility::concatUrlPath(_url, oauth2TokenPathC));
    tokenRequest.setAttribute(DontAddCredentialsAttribute, true);
    tokenRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    tokenRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply *tokenReply = _nam->post(tokenRequest, QByteArray());
    waitForFinished(tokenReply);
    const int httpStatus = tokenReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const ServerDiagnostics::Issue issue = ServerDiagnostics::classifyOAuth2TokenReply(httpStatus, tokenReply->readAll());
    qCInfo(lcDetermineAuthTypeAdapter) << "OAuth2 token endpoint answered" << httpStatus << tokenReply->header(QNetworkRequest::ContentTypeHeader).toString()
                                       << (issue == ServerDiagnostics::Issue::OAuth2AppMissing ? "- the OAuth2 app is missing" : "- cause unclear");
    delete tokenReply;
    return issue;
}
}
