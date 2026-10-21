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

#include <QObject>
#include <QTimer>

#include "oauth.h"

namespace OCC {

class RequestAuthenticationWidget;
class AccountBasedOAuth;
class Account;
class AccountModalWidget;

class RequestAuthenticationController : public QObject
{
    Q_OBJECT
public:
    explicit RequestAuthenticationController(QObject *parent);

    ~RequestAuthenticationController() override;

    void startAuthentication(Account *account);

    /** Pause vor dem Neubeginn nach dem n-ten Fehlschlag in Folge (n >= 1): 2 s, danach doppelt so lang, höchstens 60 s */
    static std::chrono::milliseconds restartDelay(int failedAttempts);

Q_SIGNALS:
    void requestAccountModal(OCC::AccountModalWidget *widget);
    void requestLogout();
    // expiresIn: Lebensdauer des Access-Tokens aus dem Code-Tausch in Sekunden, 0: unbekannt
    void authenticationSucceeded(const QString &token, const QString &refreshToken, qint64 expiresIn);
    // this is only emitted if the controller is running without a widget.
    void authenticationFailed(const QString &errorMessage);

private:
    void handleSignIn();
    void handleLogOut();
    void authUrlReady();
    void handleOAuthResult(OAuth::Result, const QString &accessToken, const QString &refreshToken, qint64 expiresIn);
    /** beendet die laufende Anmeldung (Listener zu, Link ungültig) und beginnt nach restartDelay() neu */
    void restartAfterError();
    void discardOAuth();

    AccountBasedOAuth *_oauth = nullptr;
    QPointer<Account> _account = nullptr;
    // Fehlschläge in Folge, seit zuletzt ein Link angezeigt wurde
    int _failedAttempts = 0;
    // der Link dieser Anmeldung wird angezeigt: ab hier folgt jedes Ergebnis einer Handlung des Nutzers
    bool _linkShown = false;
    QTimer _restartTimer;

    // these will be cleaned up by the account gui automatically when the AccountModalWidget is "finished"
    // that is handled internally when accept() or reject() is called
    QPointer<RequestAuthenticationWidget> _widget = nullptr;
    QPointer<AccountModalWidget> _modalWidget = nullptr;
};
}
