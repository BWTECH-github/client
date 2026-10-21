/*
 * Copyright (C) by Klaas Freitag <freitag@kde.org>
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
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
#include "creds/httpcredentialsgui.h"

#include "account.h"
#include "accountsgui/accountmodalwidget.h"
#include "application.h"
#include "creds/basicauthenticationwidget.h"

#include <QLoggingCategory>
#include <QNetworkReply>
#include <QTimer>

namespace OCC {

Q_LOGGING_CATEGORY(lcHttpCredentialsGui, "gui.credentials.http", QtInfoMsg)

HttpCredentialsGui::HttpCredentialsGui(Account *account, const QString &loginUser, const QString &password)
    : HttpCredentials(account, loginUser, password)
{
}

HttpCredentialsGui::~HttpCredentialsGui()
{
    // die Kontoansicht räumt den Dialog auf, sobald er beendet ist
    if (_modalWidget) {
        _modalWidget->reject();
    }
}

void HttpCredentialsGui::askFromUser()
{
    // AccountState::slotInvalidCredentials() hat eben (über invalidateToken()) das Leeren des QNAM-Caches angestoßen; erst
    // danach darf wieder etwas über das Konto laufen
    QTimer::singleShot(0, this, &HttpCredentialsGui::showDialog);
}

void HttpCredentialsGui::showDialog()
{
    if (!_account) {
        return;
    }
    if (_modalWidget) {
        // der Dialog steht schon: nicht noch einen darüber
        return;
    }
    // das Widget wandert in die Modal-Hülle, die Kontoansicht löscht beide nach dem Ende
    _widget = new BasicAuthenticationWidget(_account->url(), loginUser(), nullptr);
    if (_passwordEntered) {
        // das Konto fragt gleich wieder: der Server hat das eingegebene Kennwort abgelehnt
        _widget->setErrorMessage(tr("The user name or password is not correct."));
    }
    _passwordEntered = false;
    _modalWidget = new AccountModalWidget(QString(), _widget, nullptr);

    connect(_widget, &BasicAuthenticationWidget::loginRequested, this, [this](const QString &password) {
        qCInfo(lcHttpCredentialsGui) << "Password entered for" << _account->url().toString();
        _passwordEntered = true;
        if (_modalWidget) {
            _modalWidget->accept();
            _modalWidget.clear();
        }
        _widget.clear();
        acceptPassword(password);
    });
    connect(_widget, &BasicAuthenticationWidget::stayLoggedOutClicked, this, [this] {
        qCInfo(lcHttpCredentialsGui) << "Staying logged out of" << _account->url().toString();
        cancelAskFromUser();
        Q_EMIT requestLogout();
    });

    Q_EMIT requestAccountModal(_modalWidget);
    if (_widget) {
        _widget->setFocus(Qt::OtherFocusReason);
    }
    // ocAppOrNull(): in den Tests gibt es keine Application
    if (auto *app = ocAppOrNull()) {
        app->ensureVisible();
    }
}

bool HttpCredentialsGui::stillValid(QNetworkReply *reply)
{
    const bool valid = HttpCredentials::stillValid(reply);
    if (valid && reply->error() == QNetworkReply::NoError && reply->request().hasRawHeader(QByteArrayLiteral("Authorization"))) {
        // der Server hat das Kennwort angenommen
        _passwordEntered = false;
    }
    return valid;
}

void HttpCredentialsGui::cancelAskFromUser()
{
    _passwordEntered = false;
    if (_modalWidget) {
        _modalWidget->reject();
        _modalWidget.clear();
    }
    _widget.clear();
}

} // namespace OCC
