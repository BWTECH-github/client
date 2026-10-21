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
#pragma once

#include "creds/httpcredentials.h"
#include "gui/owncloudguilib.h"

#include <QPointer>

namespace OCC {

class AccountModalWidget;
class BasicAuthenticationWidget;

/**
 * Anmeldung mit Benutzername und Kennwort im Desktop-Client: Verlangt das Konto eine Anmeldung (askFromUser()), zeigt die
 * Kontoansicht den Kennwortdialog (BasicAuthenticationWidget über requestAccountModal()). „Anmelden“ speichert das
 * Kennwort und meldet fetched(), das Konto prüft die Verbindung; lehnt der Server das Kennwort ab, fragt es erneut und
 * der Dialog nennt den Grund. „Abgemeldet bleiben“ meldet requestLogout().
 */
class OWNCLOUDGUI_EXPORT HttpCredentialsGui : public HttpCredentials
{
    Q_OBJECT
public:
    HttpCredentialsGui(Account *account, const QString &loginUser, const QString &password);
    ~HttpCredentialsGui() override;

    void askFromUser() override;
    /** wie HttpCredentials; nimmt der Server das Kennwort an, gilt ein späterer 401 nicht mehr als Tippfehler im Dialog */
    bool stillValid(QNetworkReply *reply) override;

protected:
    void cancelAskFromUser() override;

private:
    void showDialog();

    QPointer<AccountModalWidget> _modalWidget;
    QPointer<BasicAuthenticationWidget> _widget;
    // der Nutzer hat im letzten Dialog ein Kennwort eingegeben: fragt das Konto gleich wieder, war es falsch
    bool _passwordEntered = false;
};

} // namespace OCC
