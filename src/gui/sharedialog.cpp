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
#include "sharedialog.h"

#include "accountstate.h"
#include "guiutility.h"
#include "libsync/account.h"
#include "libsync/capabilities.h"
#include "libsync/theme.h"
#include "mainwindow/colormanager.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDateEdit>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QLoggingCategory>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace OCC {

Q_LOGGING_CATEGORY(lcShareDialog, "gui.sharedialog", QtInfoMsg)

namespace {
    // die Link-Rollen der oC10-Weboberfläche
    constexpr SharePermissions linkReadOnly = SharePermission::Read;
    constexpr SharePermissions linkUpload = SharePermission::Read | SharePermission::Create;
    constexpr SharePermissions linkEdit = SharePermission::Read | SharePermission::Update | SharePermission::Create | SharePermission::Delete;
    constexpr SharePermissions linkUploadOnly = SharePermission::Create;

    SharePermissions linkRole(SharePermissions permissions)
    {
        if (permissions == linkUploadOnly)
            return linkUploadOnly;
        if (permissions.testFlag(SharePermission::Update) || permissions.testFlag(SharePermission::Delete))
            return linkEdit;
        if (permissions.testFlag(SharePermission::Create))
            return linkUpload;
        return linkReadOnly;
    }

    void clearLayout(QLayout *layout)
    {
        while (QLayoutItem *item = layout->takeAt(0)) {
            if (QWidget *widget = item->widget()) {
                widget->hide();
                widget->deleteLater();
            }
            delete item;
        }
    }

    QScrollArea *createScrollList(QWidget *parent, QVBoxLayout **layout)
    {
        auto *host = new QWidget(parent);
        *layout = new QVBoxLayout(host);
        (*layout)->setContentsMargins(0, 0, 0, 0);
        auto *scroll = new QScrollArea(parent);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(host);
        scroll->setMinimumHeight(100);
        return scroll;
    }

    QFrame *createRow(QWidget *parent)
    {
        auto *row = new QFrame(parent);
        row->setFrameShape(QFrame::StyledPanel);
        return row;
    }
}

ShareDialog::ShareDialog(
    AccountState *accountState, const QUrl &davUrl, const QString &remotePath, const QString &localPath, ShareDialogStartPage startPage, QWidget *parent)
    : QDialog(parent)
    , _accountState(accountState)
    , _remotePath(remotePath.startsWith(QLatin1Char('/')) ? remotePath : QLatin1Char('/') + remotePath)
    , _itemName(QFileInfo(localPath.isEmpty() ? remotePath : localPath).fileName())
{
    const QString &name = _itemName;
    setWindowTitle(tr("Share %1").arg(name));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(680, 620);

    // vorläufig aus der lokalen Kopie; die PROPFIND-Antwort des Servers ist maßgeblich
    _isFolder = QFileInfo(localPath).isDir();

    _api = new ShareApi(account(), this);
    connect(_api, &ShareApi::itemInfoFetched, this, &ShareDialog::onItemInfo);
    connect(_api, &ShareApi::sharesFetched, this, &ShareDialog::onSharesFetched);
    connect(_api, &ShareApi::shareCreated, this, &ShareDialog::onShareCreated);
    connect(_api, &ShareApi::shareUpdated, this, &ShareDialog::onShareChanged);
    connect(_api, &ShareApi::shareDeleted, this, &ShareDialog::onShareChanged);
    connect(_api, &ShareApi::shareesFetched, this, &ShareDialog::onShareesFetched);
    connect(_api, &ShareApi::requestFailed, this, &ShareDialog::onRequestFailed);

    auto *mainLayout = new QVBoxLayout(this);
    auto *header = new QLabel(tr("Share <b>%1</b>").arg(name.toHtmlEscaped()), this);
    header->setTextFormat(Qt::RichText);
    mainLayout->addWidget(header);

    _tabs = new QTabWidget(this);
    _tabs->addTab(buildUserTab(), tr("Users and groups"));
    if (account() && account()->capabilities().sharePublicLink()) {
        QWidget *linkTab = buildLinkTab();
        _tabs->addTab(linkTab, tr("Public links"));
        if (startPage == ShareDialogStartPage::PublicLinks)
            _tabs->setCurrentWidget(linkTab);
    }
    mainLayout->addWidget(_tabs, 1);

    _statusLabel = new QLabel(this);
    _statusLabel->setWordWrap(true);
    _statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _statusLabel->setVisible(false);
    mainLayout->addWidget(_statusLabel);

    auto *closeButton = new QPushButton(tr("Close"), this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::close);
    auto *bottomRow = new QHBoxLayout;
    bottomRow->addStretch(1);
    bottomRow->addWidget(closeButton);
    mainLayout->addLayout(bottomRow);

    // Enter in einem Eingabefeld darf nie still "Schließen" auslösen; die Felder reagieren selbst
    for (auto *button : findChildren<QPushButton *>())
        button->setAutoDefault(false);

    _searchTimer.setSingleShot(true);
    _searchTimer.setInterval(400);
    connect(&_searchTimer, &QTimer::timeout, this, &ShareDialog::searchSharees);

    resetLinkEditor();
    rebuildUserShares();
    rebuildLinkShares();

    if (!account() || !_accountState->isConnected()) {
        _tabs->setEnabled(false);
        showStatus(tr("Not connected to the server. Please check the connection and reopen the dialog."), true);
        return;
    }
    _api->fetchItemInfo(davUrl, _remotePath);
    reloadShares();
}

Account *ShareDialog::account() const
{
    return _accountState ? _accountState->account() : nullptr;
}

const Share *ShareDialog::findShare(const QString &shareId) const
{
    for (const auto &share : _shares) {
        if (share.id == shareId)
            return &share;
    }
    return nullptr;
}

QWidget *ShareDialog::buildUserTab()
{
    auto *tab = new QWidget(this);
    auto *layout = new QVBoxLayout(tab);

    auto *searchLabel = new QLabel(tr("Share with users or groups:"), tab);
    _shareeSearch = new QLineEdit(tab);
    _shareeSearch->setPlaceholderText(tr("Name of a user or group …"));
    _shareeSearch->setClearButtonEnabled(true);
    searchLabel->setBuddy(_shareeSearch);
    layout->addWidget(searchLabel);
    layout->addWidget(_shareeSearch);

    _shareeResults = new QListWidget(tab);
    _shareeResults->setAccessibleName(tr("Matching users and groups"));
    _shareeResults->setMaximumHeight(160);
    _shareeResults->setVisible(false);
    layout->addWidget(_shareeResults);

    _shareeHint = new QLabel(tab);
    _shareeHint->setWordWrap(true);
    _shareeHint->setVisible(false);
    layout->addWidget(_shareeHint);

    connect(_shareeSearch, &QLineEdit::textEdited, this, [this] { _searchTimer.start(); });
    connect(_shareeSearch, &QLineEdit::returnPressed, this, [this] {
        // Enter übernimmt nur einen eindeutigen oder ausgewählten Treffer, sonst wird sofort gesucht
        if (_shareeResults->isVisible() && (_shareeResults->count() == 1 || _shareeResults->currentItem())) {
            onShareeChosen(_shareeResults->currentItem() ? _shareeResults->currentItem() : _shareeResults->item(0));
            return;
        }
        _searchTimer.stop();
        searchSharees();
    });
    connect(_shareeResults, &QListWidget::itemClicked, this, &ShareDialog::onShareeChosen);
    connect(_shareeResults, &QListWidget::itemActivated, this, &ShareDialog::onShareeChosen);

    layout->addWidget(new QLabel(tr("Shared with:"), tab));
    layout->addWidget(createScrollList(tab, &_userSharesLayout), 1);
    return tab;
}

QWidget *ShareDialog::buildLinkTab()
{
    auto *tab = new QWidget(this);
    auto *layout = new QVBoxLayout(tab);
    layout->addWidget(createScrollList(tab, &_linkSharesLayout), 1);

    _linkEditor = new QGroupBox(tab);
    auto *form = new QFormLayout(_linkEditor);

    _linkName = new QLineEdit(_linkEditor);
    form->addRow(tr("Name:"), _linkName);

    _linkRoleBox = new QWidget(_linkEditor);
    auto *roleLayout = new QVBoxLayout(_linkRoleBox);
    roleLayout->setContentsMargins(0, 0, 0, 0);
    _roleReadOnly = new QRadioButton(tr("Download / View"), _linkRoleBox);
    _roleUpload = new QRadioButton(tr("Download / View / Upload"), _linkRoleBox);
    _roleEdit = new QRadioButton(tr("Download / View / Edit"), _linkRoleBox);
    _roleUploadOnly = new QRadioButton(tr("Upload only (File Drop)"), _linkRoleBox);
    for (auto *radio : {_roleReadOnly, _roleUpload, _roleEdit, _roleUploadOnly}) {
        roleLayout->addWidget(radio);
        connect(radio, &QRadioButton::toggled, this, [this](bool checked) {
            if (checked)
                updateLinkPolicy();
        });
    }
    auto *roleLabel = new QLabel(tr("Recipients can:"), _linkEditor);
    roleLabel->setBuddy(_roleReadOnly);
    form->addRow(roleLabel, _linkRoleBox);

    _linkPasswordCheck = new QCheckBox(tr("Protect with a password"), _linkEditor);
    _linkPassword = new QLineEdit(_linkEditor);
    _linkPassword->setEchoMode(QLineEdit::Password);
    _linkPassword->setAccessibleName(tr("Password"));
    auto *passwordRow = new QHBoxLayout;
    passwordRow->addWidget(_linkPasswordCheck);
    passwordRow->addWidget(_linkPassword, 1);
    form->addRow(passwordRow);
    connect(_linkPasswordCheck, &QCheckBox::toggled, this, &ShareDialog::updateLinkPolicy);

    _linkExpireCheck = new QCheckBox(tr("Set an expiration date"), _linkEditor);
    _linkExpireDate = new QDateEdit(_linkEditor);
    _linkExpireDate->setCalendarPopup(true);
    _linkExpireDate->setAccessibleName(tr("Expiration date"));
    auto *expireRow = new QHBoxLayout;
    expireRow->addWidget(_linkExpireCheck);
    expireRow->addWidget(_linkExpireDate, 1);
    form->addRow(expireRow);
    connect(_linkExpireCheck, &QCheckBox::toggled, this, &ShareDialog::updateLinkPolicy);

    _linkCancelButton = new QPushButton(tr("Cancel"), _linkEditor);
    connect(_linkCancelButton, &QPushButton::clicked, this, &ShareDialog::resetLinkEditor);
    _linkSaveButton = new QPushButton(_linkEditor);
    connect(_linkSaveButton, &QPushButton::clicked, this, &ShareDialog::saveLink);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->addStretch(1);
    buttonRow->addWidget(_linkCancelButton);
    buttonRow->addWidget(_linkSaveButton);
    form->addRow(buttonRow);

    connect(_linkName, &QLineEdit::returnPressed, this, &ShareDialog::saveLink);
    connect(_linkPassword, &QLineEdit::returnPressed, this, &ShareDialog::saveLink);
    layout->addWidget(_linkEditor);

    // außerhalb des Formulars, damit der Hinweis auch bei gesperrtem Formular lesbar bleibt
    _linkPolicyHint = new QLabel(tab);
    _linkPolicyHint->setWordWrap(true);
    _linkPolicyHint->setVisible(false);
    layout->addWidget(_linkPolicyHint);
    return tab;
}

void ShareDialog::onItemInfo(bool isFolder, SharePermissions maxPermissions)
{
    _isFolder = isFolder;
    _maxPermissions = maxPermissions;
    const bool mayShare = maxPermissions.testFlag(SharePermission::Share);
    _shareeSearch->setEnabled(mayShare);
    if (!mayShare)
        showStatus(tr("You are not allowed to share this item."), true);
    rebuildUserShares();
    rebuildLinkShares();
    updateLinkPolicy();
}

void ShareDialog::onSharesFetched(const QList<Share> &shares)
{
    _shares = shares;
    _sharesLoaded = true;
    if (!_editingLinkId.isEmpty() && !findShare(_editingLinkId))
        resetLinkEditor();
    rebuildUserShares();
    rebuildLinkShares();
    updateLinkPolicy();
}

void ShareDialog::onShareCreated(const Share &share)
{
    const Pending pending = std::exchange(_pending, Pending::None);
    setBusy(false);
    if (pending == Pending::CreateLink) {
        resetLinkEditor();
        if (!share.url.isEmpty())
            copyToClipboard(share.url);
        else
            showStatus(tr("Public link created."), false);
    } else {
        showStatus(tr("Shared with %1.").arg(_pendingShareeText), false);
        _shareeHint->setVisible(false);
        _shareeSearch->setFocus();
    }
    reloadShares();
}

void ShareDialog::onShareChanged()
{
    const Pending pending = std::exchange(_pending, Pending::None);
    setBusy(false);
    switch (pending) {
    case Pending::UpdateLink:
        resetLinkEditor();
        showStatus(tr("Public link saved."), false);
        break;
    case Pending::DeleteLink:
        showStatus(tr("Public link deleted."), false);
        break;
    case Pending::DeleteUserShare:
        showStatus(tr("Share removed."), false);
        break;
    default:
        break;
    }
    reloadShares();
}

void ShareDialog::onShareesFetched(const QString &search, const QList<Sharee> &sharees)
{
    // eine Antwort auf einen inzwischen geänderten Suchbegriff verwerfen
    if (search != _shareeSearch->text().trimmed())
        return;
    const QString self = account() ? account()->davUser() : QString();
    _sharees.clear();
    for (const auto &sharee : sharees) {
        if (sharee.type == Share::User && sharee.shareWith == self)
            continue;
        const bool alreadyShared = std::any_of(_shares.cbegin(), _shares.cend(),
            [&sharee](const Share &share) { return share.type == sharee.type && share.shareWith == sharee.shareWith; });
        if (!alreadyShared)
            _sharees.append(sharee);
    }

    _shareeResults->clear();
    for (int i = 0; i < _sharees.size(); ++i) {
        auto *item = new QListWidgetItem(shareeText(_sharees.at(i).type, _sharees.at(i).displayName, _sharees.at(i).additionalInfo), _shareeResults);
        item->setData(Qt::UserRole, i);
    }
    _shareeResults->setVisible(!_sharees.isEmpty());
    _shareeHint->setText(_sharees.isEmpty() ? tr("No users or groups found.") : tr("Select a user or group to share with."));
    _shareeHint->setVisible(true);
}

void ShareDialog::onRequestFailed(ShareApi::Operation operation, const QString &serverMessage, const QString &)
{
    const auto withReason = [&serverMessage](const QString &message) {
        return serverMessage.isEmpty() ? message : message + QLatin1Char('\n') + tr("Server message: %1").arg(serverMessage);
    };
    switch (operation) {
    case ShareApi::Operation::ItemInfo:
        // ohne share-permissions entscheidet der Server beim Anlegen; die Meldung kommt dann von dort
        qCInfo(lcShareDialog) << "item info unavailable for" << _remotePath << serverMessage;
        return;
    case ShareApi::Operation::Search:
        _shareeResults->setVisible(false);
        _shareeHint->setText(withReason(tr("Searching for users and groups failed.")));
        _shareeHint->setVisible(true);
        return;
    case ShareApi::Operation::Fetch:
        showStatus(withReason(tr("Could not load the existing shares.")), true);
        return;
    default:
        break;
    }

    const Pending pending = std::exchange(_pending, Pending::None);
    setBusy(false);
    switch (pending) {
    case Pending::CreateLink:
        showStatus(withReason(tr("Creating the public link failed.")), true);
        break;
    case Pending::CreateUserShare:
        showStatus(withReason(tr("Sharing with %1 failed.").arg(_pendingShareeText)), true);
        break;
    case Pending::UpdateLink:
        showStatus(withReason(tr("Saving the public link failed.")), true);
        break;
    case Pending::UpdateUserShare:
        showStatus(withReason(tr("Changing the permissions failed.")), true);
        // Kästchen auf den Stand des Servers zurücksetzen
        rebuildUserShares();
        break;
    case Pending::DeleteLink:
        showStatus(withReason(tr("Deleting the public link failed.")), true);
        break;
    case Pending::DeleteUserShare:
        showStatus(withReason(tr("Removing the share failed.")), true);
        break;
    case Pending::None:
        break;
    }
}

void ShareDialog::reloadShares()
{
    _api->fetchShares(_remotePath);
}

void ShareDialog::searchSharees()
{
    const QString search = _shareeSearch->text().trimmed();
    if (search.isEmpty()) {
        hideShareeResults();
        _shareeHint->setVisible(false);
        return;
    }
    const int minLength = account() ? account()->capabilities().shareSearchMinLength() : 2;
    if (search.size() < minLength) {
        hideShareeResults();
        _shareeHint->setText(tr("Enter at least %1 characters to search.").arg(minLength));
        _shareeHint->setVisible(true);
        return;
    }
    _shareeHint->setText(tr("Searching …"));
    _shareeHint->setVisible(true);
    _api->searchSharees(search, _isFolder);
}

void ShareDialog::onShareeChosen(QListWidgetItem *item)
{
    // ein Doppelklick meldet Klick und Aktivierung: nur eine Freigabe anlegen
    if (!item || _busy || !account())
        return;
    const int index = item->data(Qt::UserRole).toInt();
    if (index < 0 || index >= _sharees.size())
        return;
    const Sharee sharee = _sharees.at(index);
    _pendingShareeText = shareeText(sharee.type, sharee.displayName);
    _pending = Pending::CreateUserShare;
    setBusy(true);
    showStatus(tr("Sharing with %1 …").arg(_pendingShareeText), false);
    _api->createShare(_remotePath, sharee, ShareApi::defaultUserSharePermissions(account()->capabilities(), _isFolder, _maxPermissions));
    // die Liste erst nach diesem Signal leeren: das Element gehört ihr
    QTimer::singleShot(0, this, [this] {
        _shareeSearch->clear();
        hideShareeResults();
    });
}

void ShareDialog::hideShareeResults()
{
    _shareeResults->clear();
    _shareeResults->setVisible(false);
    _sharees.clear();
}

void ShareDialog::rebuildUserShares()
{
    clearLayout(_userSharesLayout);
    QList<Share> userShares;
    for (const auto &share : std::as_const(_shares)) {
        if (!share.isLink())
            userShares.append(share);
    }
    if (userShares.isEmpty()) {
        _userSharesLayout->addWidget(new QLabel(_sharesLoaded ? tr("Not shared with any user or group yet.") : tr("Loading shares …")));
        _userSharesLayout->addStretch(1);
        return;
    }

    const bool resharing = account() && account()->capabilities().shareResharing();
    for (const Share &share : std::as_const(userShares)) {
        const QString name = shareeText(share.type, share.shareWithDisplayName.isEmpty() ? share.shareWith : share.shareWithDisplayName);
        auto *row = createRow(_userSharesLayout->parentWidget());
        auto *rowLayout = new QVBoxLayout(row);

        auto *top = new QHBoxLayout;
        auto *nameLabel = new QLabel(name, row);
        nameLabel->setTextFormat(Qt::PlainText);
        top->addWidget(nameLabel, 1);
        auto *removeButton = new QToolButton(row);
        removeButton->setText(tr("Remove"));
        removeButton->setAccessibleName(tr("Stop sharing with %1").arg(name));
        connect(removeButton, &QToolButton::clicked, this, [this, share] { confirmAndDelete(share); });
        top->addWidget(removeButton);
        rowLayout->addLayout(top);

        auto *permissionRow = new QHBoxLayout;
        auto *viewBox = new QCheckBox(tr("can view"), row);
        viewBox->setChecked(true);
        viewBox->setEnabled(false);
        permissionRow->addWidget(viewBox);
        const auto addPermission = [&](SharePermission permission, const QString &text) {
            auto *box = new QCheckBox(text, row);
            const bool granted = share.permissions.testFlag(permission);
            box->setChecked(granted);
            // mehr als die eigenen Rechte lässt der Server nicht zu; Vorhandenes bleibt abwählbar
            box->setEnabled(granted || _maxPermissions.testFlag(permission));
            box->setAccessibleDescription(name);
            connect(box, &QCheckBox::toggled, this, [this, share, permission](bool enabled) { changeUserSharePermission(share, permission, enabled); });
            permissionRow->addWidget(box);
        };
        addPermission(SharePermission::Update, tr("can edit"));
        if (share.isFolder || _isFolder) {
            addPermission(SharePermission::Create, tr("can create"));
            addPermission(SharePermission::Delete, tr("can delete"));
        }
        if (resharing || share.permissions.testFlag(SharePermission::Share))
            addPermission(SharePermission::Share, tr("can share"));
        permissionRow->addStretch(1);
        rowLayout->addLayout(permissionRow);
        _userSharesLayout->addWidget(row);
    }
    _userSharesLayout->addStretch(1);
}

void ShareDialog::rebuildLinkShares()
{
    if (!_linkSharesLayout)
        return;
    clearLayout(_linkSharesLayout);
    QList<Share> links;
    for (const auto &share : std::as_const(_shares)) {
        if (share.isLink())
            links.append(share);
    }
    if (links.isEmpty()) {
        _linkSharesLayout->addWidget(new QLabel(_sharesLoaded ? tr("No public links yet.") : tr("Loading shares …")));
        _linkSharesLayout->addStretch(1);
        return;
    }

    for (const Share &share : std::as_const(links)) {
        const QString title = share.name.isEmpty() ? tr("Public link") : share.name;
        QStringList details;
        if (share.isFolder || _isFolder)
            details.append(linkRoleText(share.permissions));
        if (share.hasPassword)
            details.append(tr("password protected"));
        if (share.expireDate.isValid())
            details.append(tr("expires %1").arg(QLocale().toString(share.expireDate, QLocale::ShortFormat)));

        auto *row = createRow(_linkSharesLayout->parentWidget());
        auto *rowLayout = new QVBoxLayout(row);
        auto *titleLabel = new QLabel(details.isEmpty()
                ? QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped())
                : QStringLiteral("<b>%1</b> <i>(%2)</i>").arg(title.toHtmlEscaped(), details.join(QStringLiteral(", ")).toHtmlEscaped()),
            row);
        titleLabel->setTextFormat(Qt::RichText);
        titleLabel->setWordWrap(true);
        rowLayout->addWidget(titleLabel);

        auto *bottom = new QHBoxLayout;
        auto *urlEdit = new QLineEdit(share.url, row);
        urlEdit->setReadOnly(true);
        urlEdit->setAccessibleName(tr("Link address of %1").arg(title));
        urlEdit->setCursorPosition(0);
        bottom->addWidget(urlEdit, 1);

        auto *copyButton = new QToolButton(row);
        copyButton->setText(tr("Copy link"));
        copyButton->setAccessibleDescription(title);
        connect(copyButton, &QToolButton::clicked, this, [this, url = share.url] { copyToClipboard(url); });
        bottom->addWidget(copyButton);

        // weitere Wege, den Link weiterzugeben (wie im Client 6.0.3); InstantPopup, damit die Tastatur das Menü öffnet
        auto *moreButton = new QToolButton(row);
        moreButton->setText(tr("More"));
        moreButton->setAccessibleDescription(title);
        moreButton->setPopupMode(QToolButton::InstantPopup);
        moreButton->setMenu(createLinkMenu(share, moreButton));
        moreButton->setEnabled(!share.url.isEmpty());
        bottom->addWidget(moreButton);

        auto *editButton = new QToolButton(row);
        editButton->setText(tr("Edit"));
        editButton->setAccessibleDescription(title);
        connect(editButton, &QToolButton::clicked, this, [this, share] { startEditingLink(share); });
        bottom->addWidget(editButton);

        auto *deleteButton = new QToolButton(row);
        deleteButton->setText(tr("Delete"));
        deleteButton->setAccessibleDescription(title);
        connect(deleteButton, &QToolButton::clicked, this, [this, share] { confirmAndDelete(share); });
        bottom->addWidget(deleteButton);

        rowLayout->addLayout(bottom);
        _linkSharesLayout->addWidget(row);
    }
    _linkSharesLayout->addStretch(1);
}

void ShareDialog::changeUserSharePermission(const Share &share, SharePermission permission, bool enabled)
{
    if (_busy)
        return;
    SharePermissions wanted = share.permissions;
    wanted.setFlag(permission, enabled);
    wanted |= SharePermission::Read;
    if (wanted == share.permissions)
        return;
    _pending = Pending::UpdateUserShare;
    setBusy(true);
    _api->updateShare(share.id, {{QStringLiteral("permissions"), QString::number(wanted.toInt())}});
}

void ShareDialog::confirmAndDelete(const Share &share)
{
    if (_busy)
        return;
    const QString question = share.isLink()
        ? tr("Delete the public link \"%1\"? Everyone who has the link loses access.").arg(share.name.isEmpty() ? tr("Public link") : share.name)
        : tr("Stop sharing with %1?").arg(shareeText(share.type, share.shareWithDisplayName.isEmpty() ? share.shareWith : share.shareWithDisplayName));
    // nicht blockierend: schließt jemand den Dialog währenddessen, verschwindet die Rückfrage mit ihm
    auto *box = new QMessageBox(QMessageBox::Question, windowTitle(), question, QMessageBox::Yes | QMessageBox::No, this);
    box->setDefaultButton(QMessageBox::No);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QMessageBox::buttonClicked, this, [this, box, share](QAbstractButton *button) {
        if (box->standardButton(button) != QMessageBox::Yes || _busy)
            return;
        _pending = share.isLink() ? Pending::DeleteLink : Pending::DeleteUserShare;
        setBusy(true);
        _api->deleteShare(share.id);
    });
    box->open();
}

void ShareDialog::startEditingLink(const Share &share)
{
    if (!_linkEditor || _busy)
        return;
    _editingLinkId = share.id;
    _linkEditor->setTitle(tr("Edit public link"));
    _linkName->setText(share.name);
    const SharePermissions role = linkRole(share.permissions);
    _roleReadOnly->setChecked(role == linkReadOnly);
    _roleUpload->setChecked(role == linkUpload);
    _roleEdit->setChecked(role == linkEdit);
    _roleUploadOnly->setChecked(role == linkUploadOnly);
    _linkPasswordCheck->setChecked(share.hasPassword);
    _linkPassword->clear();
    _linkExpireCheck->setChecked(share.expireDate.isValid());
    if (share.expireDate.isValid())
        _linkExpireDate->setDate(share.expireDate);
    _linkSaveButton->setText(tr("Save"));
    _linkCancelButton->setVisible(true);
    updateLinkPolicy();
    _linkName->setFocus();
}

void ShareDialog::resetLinkEditor()
{
    if (!_linkEditor)
        return;
    _editingLinkId.clear();
    _linkEditor->setTitle(tr("New public link"));
    _linkName->setText(tr("Public link"));
    _roleReadOnly->setChecked(true);
    _linkPasswordCheck->setChecked(false);
    _linkPassword->clear();

    const QDate today = QDate::currentDate();
    // der Server lehnt ein Ablaufdatum heute oder früher ab
    _linkExpireDate->setMinimumDate(today.addDays(1));
    const auto *acc = account();
    const int days = acc ? acc->capabilities().sharePublicLinkDefaultExpireDateDays() : 0;
    _linkExpireDate->setDate(today.addDays(days > 0 ? days : 7));
    _linkExpireCheck->setChecked(acc && (acc->capabilities().sharePublicLinkDefaultExpire() || acc->capabilities().sharePublicLinkEnforceExpireDate()));

    _linkSaveButton->setText(tr("Create public link"));
    _linkCancelButton->setVisible(false);
    updateLinkPolicy();
}

void ShareDialog::updateLinkPolicy()
{
    if (!_linkEditor || !account())
        return;
    const auto &capabilities = account()->capabilities();
    const bool editing = !_editingLinkId.isEmpty();
    const Share *current = editing ? findShare(_editingLinkId) : nullptr;

    // Rollen gibt es nur für Ordner, Hochladen nur mit Erlaubnis des Servers und eigenem Schreibrecht
    const bool upload = _isFolder && capabilities.sharePublicLinkAllowUpload();
    _linkRoleBox->setVisible(_isFolder);
    _roleUpload->setEnabled(upload && _maxPermissions.testFlag(SharePermission::Create));
    _roleEdit->setEnabled(upload && (_maxPermissions & linkEdit) == linkEdit);
    _roleUploadOnly->setVisible(capabilities.sharePublicLinkSupportsUploadOnly());
    _roleUploadOnly->setEnabled(upload && _maxPermissions.testFlag(SharePermission::Create));
    if (!editing && !_isFolder && !_roleReadOnly->isChecked())
        _roleReadOnly->setChecked(true);

    const bool passwordEnforced = ShareApi::linkPasswordEnforced(capabilities, selectedLinkPermissions());
    if (passwordEnforced && !_linkPasswordCheck->isChecked()) {
        const QSignalBlocker blocker(_linkPasswordCheck);
        _linkPasswordCheck->setChecked(true);
    }
    _linkPasswordCheck->setEnabled(!passwordEnforced);
    _linkPassword->setEnabled(_linkPasswordCheck->isChecked());
    _linkPassword->setPlaceholderText(current && current->hasPassword ? tr("Leave empty to keep the current password") : QString());

    const bool expireEnforced = capabilities.sharePublicLinkEnforceExpireDate();
    const int days = capabilities.sharePublicLinkDefaultExpireDateDays();
    if (expireEnforced && !_linkExpireCheck->isChecked()) {
        const QSignalBlocker blocker(_linkExpireCheck);
        _linkExpireCheck->setChecked(true);
    }
    _linkExpireCheck->setEnabled(!expireEnforced);
    _linkExpireDate->setEnabled(_linkExpireCheck->isChecked());
    if (expireEnforced && days > 0)
        _linkExpireDate->setMaximumDate(QDate::currentDate().addDays(days));
    else
        _linkExpireDate->clearMaximumDate();

    int linkCount = 0;
    for (const auto &share : std::as_const(_shares))
        linkCount += share.isLink() ? 1 : 0;
    const bool onlyOneLink = !editing && !capabilities.sharePublicLinkMultiple() && linkCount > 0;
    const bool mayShare = _maxPermissions.testFlag(SharePermission::Share);

    QStringList hints;
    if (passwordEnforced)
        hints.append(tr("The server requires a password for this kind of link."));
    if (expireEnforced)
        hints.append(days > 0 ? tr("The server requires an expiration date at most %1 days ahead.").arg(days) : tr("The server requires an expiration date."));
    if (onlyOneLink)
        hints.append(tr("The server allows only one public link per item."));
    _linkPolicyHint->setText(hints.join(QLatin1Char('\n')));
    _linkPolicyHint->setVisible(!hints.isEmpty());
    _linkEditor->setEnabled(mayShare && !onlyOneLink);
}

void ShareDialog::saveLink()
{
    if (_busy || !_linkEditor || !_linkEditor->isEnabled())
        return;
    const Share *current = nullptr;
    if (!_editingLinkId.isEmpty()) {
        current = findShare(_editingLinkId);
        if (!current) {
            resetLinkEditor();
            return;
        }
    }

    LinkSettings settings;
    settings.name = _linkName->text().trimmed();
    settings.permissions = selectedLinkPermissions();
    // unveränderte Rolle: die Rechte des Servers unangetastet lassen
    if (current && linkRole(current->permissions) == settings.permissions)
        settings.permissions = current->permissions;
    settings.passwordProtected = _linkPasswordCheck->isChecked();
    settings.password = _linkPassword->text();
    settings.expireDate = _linkExpireCheck->isChecked() ? _linkExpireDate->date() : QDate();

    if (settings.passwordProtected && settings.password.isEmpty() && !(current && current->hasPassword)) {
        showStatus(tr("Please enter a password, or uncheck the password option."), true);
        _linkPassword->setFocus();
        return;
    }

    if (current) {
        const auto changes = ShareApi::linkUpdateArguments(*current, settings);
        if (changes.isEmpty()) {
            resetLinkEditor();
            return;
        }
        _pending = Pending::UpdateLink;
        setBusy(true);
        _api->updateShare(current->id, changes);
    } else {
        _pending = Pending::CreateLink;
        setBusy(true);
        _api->createLinkShare(_remotePath, settings);
    }
}

SharePermissions ShareDialog::selectedLinkPermissions() const
{
    if (!_isFolder)
        return linkReadOnly;
    if (_roleUpload->isChecked())
        return linkUpload;
    if (_roleEdit->isChecked())
        return linkEdit;
    if (_roleUploadOnly->isChecked())
        return linkUploadOnly;
    return linkReadOnly;
}

QString ShareDialog::linkRoleText(SharePermissions permissions) const
{
    const SharePermissions role = linkRole(permissions);
    if (role == linkUploadOnly)
        return tr("Upload only (File Drop)");
    if (role == linkEdit)
        return tr("Download / View / Edit");
    if (role == linkUpload)
        return tr("Download / View / Upload");
    return tr("Download / View");
}

QString ShareDialog::shareeText(int type, const QString &displayName, const QString &additionalInfo) const
{
    const QString name = additionalInfo.isEmpty() ? displayName : tr("%1 (%2)", "sharee (additional info, e.g. the email address)").arg(displayName, additionalInfo);
    switch (type) {
    case Share::Group:
        return tr("%1 (group)").arg(name);
    case Share::Remote:
        return tr("%1 (federated)").arg(name);
    default:
        return name;
    }
}

void ShareDialog::setBusy(bool busy)
{
    _busy = busy;
    _tabs->setEnabled(!busy);
}

void ShareDialog::showStatus(const QString &message, bool isError)
{
    _statusLabel->setVisible(true);
    // Farben aus dem Theme passend zum Farbschema (mindestens 4,5:1); das frühere #00a98c erreichte auf Weiß nur 3:1
    const bool dark = ColorManager::isDarkPalette();
    const QColor color = isError ? Theme::instance()->errorColor(dark) : Theme::instance()->accentColor(dark);
    _statusLabel->setStyleSheet(color.isValid() ? QStringLiteral("color: %1;").arg(color.name()) : QString());
    _statusLabel->setText(message);
}

void ShareDialog::showError(const QString &message)
{
    showStatus(message, true);
}

void ShareDialog::copyToClipboard(const QString &url)
{
    QGuiApplication::clipboard()->setText(url);
    showStatus(tr("Link copied to the clipboard: %1").arg(url), false);
}

QMenu *ShareDialog::createLinkMenu(const Share &share, QWidget *parent)
{
    auto *menu = new QMenu(parent);
    const QString url = share.url;
    // die Adresse kommt vom Server: nur Webadressen öffnen, nie etwa file: über die Shell
    const QUrl webUrl(url);
    if (webUrl.scheme() == QLatin1String("https") || webUrl.scheme() == QLatin1String("http"))
        menu->addAction(tr("Open in browser"), this, [this, webUrl] { Utility::openBrowser(webUrl, this); });
    menu->addAction(tr("Copy link"), this, [this, url] { copyToClipboard(url); });
    // lädt die Datei bzw. den Ordner (als ZIP) ohne Umweg über die Linkseite herunter
    const QUrl directUrl = ShareApi::directDownloadUrl(share);
    if (directUrl.isValid())
        menu->addAction(tr("Copy direct download link"), this, [this, directUrl] { copyToClipboard(directUrl.toString()); });
    menu->addAction(tr("Send by email…"), this, [this, url] { Utility::openEmailComposer(tr("I shared %1 with you").arg(_itemName), url, this); });
    return menu;
}

} // namespace OCC
