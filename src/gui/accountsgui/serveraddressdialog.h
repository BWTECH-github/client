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

#include <QDialog>
#include <QPointer>

class QLabel;
class QLineEdit;
class QPushButton;

namespace OCC {

class AccountState;
class ServerMoveController;

/**
 * @brief Dialog „Serveradresse ändern…“ im Kontomenü (nur Konten ohne Spaces)
 *
 * Nimmt die neue Adresse entgegen, führt durch die Anmeldung an der neuen Adresse und schließt sich, sobald das Konto
 * umgestellt ist. Den Ablauf und alle Prüfungen übernimmt ServerMoveController.
 */
class OWNCLOUDGUI_EXPORT ServerAddressDialog : public QDialog
{
    Q_OBJECT
public:
    ServerAddressDialog(AccountState *accountState, QWidget *parent);
    ~ServerAddressDialog() override;

    ServerMoveController *controller() const { return _controller; }

    /// bricht den Ablauf ab; während das Konto umgestellt wird, geschieht nichts
    void reject() override;

private:
    void startMove();
    void setBusy(bool busy);
    void showMessage(const QString &message, bool isError);

    QPointer<AccountState> _accountState;
    ServerMoveController *_controller;
    QLineEdit *_urlEdit;
    QLabel *_messageLabel;
    QLabel *_linkLabel;
    QPushButton *_startButton;
    QPushButton *_browserButton;
    QPushButton *_cancelButton;
};

}
