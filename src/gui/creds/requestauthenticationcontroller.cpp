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
#include "requestauthenticationcontroller.h"

#include "accountsgui/accountview.h"
#include "application.h"
#include "requestauthenticationwidget.h"

#include "accountsgui/accountmodalwidget.h"

#include <config/appconfig.h>

#include <algorithm>

using namespace std::chrono_literals;

namespace OCC {

/**
 * @brief RequestAuthenticationController::RequestAuthenticationController
 * @param widget - may be null, for example when using the "log in" function after user explicitly logged out
 * @param parent - the usual qt parenting scheme
 */
RequestAuthenticationController::RequestAuthenticationController(QObject *parent)
    : QObject{parent}
{
    _restartTimer.setSingleShot(true);
    connect(&_restartTimer, &QTimer::timeout, this, [this] {
        // ohne Dialog (vom Kontenbereich entfernt) gibt es niemanden, der sich anmelden könnte
        if (_account && _widget) {
            startAuthentication(_account);
        }
    });
}

RequestAuthenticationController::~RequestAuthenticationController()
{
    // this should be enough to ensure the widgets get cleaned up, provided the account gui exists.
    // if it does not, the widgets should have been deleted already since they are children of that gui
    if (_modalWidget)
        _modalWidget->reject();
}

std::chrono::milliseconds RequestAuthenticationController::restartDelay(int failedAttempts)
{
    const int doublings = std::clamp(failedAttempts - 1, 0, 5);
    return std::min<std::chrono::milliseconds>(2s * (1 << doublings), 60s);
}

void RequestAuthenticationController::handleSignIn()
{
    // Nur mit angezeigtem Link einer Anmeldung, die noch auf den Browser wartet: Zwischen einem Fehlschlag und dem Neubeginn
    // gibt es keine (der Knopf ist dann gesperrt), und nach dem Rückruf des Browsers ist der Listener schon zu.
    if (_oauth && _linkShown && _oauth->isListening()) {
        _oauth->openBrowser();
    }
}

void RequestAuthenticationController::handleLogOut()
{
    if (_modalWidget) {
        _modalWidget->reject();
    }
    // this ultimately needs to be chained to creds::requestLogout signal
    Q_EMIT requestLogout();
}

void RequestAuthenticationController::discardOAuth()
{
    if (_oauth) {
        // erst später löschen: das läuft auch aus einem Signal von _oauth heraus, etwa mitten im finished des
        // CheckServerJob, den _oauth besitzt. Getrennt meldet es nichts mehr.
        _oauth->disconnect(this);
        _oauth->deleteLater();
        _oauth = nullptr;
    }
}

void RequestAuthenticationController::startAuthentication(Account *account)
{
    Q_ASSERT(account);
    _restartTimer.stop();
    discardOAuth();
    AppConfig systemConfig;
    _account = account;
    _linkShown = false;
    _oauth = new AccountBasedOAuth(_account, systemConfig.openIdConfig(), this);
    connect(_oauth, &OAuth::authorisationLinkChanged, this, &RequestAuthenticationController::authUrlReady);
    connect(_oauth, &OAuth::result, this, &RequestAuthenticationController::handleOAuthResult);

    // if there is an auth error, the widget already exists and is visible so just let it ride.
    if (!_widget) {
        // the widget is reparented to the AccountModalWidget then installed in the accounts gui.
        // it will be cleaned up there - this is not a leak
        _widget = new RequestAuthenticationWidget(nullptr);
        connect(_widget, &RequestAuthenticationWidget::connectClicked, this, &RequestAuthenticationController::handleSignIn);
        connect(_widget, &RequestAuthenticationWidget::stayLoggedOutClicked, this, &RequestAuthenticationController::handleLogOut);

        _modalWidget = new AccountModalWidget(QString(), _widget, nullptr);
        emit requestAccountModal(_modalWidget);
    }
    Q_ASSERT(_widget && _modalWidget);
    _oauth->startAuthentication();
}

void RequestAuthenticationController::restartAfterError()
{
    // der angezeigte Link führt ins Leere, sobald diese Anmeldung beendet ist
    _widget->clearAuthUrl();
    discardOAuth();
    // nicht sofort neu beginnen: Ist der Server nicht erreichbar, scheitert jeder Anlauf gleich wieder, eine enge Schleife
    ++_failedAttempts;
    _restartTimer.start(restartDelay(_failedAttempts));
}

void RequestAuthenticationController::authUrlReady()
{
    if (_widget) {
        QString authUrl = _oauth->authorisationLink().toString(QUrl::FullyEncoded);
        if (authUrl.isEmpty()) {
            _widget->setErrorMessage(tr("Unable to retrieve authorization link! Please contact your system administrator."));
        } else {
            _failedAttempts = 0;
            _linkShown = true;
        }
        _widget->setAuthUrl(authUrl);
    } else {
        // just run with it
        _oauth->openBrowser();
    }
}

void RequestAuthenticationController::handleOAuthResult(OAuth::Result result, const QString &accessToken, const QString &refreshToken)
{
    QString errString;

    switch (result) {
    case OAuth::ErrorIdPUnreachable:
        errString = tr("IdP is unreachable. Contact your system administrator or try again later.");
        break;
    case OAuth::NotSupported:
        // der Browser ließ sich nicht öffnen; mit FORCE_ASSERTS (Beta) brach hier ein Q_ASSERT das Programm ab
        errString = tr("The browser could not be opened. Copy the link and open it in your browser.");
        break;
    case OAuth::ErrorInsecureUrl:
        // der Server nennt eine Anmeldeseite ohne HTTPS (etwa hinter einem Proxy ohne overwriteprotocol); mit FORCE_ASSERTS
        // (Beta) brach hier ein Q_ASSERT das Programm ab
        errString = tr("The sign-in page does not use a secure connection (HTTPS), so it was not opened. Please contact your system administrator.");
        break;
    case OAuth::Error:
        errString = tr("Authentication failed.");
        break;
    case OAuth::LoggedIn:
        errString.clear();
        break;
    }

    // Nur nach dem Browser oder einem Klick nach vorn holen, nicht bei jedem Anlauf, der den Server nicht erreicht. Vorher
    // lesen: authenticationSucceeded() löscht diesen Controller.
    const bool raiseWindow = _linkShown;

    if (errString.isEmpty()) {
        if (_modalWidget)
            _modalWidget->accept();
        Q_EMIT authenticationSucceeded(accessToken, refreshToken);
    } else if (_widget && _account) {
        _widget->setErrorMessage(errString);
        // Browser ließ sich nicht öffnen: Listener, state und angezeigter Link bleiben gültig, die Meldung fordert genau zum
        // Kopieren auf (wie im Assistenten). Jeder andere Fehlschlag beendet diese Anmeldung.
        if (result != OAuth::NotSupported) {
            restartAfterError();
        }
    } else {
        Q_EMIT authenticationFailed(errString);
    }

    if (raiseWindow) {
        // ocAppOrNull(): in den Tests gibt es keine Application
        if (auto *app = ocAppOrNull()) {
            app->ensureVisible();
        }
    }
}
}
