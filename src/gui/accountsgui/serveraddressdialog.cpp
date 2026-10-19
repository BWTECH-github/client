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
#include "serveraddressdialog.h"

#include "accountstate.h"
#include "libsync/account.h"
#include "servermove.h"
#include "tlserrordialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

namespace OCC {

ServerAddressDialog::ServerAddressDialog(AccountState *accountState, QWidget *parent)
    : QDialog(parent)
    , _accountState(accountState)
    , _controller(new ServerMoveController(accountState, this))
{
    setObjectName(QStringLiteral("ServerAddressDialog"));
    setWindowTitle(tr("Change server address"));

    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(tr("If your server has moved to a new address, enter the new address here. The account and its folders, files and "
                                "settings are kept. To make sure that it is the same server and the same user, you sign in again at the new "
                                "address."),
        this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *form = new QFormLayout;
    const QUrl currentUrl = accountState && accountState->account() ? accountState->account()->url() : QUrl();
    auto *currentLabel = new QLabel(currentUrl.toDisplayString(), this);
    currentLabel->setTextFormat(Qt::PlainText);
    currentLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Current address:"), currentLabel);

    _urlEdit = new QLineEdit(this);
    _urlEdit->setObjectName(QStringLiteral("newServerUrlEdit"));
    _urlEdit->setPlaceholderText(QStringLiteral("https://"));
    _urlEdit->setAccessibleName(tr("New address"));
    // Vorschlag: die Adresse, die der Server zuletzt per dauerhafter Weiterleitung nannte; übernommen wird nichts ungeprüft
    if (accountState && accountState->movedServerUrl().isValid()) {
        _urlEdit->setText(accountState->movedServerUrl().toDisplayString());
    }
    form->addRow(tr("New address:"), _urlEdit);
    layout->addLayout(form);

    _messageLabel = new QLabel(this);
    _messageLabel->setObjectName(QStringLiteral("messageLabel"));
    _messageLabel->setWordWrap(true);
    _messageLabel->setTextFormat(Qt::PlainText);
    _messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _messageLabel->hide();
    layout->addWidget(_messageLabel);

    _linkLabel = new QLabel(this);
    _linkLabel->setObjectName(QStringLiteral("signInLinkLabel"));
    _linkLabel->setWordWrap(true);
    _linkLabel->setTextFormat(Qt::PlainText);
    _linkLabel->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    _linkLabel->hide();
    layout->addWidget(_linkLabel);

    layout->addStretch();

    auto *buttons = new QDialogButtonBox(this);
    _browserButton = buttons->addButton(tr("Open browser again"), QDialogButtonBox::ActionRole);
    _browserButton->setObjectName(QStringLiteral("openBrowserAgainButton"));
    _browserButton->hide();
    _cancelButton = buttons->addButton(QDialogButtonBox::Cancel);
    _startButton = buttons->addButton(tr("Check and sign in"), QDialogButtonBox::AcceptRole);
    _startButton->setObjectName(QStringLiteral("checkAndSignInButton"));
    _startButton->setDefault(true);
    layout->addWidget(buttons);

    connect(_startButton, &QPushButton::clicked, this, &ServerAddressDialog::startMove);
    connect(_urlEdit, &QLineEdit::returnPressed, this, &ServerAddressDialog::startMove);
    connect(_browserButton, &QPushButton::clicked, _controller, &ServerMoveController::openBrowserAgain);
    connect(_cancelButton, &QPushButton::clicked, this, &ServerAddressDialog::reject);

    // ein unbekanntes Zertifikat der neuen Adresse lässt sich wie bei der Kontoeinrichtung bestätigen
    _controller->setTlsPrompt([this](const QList<QSslError> &errors, const QString &host) {
        // auf dem Heap: wird dieser Dialog während der Rückfrage gelöscht, löscht er sie mit (QPointer wird null)
        QPointer<TlsErrorDialog> dialog = new TlsErrorDialog(errors, host, this);
        const bool trusted = dialog->exec() == QDialog::Accepted;
        delete dialog;
        return trusted;
    });
    connect(_controller, &ServerMoveController::progress, this, [this](const QString &message) { showMessage(message, false); });
    connect(_controller, &ServerMoveController::signInLinkChanged, this, [this](const QUrl &link) {
        _linkLabel->setText(tr("Sign-in link: %1").arg(link.toString(QUrl::FullyEncoded)));
        _linkLabel->setVisible(!link.isEmpty());
        _browserButton->setVisible(!link.isEmpty());
    });
    connect(_controller, &ServerMoveController::failed, this, [this](const QString &message) {
        setBusy(false);
        showMessage(message, true);
    });
    connect(_controller, &ServerMoveController::succeeded, this, [this] {
        setBusy(false);
        accept();
    });
}

ServerAddressDialog::~ServerAddressDialog() = default;

void ServerAddressDialog::reject()
{
    // während das Konto umgestellt wird, lässt sich nichts mehr abbrechen
    if (_controller->isApplying()) {
        return;
    }
    _controller->cancel();
    QDialog::reject();
}

void ServerAddressDialog::startMove()
{
    if (_controller->isBusy()) {
        return;
    }
    setBusy(true);
    _linkLabel->hide();
    _browserButton->hide();
    showMessage(QString(), false);
    _controller->start(_urlEdit->text());
}

void ServerAddressDialog::setBusy(bool busy)
{
    _urlEdit->setEnabled(!busy);
    _startButton->setEnabled(!busy);
    if (!busy) {
        _linkLabel->hide();
        _browserButton->hide();
    }
}

void ServerAddressDialog::showMessage(const QString &message, bool isError)
{
    _messageLabel->setText(message);
    QFont font = _messageLabel->font();
    font.setBold(isError);
    _messageLabel->setFont(font);
    _messageLabel->setVisible(!message.isEmpty());
}

}
